#include "tashkeel/diacritizer.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tashkeel {

namespace {

constexpr size_t MAX_STEM_CANDS = 10;
constexpr size_t MAX_CANDS = 18;
constexpr int32_t NEG_INF = std::numeric_limits<int32_t>::min() / 4;

constexpr uint16_t BIT(uint8_t d) { return static_cast<uint16_t>(1u << d); }
constexpr uint16_t MASK_ALL = 0x7FFF;
constexpr uint16_t MASK_TAN = BIT(DIAC_FATHATAN) | BIT(DIAC_DAMMATAN) | BIT(DIAC_KASRATAN) |
                              BIT(DIAC_SHADDA_FATHATAN) | BIT(DIAC_SHADDA_DAMMATAN) | BIT(DIAC_SHADDA_KASRATAN);
constexpr uint16_t MASK_SH = BIT(DIAC_SHADDA) | BIT(DIAC_SHADDA_FATHA) | BIT(DIAC_SHADDA_DAMMA) |
                             BIT(DIAC_SHADDA_KASRA) | (MASK_TAN & (BIT(DIAC_SHADDA_FATHATAN) |
                             BIT(DIAC_SHADDA_DAMMATAN) | BIT(DIAC_SHADDA_KASRATAN)));
constexpr uint16_t MASK_FATHA_ONLY = BIT(DIAC_FATHA) | BIT(DIAC_SHADDA_FATHA);
constexpr uint16_t MASK_CASE = BIT(DIAC_FATHA) | BIT(DIAC_DAMMA) | BIT(DIAC_KASRA) | BIT(DIAC_FATHATAN) |
                               BIT(DIAC_DAMMATAN) | BIT(DIAC_KASRATAN) | BIT(DIAC_SHADDA_FATHA) |
                               BIT(DIAC_SHADDA_DAMMA) | BIT(DIAC_SHADDA_KASRA) | BIT(DIAC_SHADDA_FATHATAN) |
                               BIT(DIAC_SHADDA_DAMMATAN) | BIT(DIAC_SHADDA_KASRATAN);
constexpr uint16_t MASK_SH_VOWEL = BIT(DIAC_SHADDA_FATHA) | BIT(DIAC_SHADDA_DAMMA) | BIT(DIAC_SHADDA_KASRA);

struct SufRule {
    uint8_t len;
    uint8_t syms[3];
    uint8_t diacs[3];
    uint8_t mode;
};

const SufRule SUFFIXES[] = {
    {3, {S_HEH, S_MEEM, S_ALEF}, {DIAC_DAMMA, DIAC_FATHA, DIAC_NONE}, 1},
    {3, {S_KAF, S_MEEM, S_ALEF}, {DIAC_DAMMA, DIAC_FATHA, DIAC_NONE}, 0},
    {2, {S_HEH, S_MEEM, 0}, {DIAC_DAMMA, DIAC_SUKUN, 0}, 1},
    {2, {S_HEH, S_NOON, 0}, {DIAC_DAMMA, DIAC_SHADDA_FATHA, 0}, 1},
    {2, {S_KAF, S_MEEM, 0}, {DIAC_DAMMA, DIAC_SUKUN, 0}, 0},
    {2, {S_KAF, S_NOON, 0}, {DIAC_DAMMA, DIAC_SHADDA_FATHA, 0}, 0},
    {2, {S_HEH, S_ALEF, 0}, {DIAC_FATHA, DIAC_NONE, 0}, 0},
    {2, {S_NOON, S_ALEF, 0}, {DIAC_FATHA, DIAC_NONE, 0}, 0},
    {2, {S_NOON, S_YEH, 0}, {DIAC_KASRA, DIAC_NONE, 0}, 0},
    {1, {S_HEH, 0, 0}, {DIAC_DAMMA, 0, 0}, 1},
    {1, {S_KAF, 0, 0}, {DIAC_FATHA, 0, 0}, 0},
    {1, {S_YEH, 0, 0}, {DIAC_NONE, 0, 0}, 2},
};
constexpr size_t NUM_SUFFIXES = sizeof(SUFFIXES) / sizeof(SUFFIXES[0]);
constexpr size_t NO_SUFFIX = static_cast<size_t>(-1);

inline bool is_boundary_cp(uint32_t cp) {
    switch (cp) {
        case '.': case '!': case '?': case ';': case ':': case ',': case '\n': case '(': case ')':
        case '[': case ']': case '{': case '}': case '"':
        case 0x061F: case 0x061B: case 0x060C: case 0x00AB: case 0x00BB: case 0x06D4:
            return true;
        default:
            return false;
    }
}

inline bool is_imperf_prefix(uint8_t s) {
    return s == S_YEH || s == S_TEH || s == S_NOON || s == S_ALEF_HAMZA_A;
}

inline bool starts_like_imperf(const uint8_t* s, size_t n) {
    if (n < 3) return false;
    size_t st = 0;
    if (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH)) st = 1;
    if (n - st >= 4 && s[st] == S_SEEN && is_imperf_prefix(s[st + 1])) st += 1;
    return is_imperf_prefix(s[st]);
}

inline bool is_man_word(const uint8_t* s, size_t n) {
    return (n == 2 && s[0] == S_MEEM && s[1] == S_NOON) ||
           (n == 3 && (s[0] == S_WAW || s[0] == S_FEH) && s[1] == S_MEEM && s[2] == S_NOON);
}

inline bool is_ibn_word(const uint8_t* s, size_t n) {
    size_t st = 0;
    if (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH)) st = 1;
    const size_t len = n - st;
    const uint8_t* w = s + st;
    return (len == 2 && w[0] == S_BEH && w[1] == S_NOON) ||
           (len == 3 && (w[0] == S_ALEF || w[0] == S_ALEF_WASLA) && w[1] == S_BEH && w[2] == S_NOON) ||
           (len == 3 && w[0] == S_BEH && w[1] == S_NOON && w[2] == S_TEH);
}

inline bool is_ashar_word(const uint8_t* s, size_t n) {
    return (n == 3 && s[0] == S_AIN && s[1] == S_SHEEN && s[2] == S_REH) ||
           (n == 4 && s[0] == S_AIN && s[1] == S_SHEEN && s[2] == S_REH && s[3] == S_TEH_MARBUTA);
}

inline bool is_compound_num_unit(const uint8_t* s, size_t n) {
    if (n < 2) return false;
    size_t st = 0;
    if (n >= 3 && (s[0] == S_BEH || s[0] == S_LAM || s[0] == S_KAF || s[0] == S_WAW || s[0] == S_FEH)) st = 1;
    if (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH) && (s[1] == S_BEH || s[1] == S_LAM || s[1] == S_KAF)) st = 2;
    size_t len = n - st;
    const uint8_t* w = s + st;
    if (len == 3 && w[0] == S_ALEF_HAMZA_A && w[1] == S_HAH && w[2] == S_DAL) return true; // أحد
    if (len == 4 && w[0] == S_ALEF_HAMZA_B && w[1] == S_HAH && w[2] == S_DAL && w[3] == S_ALEF_MAKSURA) return true; // إحدى
    if (len >= 4 && len <= 6 && w[len - 1] == S_TEH_MARBUTA &&
        (w[0] == S_THEH || w[0] == S_ALEF_HAMZA_A || w[0] == S_KHAH || w[0] == S_SEEN || w[0] == S_TEH)) {
        return true; // ثلاثة..تسعة
    }
    if (len == 2 && w[0] == S_SEEN && w[1] == S_TEH) return true; // ست
    if (len == 3 && w[0] == S_SEEN && w[1] == S_BEH && w[2] == S_AIN) return true; // سبع
    if (len == 3 && w[0] == S_TEH && w[1] == S_SEEN && w[2] == S_AIN) return true; // تسع
    if (len == 3 && w[0] == S_KHAH && w[1] == S_MEEM && w[2] == S_SEEN) return true; // خمس
    if (len == 4 && w[0] == S_ALEF_HAMZA_A && w[1] == S_REH && w[2] == S_BEH && w[3] == S_AIN) return true; // أربع
    if (len == 4 && w[0] == S_THEH && w[1] == S_LAM && w[2] == S_ALEF && w[3] == S_THEH) return true; // ثلاث
    if ((len == 4 || len == 5) && w[0] == S_THEH && w[1] == S_MEEM && w[2] == S_ALEF && w[3] == S_NOON &&
        (len == 4 || w[4] == S_YEH)) return true; // ثمان / ثماني
    return false;
}

inline bool is_an_prep_word(const uint8_t* s, size_t n) {
    size_t st = (n == 3 && (s[0] == S_WAW || s[0] == S_FEH)) ? 1 : 0;
    return n - st == 2 && s[st] == S_AIN && s[st + 1] == S_NOON;
}

inline bool is_bare_baad(const uint8_t* s, size_t n) {
    return n == 3 && s[0] == S_BEH && s[1] == S_AIN && s[2] == S_DAL;
}

inline bool is_name_head_word(const uint8_t* s, size_t n) {
    size_t st = 0;
    if (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM)) st = 1;
    const size_t len = n - st;
    const uint8_t* w = s + st;
    if (len == 3 && w[0] == S_AIN && w[1] == S_BEH && w[2] == S_DAL) return true;
    if (len == 3 && w[0] == S_ALEF_HAMZA_A && w[1] == S_BEH && (w[2] == S_WAW || w[2] == S_YEH || w[2] == S_ALEF)) return true;
    if (len == 2 && w[0] == S_ALEF_HAMZA_A && w[1] == S_MEEM) return true;
    return false;
}

inline bool is_five_noun_head(const uint8_t* s, size_t n) {
    size_t st = 0;
    if (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM)) st = 1;
    if (n >= 5 && (s[0] == S_WAW || s[0] == S_FEH) && (s[1] == S_BEH || s[1] == S_LAM)) st = 2;
    const size_t len = n - st;
    const uint8_t* w = s + st;
    if (len == 3 && w[0] == S_ALEF_HAMZA_A && (w[1] == S_BEH || w[1] == S_KHAH) &&
        (w[2] == S_WAW || w[2] == S_YEH || w[2] == S_ALEF)) return true;
    if (len == 2 && w[0] == S_THAL && (w[1] == S_WAW || w[1] == S_YEH || w[1] == S_ALEF)) return true;
    return false;
}

inline bool is_short_prep_pron(const uint8_t* s, size_t n) {
    if (n < 2 || n > 5) return false;
    return (s[0] == S_FEH && s[1] == S_YEH) ||
           (s[0] == S_BEH && (n <= 4)) ||
           (s[0] == S_LAM && (n <= 4)) ||
           (s[0] == S_MEEM && s[1] == S_NOON) ||
           (s[0] == S_AIN && s[1] == S_NOON) ||
           (s[0] == S_AIN && s[1] == S_LAM && s[2] == S_YEH);
}

inline bool is_inna_pron_word(const uint8_t* s, size_t n) {
    size_t st = 0;
    if (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH)) st = 1;
    if (n - st >= 4 && (s[st] == S_LAM || s[st] == S_BEH || s[st] == S_KAF)) ++st;
    const size_t len = n - st;
    const uint8_t* w = s + st;
    if (len == 3 && (w[0] == S_ALEF_HAMZA_A || w[0] == S_ALEF_HAMZA_B) && w[1] == S_NOON &&
        (w[2] == S_HEH || w[2] == S_KAF)) return true; // أنه/إنه/أنك
    if (len == 4 && (w[0] == S_ALEF_HAMZA_A || w[0] == S_ALEF_HAMZA_B) && w[1] == S_NOON &&
        ((w[2] == S_HEH && (w[3] == S_ALEF || w[3] == S_MEEM)) ||
         (w[2] == S_NOON && w[3] == S_ALEF) ||
         (w[2] == S_KAF && w[3] == S_MEEM))) return true; // أنها/أنهم/أننا/إنها/إنهم
    return false;
}

inline bool is_ams_word(const uint8_t* s, size_t n) {
    return n == 3 && (s[0] == S_ALEF_HAMZA_A || s[0] == S_ALEF) && s[1] == S_MEEM && s[2] == S_SEEN;
}

inline bool is_quant_idafa_word(const uint8_t* s, size_t n) {
    size_t st = (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM)) ? 1 : 0;
    if (n - st >= 5 && is_alef_or_wasla(s[st]) && s[st + 1] == S_LAM) st += 2;
    const size_t len = n - st;
    const uint8_t* w = s + st;
    if (len == 3 && w[0] == S_AIN && w[1] == S_DAL && w[2] == S_DAL) return true; // عدد
    if (len == 4 && w[0] == S_AIN && w[1] == S_DAL && w[2] == S_YEH && w[3] == S_DAL) return true; // عديد
    if (len == 4 && w[0] == S_KAF && w[1] == S_THEH && w[2] == S_YEH && w[3] == S_REH) return true; // كثير
    if (len == 4 && w[0] == S_QAF && w[1] == S_LAM && w[2] == S_YEH && w[3] == S_LAM) return true; // قليل
    if (len == 4 && w[0] == S_ALEF_HAMZA_A && w[1] == S_KAF && w[2] == S_THEH && w[3] == S_REH) return true; // أكثر
    if (len == 3 && w[0] == S_JEEM && w[1] == S_ZAIN && w[2] == S_HAMZA) return true; // جزء
    if (len == 3 && w[0] == S_NOON && w[1] == S_SAD && w[2] == S_FEH) return true; // نصف
    if (len == 6 && w[0] == S_MEEM && w[1] == S_JEEM && w[2] == S_MEEM && w[3] == S_WAW && w[4] == S_AIN && w[5] == S_TEH_MARBUTA) return true; // مجموعة
    return false;
}

inline bool starts_with_wasl(const uint8_t* s, size_t n, const SurfaceInfo& si) {
    if (n < 2) return false;
    if (si.al_len > 0 && !si.has_prep && !si.has_conj) return true;
    if (s[0] == S_ALEF_WASLA) return true;
    if (s[0] == S_ALEF && n >= 3) {
        if (s[1] == S_NOON || (n >= 4 && s[1] == S_SEEN && s[2] == S_TEH) ||
            (n >= 4 && s[2] == S_TEH && s[1] != S_ALEF) ||
            (s[1] == S_BEH && s[2] == S_NOON) || (s[1] == S_SEEN && s[2] == S_MEEM) ||
            (s[1] == S_MEEM && s[2] == S_REH) || (s[1] == S_THEH && s[2] == S_NOON)) {
            return true;
        }
    }
    return false;
}

inline bool is_plural_meem_pron(const uint8_t* s, size_t n) {
    return n >= 3 && s[n - 1] == S_MEEM && (s[n - 2] == S_HEH || s[n - 2] == S_KAF || s[n - 2] == S_TEH);
}

inline bool is_baad_qabl(const uint8_t* s, size_t n) {
    size_t st = (n == 4 && (s[0] == S_WAW || s[0] == S_FEH)) ? 1 : 0;
    if (n - st != 3) return false;
    const uint8_t* w = s + st;
    return (w[0] == S_BEH && w[1] == S_AIN && w[2] == S_DAL) ||
           (w[0] == S_QAF && w[1] == S_BEH && w[2] == S_LAM);
}

inline bool is_ali_word(const uint8_t* s, size_t n) {
    size_t st = (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM)) ? 1 : 0;
    if (n - st != 3) return false;
    return s[st] == S_AIN && s[st + 1] == S_LAM && (s[st + 2] == S_YEH || s[st + 2] == S_ALEF_MAKSURA);
}

inline bool is_ala_word(const uint8_t* s, size_t n) {
    size_t st = (n == 4 && (s[0] == S_WAW || s[0] == S_FEH)) ? 1 : 0;
    return (n - st == 3) && s[st] == S_AIN && s[st + 1] == S_LAM && s[st + 2] == S_ALEF_MAKSURA;
}

inline bool is_am_word(const uint8_t* s, size_t n) {
    return n == 2 && s[0] == S_ALEF_HAMZA_A && s[1] == S_MEEM;
}

inline bool is_themma_word(const uint8_t* s, size_t n) {
    return n == 2 && s[0] == S_THEH && s[1] == S_MEEM;
}

inline bool is_sound_fem_plural_stem(const uint8_t* s, size_t n, size_t al_len) {
    return n >= al_len + 5 && s[n - 1] == S_TEH && s[n - 2] == S_ALEF &&
           s[al_len] != S_ALEF_HAMZA_A && s[al_len] != S_ALEF;
}

inline bool is_defective_yeh_noun(const uint8_t* s, size_t n, size_t cp, const uint8_t* d, uint8_t verb_pat) {
    if (verb_pat == 1 || cp + 1 != n || n < 4 || s[cp] != S_YEH) return false;
    if (diac_has_shadda(d[cp])) return false;
    return diac_family(d[cp - 1]) == FAM_KASRA;
}

inline bool is_adj_surface(const uint8_t* s, size_t n, size_t al_len) {
    if (n <= al_len + 2) return false;
    const size_t sn = n - al_len;
    const uint8_t* w = s + al_len;
    if (sn >= 4 && w[sn - 1] == S_YEH && w[0] != S_YEH && w[0] != S_TEH && w[0] != S_NOON) return true;
    if (sn >= 5 && w[sn - 2] == S_YEH && w[sn - 1] == S_TEH_MARBUTA) return true;
    if (sn >= 5 && w[0] == S_MEEM &&
        (w[1] == S_TEH || w[1] == S_NOON || (w[1] == S_SEEN && w[2] == S_TEH) ||
         w[sn - 2] == S_WAW || (sn >= 6 && w[sn - 1] == S_TEH_MARBUTA && w[sn - 3] == S_WAW))) {
        return true;
    }
    if (sn == 4 && w[0] == S_ALEF_HAMZA_A && w[3] != S_ALEF && w[3] != S_ALEF_MAKSURA) return true;
    if ((sn == 4 && w[2] == S_YEH && w[0] != S_YEH && w[0] != S_TEH && w[0] != S_MEEM) ||
        (sn == 5 && w[2] == S_YEH && w[4] == S_TEH_MARBUTA && w[0] != S_MEEM)) {
        return true; // فَعِيل / فَعِيلَة (قَصِيرَة، جَدِيد، كَبِير)
    }
    return false;
}

// Returns 0=not diptote, 1=common/plural diptote (can take kasra if Mudaf), 2=proper/month diptote (never Mudaf)
inline uint8_t classify_diptote_surface(const uint8_t* s, size_t n, size_t st) {
    if (n <= st + 2) return 0;
    const size_t sn = n - st;
    const uint8_t* w = s + st;
    if (w[sn - 1] == S_TEH_MARBUTA) {
        if (sn == 3 && w[0] == S_MEEM && w[1] == S_KAF) return 2; // مكة
        if (sn == 3 && w[0] == S_JEEM && w[1] == S_DAL) return 2; // جدة
        if (sn == 3 && w[0] == S_GHAIN && w[1] == S_ZAIN) return 2; // غزة
        if (sn == 4 && w[0] == S_HAH && w[1] == S_MEEM && w[2] == S_ZAIN) return 2; // حمزة
        if (sn == 4 && w[0] == S_TAH && w[1] == S_LAM && w[2] == S_HAH) return 2; // طلحة
        if (sn == 4 && w[0] == S_AIN && w[1] == S_REH && w[2] == S_WAW) return 2; // عروة
        if (sn == 4 && w[0] == S_AIN && w[1] == S_REH && w[2] == S_FEH) return 2; // عرفة
        if (sn == 5 && w[0] == S_HAH && w[1] == S_THAL && w[2] == S_YEH && w[3] == S_FEH) return 2; // حذيفة
        if (sn == 5 && w[0] == S_REH && w[1] == S_BEH && w[2] == S_YEH && w[3] == S_AIN) return 2; // ربيعة
        if (sn == 5 && w[0] == S_QAF && w[1] == S_TEH && w[2] == S_ALEF && w[3] == S_DAL) return 2; // قتادة
        if (sn == 5 && w[0] == S_AIN && w[1] == S_KAF && w[2] == S_REH && w[3] == S_MEEM) return 2; // عكرمة
        if (sn == 5 && w[0] == S_MEEM && w[1] == S_SEEN && w[2] == S_LAM && w[3] == S_MEEM) return 2; // مسلمة
        if (sn == 5 && w[0] == S_FEH && w[1] == S_ALEF && w[2] == S_TAH && w[3] == S_MEEM) return 2; // فاطمة
        if (sn == 5 && w[0] == S_AIN && w[1] == S_ALEF && w[2] == S_YEH_HAMZA && w[3] == S_SHEEN) return 2; // عائشة
        if (sn == 5 && w[0] == S_KHAH && w[1] == S_DAL && w[2] == S_YEH && w[3] == S_JEEM) return 2; // خديجة
        if (sn == 5 && w[0] == S_ALEF_HAMZA_A && w[1] == S_SEEN && w[2] == S_ALEF && w[3] == S_MEEM) return 2; // أسامة
        if (sn == 6 && w[0] == S_MEEM && w[1] == S_AIN && w[2] == S_ALEF && w[3] == S_WAW && w[4] == S_YEH) return 2; // معاوية
        return 0;
    }
    if (sn == 5 && w[2] == S_ALEF && w[4] != S_ALEF && w[4] != S_ALEF_MAKSURA &&
        (w[3] == S_YEH_HAMZA || w[0] == S_MEEM || w[1] == S_WAW)) return 1;
    if (sn == 6 && w[2] == S_ALEF && w[4] == S_YEH && w[5] != S_YEH &&
        (w[0] == S_MEEM || w[0] == S_TEH || w[0] == S_ALEF_HAMZA_A)) return 1;
    if (sn >= 5 && w[sn - 2] == S_ALEF && w[sn - 1] == S_HAMZA &&
        w[0] != S_ALEF && w[0] != S_ALEF_HAMZA_B && w[sn - 3] != S_ALEF) return 1;
    // Proper / Foreign names & Gregorian months (never Mudaf)
    if (sn >= 6 && w[sn - 2] == S_BEH && w[sn - 1] == S_REH &&
        (w[0] == S_SEEN || w[0] == S_ALEF_HAMZA_A || w[0] == S_NOON || w[0] == S_DAL)) return 2; // سبتمبر/أكتوبر/نوفمبر/ديسمبر
    if (sn == 6 && w[0] == S_FEH && w[1] == S_BEH && w[2] == S_REH && w[3] == S_ALEF && w[4] == S_YEH && w[5] == S_REH) return 2; // فبراير
    if (sn == 5 && w[0] == S_ALEF_HAMZA_A && w[1] == S_BEH && w[2] == S_REH && w[3] == S_YEH && w[4] == S_LAM) return 2; // أبريل
    if (sn == 5 && w[0] == S_YEH && w[1] == S_NOON && w[2] == S_ALEF && w[3] == S_YEH && w[4] == S_REH) return 2; // يناير
    if (sn == 5 && w[0] == S_REH && w[1] == S_MEEM && w[2] == S_DAD && w[3] == S_ALEF && w[4] == S_NOON) return 2; // رمضان
    if (sn == 5 && w[0] == S_SHEEN && w[1] == S_AIN && w[2] == S_BEH && w[3] == S_ALEF && w[4] == S_NOON) return 2; // شعبان
    if (sn == 5 && w[0] == S_SEEN && w[1] == S_FEH && w[2] == S_YEH && w[3] == S_ALEF && w[4] == S_NOON) return 2; // سفيان
    if (sn == 4 && w[0] == S_MEEM && w[1] == S_ALEF && w[2] == S_REH && w[3] == S_SEEN) return 2; // مارس
    if (sn == 5 && w[0] == S_ALEF_HAMZA_A && w[1] == S_GHAIN && w[2] == S_SEEN && w[3] == S_TAH && w[4] == S_SEEN) return 2; // أغسطس
    if (sn == 5 && w[0] == S_ALEF_HAMZA_B && w[1] == S_YEH && w[2] == S_REH && w[3] == S_ALEF && w[4] == S_NOON) return 2; // إيران
    if (sn == 4 && w[0] == S_TEH && w[1] == S_WAW && w[2] == S_NOON && w[3] == S_SEEN) return 2; // تونس
    if (sn == 5 && w[0] == S_BEH && w[1] == S_ALEF && w[2] == S_REH && w[3] == S_YEH && w[4] == S_SEEN) return 2; // باريس
    if (sn == 5 && w[0] == S_MEEM && w[1] == S_DAL && w[2] == S_REH && w[3] == S_YEH && w[4] == S_DAL) return 2; // مدريد
    if (sn == 5 && w[0] == S_BEH && w[1] == S_REH && w[2] == S_LAM && w[3] == S_YEH && w[4] == S_NOON) return 2; // برلين
    if (sn == 6 && w[0] == S_ALEF_HAMZA_A && w[1] == S_WAW && w[2] == S_REH && w[3] == S_WAW && w[4] == S_BEH && w[5] == S_ALEF) return 2; // أوروبا
    if (sn == 4 && w[0] == S_ALEF_HAMZA_A && w[1] == S_HAH && w[2] == S_MEEM && w[3] == S_DAL) return 2; // أحمد
    if (sn == 4 && w[0] == S_DAL && w[1] == S_ALEF && w[2] == S_WAW && w[3] == S_DAL) return 2; // داود
    if (sn == 4 && w[0] == S_YEH && w[1] == S_WAW && w[2] == S_SEEN && w[3] == S_FEH) return 2; // يوسف
    if (sn == 4 && w[0] == S_MEEM && w[1] == S_REH && w[2] == S_YEH && w[3] == S_MEEM) return 2; // مريم
    if (sn == 4 && w[0] == S_ZAIN && w[1] == S_YEH && w[2] == S_NOON && w[3] == S_BEH) return 2; // زينب
    if (sn == 4 && w[0] == S_LAM && w[1] == S_NOON && w[2] == S_DAL && w[3] == S_NOON) return 2; // لندن
    if (sn == 4 && w[0] == S_DAL && w[1] == S_MEEM && w[2] == S_SHEEN && w[3] == S_QAF) return 2; // دمشق
    if (sn == 5 && w[0] == S_BEH && w[1] == S_GHAIN && w[2] == S_DAL && w[3] == S_ALEF && w[4] == S_DAL) return 2; // بغداد
    if (sn == 5 && w[0] == S_BEH && w[1] == S_YEH && w[2] == S_REH && w[3] == S_WAW && w[4] == S_TEH) return 2; // بيروت
    if (sn == 5 && w[0] == S_AIN && w[1] == S_THEH && w[2] == S_MEEM && w[3] == S_ALEF && w[4] == S_NOON) return 2; // عثمان
    if (sn == 5 && w[0] == S_YEH && w[1] == S_AIN && w[2] == S_QAF && w[3] == S_WAW && w[4] == S_BEH) return 2; // يعقوب
    if (sn == 5 && w[0] == S_ALEF_HAMZA_B && w[1] == S_SEEN && w[2] == S_HAH && w[3] == S_ALEF && w[4] == S_QAF) return 2; // إسحاق
    if (sn == 6 && w[0] == S_SEEN && w[1] == S_LAM && w[2] == S_YEH && w[3] == S_MEEM && w[4] == S_ALEF && w[5] == S_NOON) return 2; // سليمان
    if (sn == 7 && (w[0] == S_ALEF_HAMZA_B || w[0] == S_ALEF) && w[1] == S_SEEN && w[2] == S_REH && w[3] == S_ALEF && w[4] == S_YEH_HAMZA && w[5] == S_YEH && w[6] == S_LAM) return 2;
    if (sn == 7 && w[0] == S_ALEF_HAMZA_B && w[1] == S_BEH && w[2] == S_REH && w[3] == S_ALEF && w[4] == S_HEH && w[5] == S_YEH && w[6] == S_MEEM) return 2;
    if (sn == 7 && w[0] == S_ALEF_HAMZA_B && w[1] == S_SEEN && w[2] == S_MEEM && w[3] == S_ALEF && w[4] == S_AIN && w[5] == S_YEH && w[6] == S_LAM) return 2;
    return 0;
}

inline bool is_diptote_surface(const uint8_t* s, size_t n, size_t st) {
    return classify_diptote_surface(s, n, st) != 0;
}

inline uint8_t classify_diptote_stem(const uint8_t* s, size_t n, size_t st, const uint8_t* d) {
    if (n <= st + 2) return 0;
    const size_t sn = n - st;
    const uint8_t* w = s + st;
    const uint8_t* wd = d + st;
    uint8_t surf = classify_diptote_surface(s, n, st);
    if (surf != 0) return surf;
    if (w[sn - 1] == S_TEH_MARBUTA) return 0;
    if (sn == 5 && w[2] == S_ALEF && w[4] != S_ALEF && w[4] != S_ALEF_MAKSURA) {
        if (w[3] == S_YEH_HAMZA && wd[0] == DIAC_FATHA) return 1;
        if (w[0] == S_MEEM && wd[0] == DIAC_FATHA && wd[1] == DIAC_FATHA) return 1;
        if (w[1] == S_WAW && wd[0] == DIAC_FATHA) return 1;
        if (w[0] == S_DAL && w[1] == S_REH && w[3] == S_HEH && w[4] == S_MEEM) return 1;
    }
    if (sn == 6 && w[2] == S_ALEF && w[4] == S_YEH && w[5] != S_YEH &&
        (wd[0] == DIAC_FATHA || w[0] == S_ALEF_HAMZA_A || w[0] == S_TEH)) {
        return 1;
    }
    if (sn == 4 && w[0] == S_ALEF_HAMZA_A && wd[0] == DIAC_FATHA && wd[1] == DIAC_SUKUN &&
        w[3] != S_ALEF && w[3] != S_ALEF_MAKSURA) {
        return 1;
    }
    if (sn == 3 && w[0] == S_ALEF_MADDA && w[1] == S_KHAH && w[2] == S_REH && wd[0] == DIAC_FATHA) {
        return 1;
    }
    if (sn >= 5 && w[sn - 2] == S_ALEF && w[sn - 1] == S_HAMZA &&
        w[0] != S_ALEF && w[0] != S_ALEF_HAMZA_B && w[sn - 3] != S_ALEF) {
        if (sn >= 6 || wd[0] == DIAC_DAMMA || wd[0] == DIAC_FATHA) return 1;
    }
    if (sn == 3 && w[0] == S_AIN && w[1] == S_MEEM && w[2] == S_REH && wd[0] == DIAC_DAMMA) return 2; // عُمَر
    if (sn == 3 && w[0] == S_MEEM && w[1] == S_SAD && w[2] == S_REH && wd[0] == DIAC_KASRA) return 2; // مِصْر
    return 0;
}

inline bool is_diptote_stem(const uint8_t* s, size_t n, size_t st, const uint8_t* d) {
    return classify_diptote_stem(s, n, st, d) != 0;
}

inline uint16_t compute_surf_flags(const uint8_t* s, size_t n, const SurfaceInfo& si) {
    uint16_t sf = SF_NONE;
    if (is_man_word(s, n)) sf |= SF_MAN;
    if (is_ibn_word(s, n)) sf |= SF_IBN;
    if (is_five_noun_head(s, n)) sf |= SF_FIVE_NOUN;
    if (is_short_prep_pron(s, n)) sf |= SF_SHORT_PREP_PRON;
    if (is_baad_qabl(s, n)) sf |= SF_BAAD_QABL;
    if (is_ali_word(s, n)) sf |= SF_ALI;
    if (is_ala_word(s, n)) sf |= SF_ALA;
    if (is_am_word(s, n)) sf |= SF_AM;
    if (is_themma_word(s, n)) sf |= SF_THEMMA;
    if (is_ashar_word(s, n)) sf |= SF_ASHAR;
    if (is_compound_num_unit(s, n)) sf |= SF_COMP_NUM;
    if (is_name_head_word(s, n)) sf |= SF_NAME_HEAD;
    if (starts_with_wasl(s, n, si)) sf |= SF_WASL;
    if (is_inna_pron_word(s, n)) sf |= SF_INNA_PRON;
    if (is_ams_word(s, n)) sf |= SF_AMS;
    if (is_quant_idafa_word(s, n)) sf |= SF_QUANT_IDAFA;
    return sf;
}

} // namespace

Diacritizer::Diacritizer() {
    for (uint32_t i = 0; i < NUM_WEIGHTS; ++i) weights_[i] = DEFAULT_WEIGHTS[i];
}

Diacritizer::~Diacritizer() { unload(); }

void Diacritizer::unload() {
#ifdef _WIN32
    if (mapped_ptr_) {
        UnmapViewOfFile(mapped_ptr_);
        mapped_ptr_ = nullptr;
        mapped_size_ = 0;
    }
    if (win_map_) {
        CloseHandle(static_cast<HANDLE>(win_map_));
        win_map_ = nullptr;
    }
    if (win_file_) {
        CloseHandle(static_cast<HANDLE>(win_file_));
        win_file_ = nullptr;
    }
#else
    if (mapped_ptr_) {
        munmap(mapped_ptr_, mapped_size_);
        mapped_ptr_ = nullptr;
        mapped_size_ = 0;
    }
#endif
    is_v3_ = false;
    is_v5_ = false;
    has_v5_24roles_ = false;
    vocab_count_ = 0;
    fst_nodes_count_ = 0;
    word_meta_ = nullptr;
    word_meta_v3_ = nullptr;
    candidates_ = nullptr;
    candidates_v3_ = nullptr;
    candidates_v5_ = nullptr;
    core_pool_ = nullptr;
    trans_matrix_ = nullptr;
    trans_matrix_size_ = 0;
    oov_feat_count_ = 16;
    irab_feat_count_ = 40;
    fst_ = BitmappedFST();
    bigrams_ = BigramTable();
    oov_trees_ = FlatTreeEnsemble();
    irab_trees_ = FlatTreeEnsemble();
    morph_trees_ = FlatTreeEnsemble();
}

bool Diacritizer::load(const std::string& model_path, std::string* err_msg) {
    unload();
#ifdef _WIN32
    // UTF-8 path -> UTF-16, so folders with non-ASCII names (an Arabic Windows user name) open too.
    std::wstring wpath(MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, &wpath[0], (int)wpath.size());
    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        if (err_msg) *err_msg = "Failed to open model file: " + model_path;
        return false;
    }
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(hFile, &li) || li.QuadPart < static_cast<LONGLONG>(sizeof(FileHeader))) {
        CloseHandle(hFile);
        if (err_msg) *err_msg = "Invalid or empty model file";
        return false;
    }
    size_t fsize = static_cast<size_t>(li.QuadPart);
    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        if (err_msg) *err_msg = "CreateFileMappingA failed";
        return false;
    }
    void* ptr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!ptr) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        if (err_msg) *err_msg = "MapViewOfFile failed";
        return false;
    }
    win_file_ = hFile;
    win_map_ = hMap;
#else
    int fd = open(model_path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (err_msg) *err_msg = "Failed to open model file: " + model_path;
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(FileHeader))) {
        close(fd);
        if (err_msg) *err_msg = "Invalid or empty model file";
        return false;
    }
    size_t fsize = static_cast<size_t>(st.st_size);
    void* ptr = mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) {
        if (err_msg) *err_msg = "mmap failed";
        return false;
    }
#endif
    const auto* base = static_cast<const uint8_t*>(ptr);
    const auto* hdr2 = reinterpret_cast<const FileHeader*>(base);
    if (hdr2->magic != TASH_MAGIC ||
        (hdr2->version != TASH_VERSION_2 && hdr2->version != TASH_VERSION_3 &&
         hdr2->version != TASH_VERSION_4 && hdr2->version != TASH_VERSION_5)) {
        mapped_ptr_ = ptr;
        mapped_size_ = fsize;
        unload();
        if (err_msg) *err_msg = "Bad magic or unsupported model version";
        return false;
    }
    auto in_range = [fsize](uint64_t off, uint64_t bytes) { return off <= fsize && bytes <= fsize - off; };

    if (hdr2->version == TASH_VERSION_5) {
        if (fsize < sizeof(FileHeaderV5)) {
            mapped_ptr_ = ptr;
            mapped_size_ = fsize;
            unload();
            if (err_msg) *err_msg = "Truncated header";
            return false;
        }
        const auto* hdr5 = reinterpret_cast<const FileHeaderV5*>(base);
        const bool cand5 = (hdr5->flags & 2u) != 0;
        const bool bg_int8 = (hdr5->flags & 4u) != 0;
        const uint8_t tree_mode = (hdr5->flags & 1u) ? 2 : 1;
        if (!in_range(hdr5->fst_blob_offset, hdr5->fst_blob_size) ||
            !in_range(hdr5->word_meta_offset, uint64_t(hdr5->word_meta_count) * sizeof(WordMetaV3)) ||
            !in_range(hdr5->candidates_offset, uint64_t(hdr5->candidates_count) * (cand5 ? sizeof(WordCandidateV5) : sizeof(WordCandidateV3))) ||
            !in_range(hdr5->core_pool_offset, hdr5->core_pool_size) ||
            !in_range(hdr5->bg_row_off_offset, uint64_t(hdr5->bg_row_off_count) * sizeof(uint32_t)) ||
            !in_range(hdr5->bg_block_base_offset, uint64_t(hdr5->bg_block_base_count) * sizeof(uint32_t)) ||
            !in_range(hdr5->bg_entries_offset, uint64_t(hdr5->bg_entries_count) * sizeof(BigramEntryV5)) ||
            !in_range(hdr5->bg_vals_offset, uint64_t(hdr5->bg_vals_count) * (bg_int8 ? sizeof(int8_t) : sizeof(int16_t))) ||
            !in_range(hdr5->trans_matrix_offset, uint64_t(hdr5->trans_matrix_size) * sizeof(int16_t)) ||
            !in_range(hdr5->oov_trees_offset, hdr5->oov_trees_size) ||
            !in_range(hdr5->irab_trees_offset, hdr5->irab_trees_size) ||
            !in_range(hdr5->morph_trees_offset, hdr5->morph_trees_size) ||
            !in_range(hdr5->weights_offset, uint64_t(hdr5->weights_count) * sizeof(int32_t))) {
            mapped_ptr_ = ptr;
            mapped_size_ = fsize;
            unload();
            if (err_msg) *err_msg = "Model sections exceed file bounds";
            return false;
        }
        mapped_ptr_ = ptr;
        mapped_size_ = fsize;
        is_v5_ = true;
        is_v3_ = false;
        vocab_count_ = hdr5->word_meta_count;
        if (!fst_.init_v5(base + hdr5->fst_blob_offset, hdr5->fst_blob_size)) {
            unload();
            if (err_msg) *err_msg = "Invalid v5 FST blob";
            return false;
        }
        fst_nodes_count_ = fst_.node_count();
        word_meta_v3_ = reinterpret_cast<const WordMetaV3*>(base + hdr5->word_meta_offset);
        if (cand5) candidates_v5_ = reinterpret_cast<const WordCandidateV5*>(base + hdr5->candidates_offset);
        else candidates_v3_ = reinterpret_cast<const WordCandidateV3*>(base + hdr5->candidates_offset);
        core_pool_ = base + hdr5->core_pool_offset;
        if (hdr5->bg_entries_count > 0) {
            bigrams_.init_v5(
                reinterpret_cast<const uint32_t*>(base + hdr5->bg_row_off_offset), hdr5->bg_row_off_count,
                reinterpret_cast<const uint32_t*>(base + hdr5->bg_block_base_offset), hdr5->bg_block_base_count,
                reinterpret_cast<const BigramEntryV5*>(base + hdr5->bg_entries_offset), hdr5->bg_entries_count,
                bg_int8 ? reinterpret_cast<const int8_t*>(base + hdr5->bg_vals_offset) : nullptr,
                bg_int8 ? nullptr : reinterpret_cast<const int16_t*>(base + hdr5->bg_vals_offset),
                hdr5->bg_vals_count);
        }
        if (hdr5->trans_matrix_size > 0) {
            trans_matrix_ = reinterpret_cast<const int16_t*>(base + hdr5->trans_matrix_offset);
            trans_matrix_size_ = hdr5->trans_matrix_size;
            has_v5_24roles_ = ((hdr5->flags & 8u) != 0) ||
                              (trans_matrix_size_ >= NUM_TRANS_ROLES_V5 * NUM_DIACRITICS * NUM_TRANS_ROLES_V5 * NUM_DIACRITICS);
        }
        if (hdr5->oov_trees_size > 0)
            oov_trees_.init(base + hdr5->oov_trees_offset, hdr5->oov_trees_size, tree_mode);
        if (hdr5->irab_trees_size > 0)
            irab_trees_.init(base + hdr5->irab_trees_offset, hdr5->irab_trees_size, tree_mode);
        if (hdr5->morph_trees_size > 0)
            morph_trees_.init(base + hdr5->morph_trees_offset, hdr5->morph_trees_size, tree_mode);
        oov_feat_count_ = OOV_NUM_FEATURES;
        irab_feat_count_ = (hdr5->flags & 16u) ? IRAB_NUM_FEATURES : IRAB_V5_BASE_FEATURES;
        for (uint32_t i = 0; i < NUM_WEIGHTS; ++i) weights_[i] = DEFAULT_WEIGHTS[i];
        const auto* w = reinterpret_cast<const int32_t*>(base + hdr5->weights_offset);
        for (uint32_t i = 0; i < hdr5->weights_count && i < NUM_WEIGHTS; ++i) weights_[i] = w[i];
        (void)particle_map();
        return true;
    }

    if (hdr2->version == TASH_VERSION_3 || hdr2->version == TASH_VERSION_4) {
        const size_t min_hdr = hdr2->version == TASH_VERSION_4 ? sizeof(FileHeaderV4) : sizeof(FileHeaderV3);
        if (fsize < min_hdr) {
            mapped_ptr_ = ptr;
            mapped_size_ = fsize;
            unload();
            if (err_msg) *err_msg = "Truncated v3/v4 header";
            return false;
        }
        const auto* hdr3 = reinterpret_cast<const FileHeaderV3*>(base);
        const auto* hdr4 = (hdr2->version == TASH_VERSION_4) ? reinterpret_cast<const FileHeaderV4*>(base) : nullptr;
        if (!in_range(hdr3->fst_nodes_offset, uint64_t(hdr3->fst_nodes_count) * sizeof(BitTrieNodeV3)) ||
            !in_range(hdr3->word_meta_offset, uint64_t(hdr3->word_meta_count) * sizeof(WordMetaV3)) ||
            !in_range(hdr3->candidates_offset, uint64_t(hdr3->candidates_count) * sizeof(WordCandidateV3)) ||
            !in_range(hdr3->core_pool_offset, hdr3->core_pool_size) ||
            !in_range(hdr3->bg_row_off_offset, uint64_t(hdr3->bg_row_off_count) * sizeof(uint32_t)) ||
            !in_range(hdr3->bg_entries_offset, uint64_t(hdr3->bg_entries_count) * sizeof(BigramEntryV3)) ||
            !in_range(hdr3->bg_vals_offset, uint64_t(hdr3->bg_vals_count) * ((hdr3->flags & 4u) ? sizeof(int8_t) : sizeof(int16_t))) ||
            !in_range(hdr3->trans_matrix_offset, uint64_t(hdr3->trans_matrix_size) * sizeof(int16_t)) ||
            !in_range(hdr3->oov_trees_offset, hdr3->oov_trees_size) ||
            !in_range(hdr3->irab_trees_offset, hdr3->irab_trees_size) ||
            !in_range(hdr3->weights_offset, uint64_t(hdr3->weights_count) * sizeof(int32_t)) ||
            (hdr4 && !in_range(hdr4->morph_trees_offset, hdr4->morph_trees_size))) {
            mapped_ptr_ = ptr;
            mapped_size_ = fsize;
            unload();
            if (err_msg) *err_msg = "v3/v4 model sections exceed file bounds";
            return false;
        }
        mapped_ptr_ = ptr;
        mapped_size_ = fsize;
        is_v3_ = true;
        vocab_count_ = hdr3->word_meta_count;
        fst_nodes_count_ = hdr3->fst_nodes_count;
        fst_.init_v3(reinterpret_cast<const BitTrieNodeV3*>(base + hdr3->fst_nodes_offset), hdr3->fst_nodes_count);
        word_meta_v3_ = reinterpret_cast<const WordMetaV3*>(base + hdr3->word_meta_offset);
        candidates_v3_ = reinterpret_cast<const WordCandidateV3*>(base + hdr3->candidates_offset);
        core_pool_ = base + hdr3->core_pool_offset;
        if (hdr3->bg_entries_count > 0) {
            if (hdr3->flags & 4u) {
                bigrams_.init_v4(
                    reinterpret_cast<const uint32_t*>(base + hdr3->bg_row_off_offset), hdr3->bg_row_off_count,
                    reinterpret_cast<const BigramEntryV3*>(base + hdr3->bg_entries_offset), hdr3->bg_entries_count,
                    reinterpret_cast<const int8_t*>(base + hdr3->bg_vals_offset), hdr3->bg_vals_count);
            } else {
                bigrams_.init_v3(
                    reinterpret_cast<const uint32_t*>(base + hdr3->bg_row_off_offset), hdr3->bg_row_off_count,
                    reinterpret_cast<const BigramEntryV3*>(base + hdr3->bg_entries_offset), hdr3->bg_entries_count,
                    reinterpret_cast<const int16_t*>(base + hdr3->bg_vals_offset), hdr3->bg_vals_count);
            }
        }
        if (hdr3->trans_matrix_size > 0) {
            trans_matrix_ = reinterpret_cast<const int16_t*>(base + hdr3->trans_matrix_offset);
            trans_matrix_size_ = hdr3->trans_matrix_size;
        }
        if (hdr3->oov_trees_size > 0)
            oov_trees_.init(base + hdr3->oov_trees_offset, hdr3->oov_trees_size, (hdr3->flags & 1u) ? 1 : 0);
        if (hdr3->irab_trees_size > 0)
            irab_trees_.init(base + hdr3->irab_trees_offset, hdr3->irab_trees_size, (hdr3->flags & 2u) ? 1 : 0);
        if (hdr4 && hdr4->morph_trees_size > 0)
            morph_trees_.init(base + hdr4->morph_trees_offset, hdr4->morph_trees_size, (hdr3->flags & 8u) ? 1 : 0);
        oov_feat_count_ = OOV_LEGACY_FEATURES;
        irab_feat_count_ = morph_trees_.valid() ? IRAB_LEGACY_FEATURES : 24;
        for (uint32_t i = 0; i < NUM_WEIGHTS; ++i) weights_[i] = DEFAULT_WEIGHTS[i];
        const auto* w = reinterpret_cast<const int32_t*>(base + hdr3->weights_offset);
        for (uint32_t i = 0; i < hdr3->weights_count && i < NUM_WEIGHTS; ++i) weights_[i] = w[i];
        (void)particle_map();
        return true;
    }

    if (!in_range(hdr2->fst_nodes_offset, uint64_t(hdr2->fst_nodes_count) * sizeof(BitTrieNode)) ||
        !in_range(hdr2->word_meta_offset, uint64_t(hdr2->word_meta_count) * sizeof(WordMeta)) ||
        !in_range(hdr2->candidates_offset, uint64_t(hdr2->candidates_count) * sizeof(WordCandidate)) ||
        !in_range(hdr2->core_pool_offset, hdr2->core_pool_size) ||
        !in_range(hdr2->bigram_offset, uint64_t(hdr2->bigram_capacity) * sizeof(BigramEntry)) ||
        !in_range(hdr2->trans_matrix_offset, uint64_t(hdr2->trans_matrix_size) * sizeof(int16_t)) ||
        !in_range(hdr2->oov_trees_offset, hdr2->oov_trees_size) ||
        !in_range(hdr2->irab_trees_offset, hdr2->irab_trees_size) ||
        !in_range(hdr2->weights_offset, uint64_t(hdr2->weights_count) * sizeof(int32_t))) {
        mapped_ptr_ = ptr;
        mapped_size_ = fsize;
        unload();
        if (err_msg) *err_msg = "Model sections exceed file bounds";
        return false;
    }
    mapped_ptr_ = ptr;
    mapped_size_ = fsize;
    is_v3_ = false;
    vocab_count_ = hdr2->word_meta_count;
    fst_nodes_count_ = hdr2->fst_nodes_count;
    fst_.init(reinterpret_cast<const BitTrieNode*>(base + hdr2->fst_nodes_offset), hdr2->fst_nodes_count);
    word_meta_ = reinterpret_cast<const WordMeta*>(base + hdr2->word_meta_offset);
    candidates_ = reinterpret_cast<const WordCandidate*>(base + hdr2->candidates_offset);
    core_pool_ = base + hdr2->core_pool_offset;
    if (hdr2->bigram_capacity > 0)
        bigrams_.init(reinterpret_cast<const BigramEntry*>(base + hdr2->bigram_offset), hdr2->bigram_capacity);
    if (hdr2->trans_matrix_size > 0) {
        trans_matrix_ = reinterpret_cast<const int16_t*>(base + hdr2->trans_matrix_offset);
        trans_matrix_size_ = hdr2->trans_matrix_size;
    }
    if (hdr2->oov_trees_size > 0) oov_trees_.init(base + hdr2->oov_trees_offset, hdr2->oov_trees_size, 0);
    if (hdr2->irab_trees_size > 0) irab_trees_.init(base + hdr2->irab_trees_offset, hdr2->irab_trees_size, 0);
    oov_feat_count_ = OOV_LEGACY_FEATURES;
    irab_feat_count_ = 24;
    for (uint32_t i = 0; i < NUM_WEIGHTS; ++i) weights_[i] = DEFAULT_WEIGHTS[i];
    const auto* w = reinterpret_cast<const int32_t*>(base + hdr2->weights_offset);
    for (uint32_t i = 0; i < hdr2->weights_count && i < NUM_WEIGHTS; ++i) weights_[i] = w[i];
    (void)particle_map();
    return true;
}

uint8_t Diacritizer::cand_role_v5(const Token& tok, const Cand& c) const {
    switch (tok.si.particle) {
        case PART_GEN: return 1;
        case PART_NASB:
        case PART_HATTA: return 2;
        case PART_JAZM: return 3;
        case PART_LAMA: return (c.morph_class == MC_PART_SHADDA) ? 13 : 3;
        case PART_AN:
        case PART_LI_AN: return (c.morph_class == MC_PART_SHADDA) ? 4 : 2;
        case PART_IN: return (c.morph_class == MC_PART_SHADDA) ? 4 : 13;
        case PART_LAKIN: return (c.morph_class == MC_PART_SHADDA) ? 4 : 6;
        case PART_INNA: return 4;
        case PART_KANA: return 5;
        case PART_CONJ: return 6;
        case PART_LA: return 7;
        case PART_QAD:
        case PART_SAWFA: return 8;
        case PART_DEM: return 9;
        case PART_REL: return 10;
        case PART_PRON: return 11;
        case PART_IDAFA_NOUN:
        case PART_NUM_IDAFA: return 12;
        case PART_COND:
        case PART_QUEST:
        case PART_AMMA:
        case PART_YA:
        case PART_AY:
        case PART_ILLA: return 13;
        case PART_PREP_PRON: return 14;
        default: break;
    }
    switch (c.pos_tag) {
        case POS_VERB_PAST_3SG: return 15;
        case POS_VERB_PAST_SUBJ: return 16;
        case POS_VERB_PAST_FEM: return 17;
        case POS_VERB_PASS: return 18;
        case POS_VERB_IMPERF_12: return 19;
        case POS_VERB_IMPERF_3: return 20;
        case POS_NOUN_AL: return 21;
        case POS_NOUN_SUFFIX: return 22;
        default: return 23;
    }
}

int32_t Diacritizer::trans_score(uint8_t prev_role, uint8_t prev_end, uint8_t curr_role, uint8_t curr_end) const {
    if (has_v5_24roles_) {
        if (!trans_matrix_ || prev_role >= NUM_TRANS_ROLES_V5 || curr_role >= NUM_TRANS_ROLES_V5) return 0;
        size_t idx = ((static_cast<size_t>(prev_role) * NUM_DIACRITICS + prev_end) * NUM_TRANS_ROLES_V5 + curr_role) *
                         NUM_DIACRITICS + curr_end;
        return trans_matrix_[idx];
    }
    if (!trans_matrix_ || trans_matrix_size_ < 16u * NUM_DIACRITICS * 16u * NUM_DIACRITICS) return 0;
    size_t idx = ((static_cast<size_t>(prev_role & 0xF) * NUM_DIACRITICS + prev_end) * 16u + (curr_role & 0xF)) *
                     NUM_DIACRITICS + curr_end;
    return trans_matrix_[idx];
}

uint32_t Diacritizer::lookup_relaxed(const uint8_t* syms, size_t n, int& repl_pos, uint8_t& repl_sym) const {
    repl_pos = -1;
    repl_sym = 0;
    if (n == 0 || n > MAX_WORD_LEN) return UINT32_MAX;
    uint32_t wid = fst_.lookup(syms, n);
    if (wid != UINT32_MAX && wid < vocab_count_) return wid;
    uint8_t buf[MAX_WORD_LEN];
    std::memcpy(buf, syms, n);
    auto attempt = [&](size_t pos, uint8_t sym, bool record_repl) -> uint32_t {
        uint8_t old = buf[pos];
        buf[pos] = sym;
        uint32_t r = fst_.lookup(buf, n);
        buf[pos] = old;
        if (r != UINT32_MAX && r < vocab_count_) {
            if (record_repl) {
                repl_pos = static_cast<int>(pos);
                repl_sym = sym;
            }
            return r;
        }
        return UINT32_MAX;
    };
    for (size_t pos = 0; pos < std::min<size_t>(2, n); ++pos) {
        if (syms[pos] == S_ALEF_WASLA) {
            uint32_t r = attempt(pos, S_ALEF, false);
            if (r != UINT32_MAX) return r;
        }
    }
    if (syms[0] == S_ALEF) {
        for (uint8_t a : {S_ALEF_HAMZA_A, S_ALEF_HAMZA_B, S_ALEF_MADDA}) {
            uint32_t r = attempt(0, a, true);
            if (r != UINT32_MAX) return r;
        }
    }
    if (n >= 2) {
        uint8_t last = syms[n - 1];
        uint32_t r = UINT32_MAX;
        if (last == S_HEH) r = attempt(n - 1, S_TEH_MARBUTA, true);
        else if (last == S_YEH) r = attempt(n - 1, S_ALEF_MAKSURA, true);
        else if (last == S_ALEF_MAKSURA) r = attempt(n - 1, S_YEH, true);
        if (r != UINT32_MAX) return r;
    }
    return UINT32_MAX;
}

bool Diacritizer::has_stem_in_fst(const uint8_t* s, size_t stem_len) const {
    if (stem_len < 2 || stem_len > MAX_WORD_LEN) return false;
    if (fst_.lookup(s, stem_len) != UINT32_MAX) return true;
    uint8_t buf[MAX_WORD_LEN];
    std::memcpy(buf, s, stem_len);
    if (buf[stem_len - 1] == S_TEH) {
        buf[stem_len - 1] = S_TEH_MARBUTA;
        if (fst_.lookup(buf, stem_len) != UINT32_MAX) return true;
        buf[stem_len - 1] = S_TEH;
    } else if (buf[stem_len - 1] == S_ALEF) {
        buf[stem_len - 1] = S_ALEF_MAKSURA;
        if (fst_.lookup(buf, stem_len) != UINT32_MAX) return true;
        buf[stem_len - 1] = S_ALEF;
    }
    if (stem_len >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM || s[0] == S_KAF)) {
        if (fst_.lookup(s + 1, stem_len - 1) != UINT32_MAX) return true;
    }
    return false;
}

void Diacritizer::finalize_cand(Cand& c, const uint8_t* s, size_t n, const SurfaceInfo& si, uint8_t has_noun_lex) const {
    const uint8_t* d = c.diac;
    c.ext_flags = 0;

    if (!(c.flags & CF_SUFFIX)) {
        c.case_pos = static_cast<uint8_t>((n >= 3 && s[n - 1] == S_ALEF) ? n - 2 : n - 1);

        if (n == 3 && (s[0] == S_ALEF_HAMZA_A || s[0] == S_ALEF_HAMZA_B) && s[1] == S_NOON && s[2] == S_HEH &&
            (d[2] == DIAC_DAMMA || d[2] == DIAC_KASRA)) {
            c.flags |= CF_SUFFIX;
            c.case_pos = 1;
        } else if (si.particle == PART_NONE && si.al_len == 0 && n >= 4 && !diac_is_tanween(d[n - 1])) {
            size_t sl = 0;
            if (n >= 6 && (s[n - 3] == S_HEH || s[n - 3] == S_KAF) && s[n - 2] == S_MEEM && s[n - 1] == S_ALEF &&
                d[n - 1] == DIAC_NONE) {
                sl = 3;
            } else if (n >= 5 && (
                (s[n - 2] == S_HEH && ((s[n - 1] == S_ALEF && d[n - 1] == DIAC_NONE) ||
                                       (s[n - 1] == S_MEEM && (d[n - 1] == DIAC_SUKUN || d[n - 1] == DIAC_DAMMA)) ||
                                       (s[n - 1] == S_NOON && d[n - 1] == DIAC_SHADDA_FATHA))) ||
                (s[n - 2] == S_KAF && ((s[n - 1] == S_MEEM && (d[n - 1] == DIAC_SUKUN || d[n - 1] == DIAC_DAMMA)) ||
                                       (s[n - 1] == S_NOON && d[n - 1] == DIAC_SHADDA_FATHA))) ||
                (s[n - 2] == S_NOON && ((s[n - 1] == S_ALEF && d[n - 1] == DIAC_NONE) ||
                                        (s[n - 1] == S_YEH && d[n - 1] == DIAC_NONE))))) {
                sl = 2;
            } else if ((s[n - 1] == S_HEH && (d[n - 1] == DIAC_DAMMA || d[n - 1] == DIAC_KASRA)) ||
                       (s[n - 1] == S_KAF && (d[n - 1] == DIAC_FATHA || d[n - 1] == DIAC_KASRA))) {
                sl = 1;
            }

            if (sl > 0 && n - sl >= 2) {
                size_t stem_len = n - sl;
                uint8_t stem_end_d = d[stem_len - 1];
                if (!diac_is_tanween(stem_end_d) && has_stem_in_fst(s, stem_len)) {
                    c.flags |= CF_SUFFIX;
                    c.case_pos = static_cast<uint8_t>(stem_len - 1);
                }
            }
        }
    }

    c.verb_mood_diac = d[c.case_pos];
    c.verb_pattern = 0;

    if (is_plural_meem_pron(s, n)) c.ext_flags |= EF_PLURAL_MEEM;
    if (si.al_len >= 2) {
        size_t lam_pos = si.al_lam_pos;
        if (lam_pos < n && !is_sun_letter(s[si.al_len]) && d[lam_pos] == DIAC_SUKUN) {
            c.ext_flags |= EF_LAM_SUKUN;
        }
    }
    if (is_sound_fem_plural_stem(s, n, si.al_len)) c.ext_flags |= EF_FEM_PL_AT;
    if (n >= 4 && !(c.flags & CF_SUFFIX) && s[n - 1] == S_NOON && d[n - 1] == DIAC_KASRA &&
        ((s[n - 2] == S_ALEF && d[n - 2] == DIAC_NONE) ||
         (s[n - 2] == S_YEH && n >= 5 && d[n - 3] == DIAC_FATHA && d[n - 2] == DIAC_SUKUN))) {
        c.ext_flags |= EF_DUAL_NOON;
    }
    if (n >= 5 && !(c.flags & CF_SUFFIX) && s[n - 1] == S_NOON && d[n - 2] == DIAC_NONE &&
        ((s[n - 2] == S_WAW && diac_family(d[n - 3]) == FAM_DAMMA) ||
         (s[n - 2] == S_YEH && diac_family(d[n - 3]) == FAM_KASRA && d[n - 1] == DIAC_FATHA))) {
        c.ext_flags |= EF_MASC_PL_NOON;
    }

    const size_t sn = (c.flags & CF_SUFFIX) ? (c.case_pos + 1) : n;
    size_t st = si.al_len;
    if (!(c.flags & CF_HAS_AL) && sn >= 4 && (s[0] == S_WAW || s[0] == S_FEH) &&
        (is_imperf_prefix(s[1]) || s[1] == S_SEEN || d[0] == DIAC_FATHA)) {
        st = 1;
    }
    if (!(c.flags & CF_HAS_AL) && st < sn && s[st] == S_SEEN && sn - st >= 4 &&
        is_imperf_prefix(s[st + 1]) && d[st] == DIAC_FATHA) {
        ++st;
    }

    auto set_pos_and_mc = [&]() {
        if (si.particle != PART_NONE && si.particle != PART_KANA && si.particle != PART_IDAFA_NOUN && si.particle != PART_NUM_IDAFA) {
            c.pos_tag = POS_PARTICLE;
            bool sh_any = false;
            for (size_t k = 0; k < n; ++k) if (diac_has_shadda(d[k])) { sh_any = true; break; }
            c.morph_class = sh_any ? MC_PART_SHADDA : MC_PART_SUKUN_LIGHT;
            return;
        }
        if (c.verb_pattern == 1) {
            bool is_imp = is_imperf_prefix(s[st]);
            bool is_pass = (c.ext_flags & EF_PASSIVE) != 0;
            if (is_imp) {
                c.pos_tag = is_pass ? POS_VERB_PASS :
                    ((s[st] == S_ALEF_HAMZA_A || s[st] == S_NOON || (sn >= 5 && (s[sn - 2] == S_WAW || s[sn - 2] == S_ALEF) && s[sn - 1] == S_NOON))
                     ? POS_VERB_IMPERF_12 : POS_VERB_IMPERF_3);
                c.morph_class = is_pass ? MC_VERB_IMPERF_PASS : MC_VERB_IMPERF_ACT;
            } else {
                bool has_subj = ((sn >= 4 && s[sn - 1] == S_TEH && !(c.ext_flags & EF_PAST_TEH_FEM) && d[sn - 2] == DIAC_SUKUN) ||
                                 (sn >= 5 && s[sn - 2] == S_TEH && s[sn - 1] == S_MEEM) ||
                                 (sn >= 5 && s[sn - 2] == S_NOON && s[sn - 1] == S_ALEF) ||
                                 (sn >= 4 && s[sn - 2] == S_WAW && s[sn - 1] == S_ALEF) ||
                                 (sn >= 4 && s[sn - 1] == S_ALEF && s[sn - 2] != S_WAW && d[sn - 2] == DIAC_FATHA));
                if (is_pass) c.pos_tag = POS_VERB_PASS;
                else if (c.ext_flags & EF_PAST_TEH_FEM) c.pos_tag = POS_VERB_PAST_FEM;
                else if (has_subj) c.pos_tag = POS_VERB_PAST_SUBJ;
                else c.pos_tag = POS_VERB_PAST_3SG;
                if (is_pass) {
                    c.morph_class = MC_VERB_PAST_PASS;
                } else {
                    bool sh_core = false;
                    for (size_t k = st; k < c.case_pos; ++k) if (diac_has_shadda(d[k])) { sh_core = true; break; }
                    c.morph_class = sh_core ? MC_VERB_SHADDA_ACT : MC_VERB_PAST_ACT;
                }
            }
            return;
        }
        if ((c.flags & CF_HAS_AL) || si.al_len > 0) c.pos_tag = POS_NOUN_AL;
        else if (c.flags & CF_SUFFIX) c.pos_tag = POS_NOUN_SUFFIX;
        else c.pos_tag = POS_NOUN_INDEF;

        bool sh_core = false;
        for (size_t k = st + 1; k < c.case_pos; ++k) if (diac_has_shadda(d[k])) { sh_core = true; break; }
        if (sh_core) {
            c.morph_class = MC_NOUN_SHADDA_CORE;
        } else if (st < n) {
            uint8_t f0 = diac_family(d[st]);
            if (f0 == FAM_FATHA) c.morph_class = MC_NOUN_FATHA_START;
            else if (f0 == FAM_KASRA) c.morph_class = MC_NOUN_KASRA_START;
            else if (f0 == FAM_DAMMA) c.morph_class = MC_NOUN_DAMMA_START;
            else c.morph_class = MC_OTHER;
        } else {
            c.morph_class = MC_OTHER;
        }
    };

    if ((c.flags & CF_HAS_AL) || si.al_len > 0) {
        if (is_diptote_stem(s, n, si.al_len, d)) c.ext_flags |= EF_DIPTOTE;
        set_pos_and_mc();
        return;
    }
    if (si.particle != PART_NONE && si.particle != PART_KANA) {
        if (is_diptote_stem(s, sn, st, d)) c.ext_flags |= EF_DIPTOTE;
        set_pos_and_mc();
        return;
    }
    if (sn < 3 || diac_is_tanween(d[c.case_pos]) || s[sn - 1] == S_TEH_MARBUTA) {
        if (is_diptote_stem(s, sn, st, d)) c.ext_flags |= EF_DIPTOTE;
        set_pos_and_mc();
        return;
    }

    const size_t slen = sn - st;
    if (slen < 3) {
        set_pos_and_mc();
        return;
    }

    uint8_t mood_d = d[sn - 1];
    bool valid_imperf_mood = (mood_d == DIAC_DAMMA || mood_d == DIAC_FATHA || mood_d == DIAC_SUKUN) ||
                             (mood_d == DIAC_NONE && (s[sn - 1] == S_YEH || s[sn - 1] == S_WAW || s[sn - 1] == S_ALEF_MAKSURA));
    const bool is_proper_dip_a = (s[st] == S_ALEF_HAMZA_A) && (classify_diptote_surface(s, sn, st) == 2);
    if (!is_proper_dip_a && is_imperf_prefix(s[st]) && valid_imperf_mood) {
        if (s[st] == S_YEH ||
            (d[st] == DIAC_DAMMA && d[st + 1] == DIAC_FATHA) ||
            (d[st + 1] == DIAC_SUKUN && slen >= 4 && s[sn - 2] != S_YEH && s[sn - 2] != S_ALEF) ||
            (d[st] == DIAC_DAMMA && slen >= 4 && (d[sn - 2] == DIAC_FATHA || d[sn - 2] == DIAC_SHADDA_FATHA))) {
            c.verb_pattern = 1;
            if (has_noun_lex && is_diptote_stem(s, sn, st, d)) c.ext_flags |= EF_DIPTOTE;
            if (d[st] == DIAC_DAMMA && slen >= 4 &&
                (d[sn - 2] == DIAC_FATHA || d[sn - 2] == DIAC_SHADDA_FATHA ||
                 (s[sn - 2] == S_ALEF && slen >= 5 && d[sn - 3] == DIAC_FATHA))) {
                c.ext_flags |= EF_PASSIVE;
            }
            set_pos_and_mc();
            return;
        }
        if ((s[st] == S_TEH || s[st] == S_NOON) && d[st] == DIAC_FATHA && slen == 3 &&
            (s[sn - 1] == S_YEH || s[sn - 1] == S_ALEF_MAKSURA)) {
            c.verb_pattern = 1;
            set_pos_and_mc();
            return;
        }
    }

    if (slen >= 5 && is_imperf_prefix(s[st]) &&
        (((s[sn - 2] == S_WAW || s[sn - 2] == S_WAW_HAMZA) && s[sn - 1] == S_NOON && d[sn - 1] == DIAC_FATHA) ||
         (s[sn - 2] == S_ALEF && s[sn - 1] == S_NOON && d[sn - 1] == DIAC_KASRA))) {
        c.verb_pattern = 1;
        if (d[st] == DIAC_DAMMA && (d[sn - 4] == DIAC_FATHA || d[sn - 4] == DIAC_SHADDA_FATHA)) {
            c.ext_flags |= EF_PASSIVE;
        }
        set_pos_and_mc();
        return;
    }

    if (slen == 3 && (d[st + 2] == DIAC_FATHA || d[st + 2] == DIAC_SHADDA_FATHA) &&
        (d[st] == DIAC_FATHA || d[st] == DIAC_DAMMA) &&
        (d[st + 1] == DIAC_FATHA || d[st + 1] == DIAC_KASRA || d[st + 1] == DIAC_DAMMA ||
         d[st + 1] == DIAC_SHADDA_FATHA || d[st + 1] == DIAC_SHADDA_KASRA || d[st + 1] == DIAC_NONE)) {
        if (d[st] == DIAC_FATHA && d[st + 1] == DIAC_FATHA && d[st + 2] == DIAC_FATHA && has_noun_lex) {
            c.verb_pattern = 2;
        } else {
            c.verb_pattern = 1;
        }
        if (d[st] == DIAC_DAMMA && (d[st + 1] == DIAC_KASRA || d[st + 1] == DIAC_SHADDA_KASRA ||
                                    (s[st + 1] == S_YEH && d[st + 1] == DIAC_NONE))) {
            c.ext_flags |= EF_PASSIVE;
        }
    } else if (slen == 3 && !si.has_prep && !has_noun_lex && !is_five_noun_head(s, sn) &&
               d[st] == DIAC_FATHA && d[st + 1] == DIAC_FATHA &&
               (s[st + 2] == S_ALEF_MAKSURA || s[st + 2] == S_ALEF)) {
        c.verb_pattern = 1;
    } else if (slen == 3 && (s[st + 1] == S_ALEF || s[st + 1] == S_YEH) &&
               (d[st] == DIAC_FATHA || d[st] == DIAC_KASRA) && d[st + 2] == DIAC_FATHA) {
        c.verb_pattern = 1;
        if (s[st + 1] == S_YEH && d[st] == DIAC_KASRA) c.ext_flags |= EF_PASSIVE;
    } else if (slen == 4 && !si.has_prep && s[st] != S_MEEM &&
               (s[st + 3] == S_ALEF_MAKSURA || s[st + 3] == S_ALEF) && d[st + 3] == DIAC_NONE &&
               d[st] == DIAC_FATHA &&
               (d[st + 1] == DIAC_SUKUN || d[st + 1] == DIAC_FATHA || d[st + 1] == DIAC_SHADDA_FATHA ||
                s[st + 1] == S_ALEF) &&
               (d[st + 2] == DIAC_FATHA || d[st + 2] == DIAC_SHADDA_FATHA)) {
        // Form II/III/IV weak-final past verbs (أَلْقَى، أَعْطَى، صَلَّى، نَادَى، بَنَى)
        c.verb_pattern = 1;
    } else if (slen == 4 && (d[st + 3] == DIAC_FATHA || d[st + 3] == DIAC_SHADDA_FATHA) &&
               (d[st] == DIAC_FATHA || d[st] == DIAC_DAMMA) &&
               (d[st + 2] == DIAC_FATHA || d[st + 2] == DIAC_SHADDA_FATHA || d[st + 2] == DIAC_KASRA ||
                (s[st + 2] == S_ALEF && d[st + 1] == DIAC_FATHA) ||
                (s[st + 2] == S_YEH && d[st + 1] == DIAC_KASRA) ||
                (s[st + 1] == S_ALEF && d[st + 2] == DIAC_FATHA))) {
        if (d[st] == DIAC_DAMMA && (d[st + 2] == DIAC_KASRA || (s[st + 2] == S_YEH && d[st + 1] == DIAC_KASRA))) {
            c.verb_pattern = 1;
            c.ext_flags |= EF_PASSIVE;
        } else if (d[st + 2] == DIAC_FATHA || d[st + 2] == DIAC_SHADDA_FATHA || s[st + 2] == S_ALEF || s[st + 1] == S_ALEF) {
            c.verb_pattern = 1;
        }
    } else if (slen >= 5 && (d[sn - 1] == DIAC_FATHA || d[sn - 1] == DIAC_SHADDA_FATHA ||
                             (s[sn - 1] == S_ALEF_MAKSURA && d[sn - 2] == DIAC_FATHA)) &&
               (s[st] == S_ALEF || s[st] == S_TEH || s[st] == S_ALEF_HAMZA_A) &&
               (d[sn - 2] == DIAC_FATHA || d[sn - 2] == DIAC_SHADDA_FATHA || s[sn - 2] == S_ALEF ||
                (d[st] == DIAC_DAMMA && d[sn - 2] == DIAC_KASRA))) {
        c.verb_pattern = 1;
        if (d[st] == DIAC_DAMMA && d[sn - 2] == DIAC_KASRA) c.ext_flags |= EF_PASSIVE;
    } else if ((slen >= 4 || (slen == 3 && !has_noun_lex && s[st + 1] != S_WAW && s[st + 1] != S_YEH && s[st] != S_ALEF_HAMZA_A)) &&
               s[sn - 1] == S_TEH &&
               (d[sn - 1] == DIAC_SUKUN || d[sn - 1] == DIAC_KASRA || d[sn - 1] == DIAC_DAMMA || d[sn - 1] == DIAC_FATHA) &&
               (d[st] == DIAC_FATHA || d[st] == DIAC_DAMMA || (slen == 3 && d[st] == DIAC_KASRA && d[sn - 2] == DIAC_SUKUN))) {
        if (slen >= 4 && (d[sn - 1] == DIAC_SUKUN || (d[sn - 1] == DIAC_KASRA && d[sn - 2] == DIAC_FATHA))) {
            c.verb_pattern = 1;
            c.ext_flags |= EF_PAST_TEH_FEM;
            if (d[st] == DIAC_DAMMA && d[sn - 2] == DIAC_FATHA && (slen == 4 ? d[st + 1] == DIAC_KASRA : true)) {
                c.ext_flags |= EF_PASSIVE;
            }
        } else if (d[sn - 2] == DIAC_SUKUN && (d[sn - 1] == DIAC_DAMMA || d[sn - 1] == DIAC_FATHA || d[sn - 1] == DIAC_KASRA)) {
            c.verb_pattern = 1;
        }
    } else if (slen >= 5 && s[sn - 2] == S_TEH && s[sn - 1] == S_MEEM && d[sn - 2] == DIAC_DAMMA &&
               (d[sn - 1] == DIAC_SUKUN || d[sn - 1] == DIAC_DAMMA) && (d[st] == DIAC_FATHA || d[st] == DIAC_DAMMA)) {
        c.verb_pattern = 1;
    } else if (slen >= 5 && s[sn - 2] == S_NOON && s[sn - 1] == S_ALEF && d[sn - 2] == DIAC_FATHA &&
               d[sn - 1] == DIAC_NONE && d[sn - 3] == DIAC_SUKUN && (d[st] == DIAC_FATHA || d[st] == DIAC_DAMMA)) {
        c.verb_pattern = 1;
    } else if (slen >= 4 && s[sn - 1] == S_ALEF && s[sn - 2] == S_WAW && d[sn - 3] == DIAC_DAMMA &&
               (d[st] == DIAC_FATHA || d[st] == DIAC_DAMMA)) {
        c.verb_pattern = 1;
        if (d[st] == DIAC_DAMMA && slen >= 5 && d[sn - 3] == DIAC_DAMMA) c.ext_flags |= EF_PASSIVE;
    } else if (slen >= 4 && s[sn - 1] == S_ALEF && s[sn - 2] != S_WAW && d[sn - 1] == DIAC_NONE &&
               d[sn - 2] == DIAC_FATHA && d[st] == DIAC_FATHA) {
        c.verb_pattern = 1;
    }

    if (c.verb_pattern == 0 && !(c.ext_flags & EF_DIPTOTE) && st == 0 && sn >= 4 &&
        (s[0] == S_BEH || s[0] == S_LAM || s[0] == S_KAF) && (d[0] == DIAC_KASRA || s[0] == S_KAF) &&
        is_diptote_stem(s, sn, 1, d)) {
        c.ext_flags |= EF_DIPTOTE;
    }
    if (c.verb_pattern == 0 && is_diptote_stem(s, sn, st, d)) {
        c.ext_flags |= EF_DIPTOTE;
    }
    set_pos_and_mc();
}

void Diacritizer::emit_stem(Ctx& ctx, Token& tok, uint32_t stem_id, size_t pre_len, const uint8_t* pre_diac,
                            bool has_al, size_t suf_idx, int32_t penalty, uint8_t extra_flags,
                            int repl_pos, uint8_t repl_sym) const {
    const uint8_t* s = &ctx.syms[tok.off];
    const size_t n = tok.n;
    const size_t stem_len = meta_word_len(stem_id);
    const SufRule* suf = suf_idx == NO_SUFFIX ? nullptr : &SUFFIXES[suf_idx];
    const size_t suf_len = suf ? suf->len : 0;
    if (pre_len + stem_len + suf_len != n || stem_len == 0) return;
    const size_t stem_last = pre_len + stem_len - 1;

    const uint32_t c_start = meta_cand_start(stem_id);
    uint16_t cnt = std::min<uint16_t>(meta_cand_count(stem_id), MAX_STEM_CANDS);
    for (uint16_t ci = 0; ci < cnt; ++ci) {
        const WordCandidate wc = read_cand(c_start + ci);
        Cand c;
        std::memset(c.diac, 0, sizeof(c.diac));
        for (size_t j = 0; j < pre_len; ++j) c.diac[j] = pre_diac[j];
        for (size_t k = 0; k + 1 < stem_len; ++k) c.diac[pre_len + k] = read_core_diac(wc.core_offset, k);
        c.diac[stem_last] = wc.ending_id;
        c.flags = extra_flags;
        c.repl_pos = 255;
        c.repl_sym = 0;
        if (repl_pos >= 0 && restore_spelling_) {
            uint8_t orig = s[pre_len + repl_pos];
            bool hamza_fix = orig == S_ALEF;
            if (hamza_fix || !suf) {
                c.repl_pos = static_cast<uint8_t>(pre_len + repl_pos);
                c.repl_sym = repl_sym;
            }
        }

        size_t cpos = stem_last;
        if (!suf && stem_len >= 3 && s[stem_last] == S_ALEF) cpos = stem_last - 1;

        if (tok.si.particle == PART_REL) {
            for (size_t j = 0; j < n && j < 3; ++j) {
                if (is_alef_or_wasla(s[j])) c.diac[j] = DIAC_NONE;
            }
        }
        if (has_al || tok.si.al_len >= 2) {
            c.flags |= CF_HAS_AL;
            c.diac[cpos] = strip_tanween(c.diac[cpos]);
            if (diac_is_tanween(c.diac[stem_last])) c.diac[stem_last] = strip_tanween(c.diac[stem_last]);
            size_t sun_pos = has_al ? pre_len : tok.si.al_len;
            size_t lam_pos = has_al ? (pre_len - 1) : tok.si.al_lam_pos;
            if (sun_pos < n && lam_pos < sun_pos) {
                if (lam_pos >= 1 && is_alef_or_wasla(s[lam_pos - 1])) {
                    c.diac[lam_pos - 1] = DIAC_NONE; // Never put fatha/vowel on the alef of الـ (e.g. وَالَّذِي)
                }
                if (is_sun_letter(s[sun_pos])) {
                    c.diac[lam_pos] = DIAC_NONE;
                    c.diac[sun_pos] = add_shadda(c.diac[sun_pos]);
                } else {
                    if (diac_has_shadda(c.diac[sun_pos])) {
                        uint8_t d = c.diac[sun_pos];
                        c.diac[sun_pos] = make_diac(diac_family(d), diac_is_tanween(d), false);
                    }
                    if (c.diac[lam_pos] != DIAC_NONE && c.diac[lam_pos] != DIAC_SUKUN) {
                        c.diac[lam_pos] = DIAC_NONE;
                    }
                }
            }
        }

        if (suf) {
            c.flags |= CF_SUFFIX;
            uint8_t v = strip_tanween(c.diac[stem_last]);
            if (suf->mode == 2) v = make_diac(FAM_KASRA, false, diac_has_shadda(v));
            c.diac[stem_last] = v;
            for (size_t j = 0; j < suf_len; ++j) c.diac[stem_last + 1 + j] = suf->diacs[j];
            if (suf->mode == 1) {
                uint8_t fam = diac_family(v);
                if (fam == FAM_KASRA || (s[stem_last] == S_YEH && (fam == FAM_NONE || fam == FAM_SUKUN))) {
                    c.diac[stem_last + 1] = DIAC_KASRA;
                }
            }
            c.case_pos = static_cast<uint8_t>(stem_last);
        }

        if (stem_len >= 2 && !is_alef_or_wasla(s[pre_len]) && pre_len > 0 && c.diac[pre_len] == DIAC_SUKUN && !has_al) {
            continue;
        }

        c.emis = static_cast<int32_t>((static_cast<int64_t>(wc.log_prob) * weights_[W_LEX]) / 1000) - penalty;
        finalize_cand(c, s, n, tok.si, tok.has_noun_lex);
        ctx.cands.push_back(c);
        ++tok.cand_count;
    }
}

bool Diacritizer::analyze_morph(Ctx& ctx, Token& tok) const {
    const uint8_t* s = &ctx.syms[tok.off];
    const size_t n = tok.n;
    static const uint8_t CONJS[3] = {0, S_WAW, S_FEH};
    static const uint8_t MIDS[5] = {0, S_BEH, S_KAF, S_LAM, S_SEEN};
    uint8_t stem_buf[MAX_WORD_LEN];
    const uint32_t start_count = tok.cand_count;

    for (uint8_t conj : CONJS) {
        for (uint8_t mid : MIDS) {
            for (int al = 0; al < 2; ++al) {
                if (mid == S_SEEN && al) continue;
                uint8_t p[5];
                uint8_t pd[5];
                size_t pl = 0;
                int morphemes = 0;
                if (conj) { p[pl] = conj; pd[pl++] = DIAC_FATHA; ++morphemes; }
                if (mid) { p[pl] = mid; pd[pl++] = proclitic_diac(mid == S_SEEN ? static_cast<uint8_t>(S_KAF) : mid); ++morphemes; }
                if (al) {
                    if (mid != S_LAM) { p[pl] = S_ALEF; pd[pl++] = DIAC_NONE; }
                    p[pl] = S_LAM; pd[pl++] = DIAC_NONE;
                    ++morphemes;
                }
                if (pl + 2 > n) continue;
                bool match = true;
                for (size_t j = 0; j < pl; ++j) {
                    if (s[j] == p[j]) continue;
                    if (p[j] == S_ALEF && s[j] == S_ALEF_WASLA) continue;
                    match = false;
                    break;
                }
                if (!match) continue;
                if (mid == S_SEEN && !is_imperf_prefix(s[pl])) continue;

                for (size_t si = 0; si <= NUM_SUFFIXES; ++si) {
                    size_t suf_idx = si == NUM_SUFFIXES ? NO_SUFFIX : si;
                    if (pl == 0 && suf_idx == NO_SUFFIX) continue;
                    size_t sl = suf_idx == NO_SUFFIX ? 0 : SUFFIXES[suf_idx].len;
                    if (n < pl + sl + 2) continue;
                    size_t stem_len = n - pl - sl;
                    bool sm = true;
                    for (size_t j = 0; j < sl; ++j) if (s[n - sl + j] != SUFFIXES[suf_idx].syms[j]) { sm = false; break; }
                    if (!sm) continue;
                    if (al && suf_idx != NO_SUFFIX) continue;

                    std::memcpy(stem_buf, s + pl, stem_len);
                    int rp = -1;
                    uint8_t rs = 0;
                    uint32_t wid = UINT32_MAX;
                    if (sl > 0 && stem_buf[stem_len - 1] == S_TEH) {
                        stem_buf[stem_len - 1] = S_TEH_MARBUTA;
                        wid = fst_.lookup(stem_buf, stem_len);
                        if (wid == UINT32_MAX || wid >= vocab_count_) {
                            wid = UINT32_MAX;
                            stem_buf[stem_len - 1] = S_TEH;
                        }
                    } else if (sl > 0 && stem_buf[stem_len - 1] == S_ALEF) {
                        stem_buf[stem_len - 1] = S_ALEF_MAKSURA;
                        wid = fst_.lookup(stem_buf, stem_len);
                        if (wid == UINT32_MAX || wid >= vocab_count_) {
                            wid = UINT32_MAX;
                            stem_buf[stem_len - 1] = S_ALEF;
                        }
                    }
                    if (wid == UINT32_MAX) wid = lookup_relaxed(stem_buf, stem_len, rp, rs);
                    if (wid == UINT32_MAX) continue;
                    if (meta_word_len(wid) != stem_len) continue;

                    uint8_t flags = 0;
                    if (mid == S_BEH || mid == S_KAF) flags |= CF_GEN;
                    if (mid == S_LAM) {
                        if (!al && is_imperf_prefix(s[pl])) flags |= CF_LAM_NASB;
                        else flags |= CF_GEN;
                    }
                    if (mid == S_SEEN) flags |= CF_FUT;

                    int32_t pen = weights_[W_PROCLITIC_PEN] * morphemes +
                                  (sl ? weights_[W_SUFFIX_PEN] : 0) + (rp >= 0 ? weights_[W_RELAXED_PEN] : 0);
                    emit_stem(ctx, tok, wid, pl, pd, al != 0, suf_idx, pen, flags, rp, rs);
                }
            }
        }
    }

    // Farasa-style fallback for OOV plural/dual/feminine-past+pronoun inflections
    if (tok.cand_count == start_count && n >= 6) {
        size_t pl = tok.si.al_len;
        bool has_al_only = (pl == 2 && !tok.si.has_prep && !tok.si.has_conj);
        size_t body_len = n - pl;
        // 1. Sound feminine plural: stem + ات (when stem or stem+ة is in FST)
        if (body_len >= 5 && s[n - 2] == S_ALEF && s[n - 1] == S_TEH && (pl == 0 || has_al_only)) {
            size_t base_len = body_len - 2;
            std::memcpy(stem_buf, s + pl, base_len);
            uint32_t wid = fst_.lookup(stem_buf, base_len);
            if (wid == UINT32_MAX) {
                stem_buf[base_len] = S_TEH_MARBUTA;
                uint32_t wid_t = fst_.lookup(stem_buf, base_len + 1);
                if (wid_t != UINT32_MAX && wid_t < vocab_count_) {
                    wid = wid_t;
                }
            }
            if (wid != UINT32_MAX && wid < vocab_count_ && meta_word_len(wid) >= base_len) {
                const uint32_t cs = meta_cand_start(wid);
                const WordCandidate wc = read_cand(cs);
                size_t wlen = meta_word_len(wid);
                static const uint8_t END_AL[2] = {DIAC_DAMMA, DIAC_KASRA};
                static const uint8_t END_INDEF[4] = {DIAC_DAMMA, DIAC_KASRA, DIAC_DAMMATAN, DIAC_KASRATAN};
                const uint8_t* ends = has_al_only ? END_AL : END_INDEF;
                const size_t n_ends = has_al_only ? 2 : 4;
                for (size_t ei = 0; ei < n_ends; ++ei) {
                    uint8_t end_d = ends[ei];
                    Cand c;
                    std::memset(c.diac, 0, sizeof(c.diac));
                    if (has_al_only) {
                        c.diac[0] = DIAC_NONE;
                        c.diac[1] = is_sun_letter(s[2]) ? DIAC_NONE : DIAC_SUKUN;
                    }
                    for (size_t k = 0; k + 1 < base_len && k + 1 < wlen; ++k) {
                        c.diac[pl + k] = read_core_diac(wc.core_offset, k);
                    }
                    if (has_al_only && is_sun_letter(s[2])) c.diac[2] = add_shadda(c.diac[2]);
                    c.diac[pl + base_len - 1] = DIAC_FATHA;
                    c.diac[n - 2] = DIAC_NONE;
                    c.diac[n - 1] = end_d;
                    c.flags = has_al_only ? CF_HAS_AL : 0;
                    c.repl_pos = 255;
                    c.repl_sym = 0;
                    c.emis = static_cast<int32_t>((static_cast<int64_t>(wc.log_prob) * weights_[W_LEX]) / 1000) - weights_[W_SUFFIX_PEN];
                    finalize_cand(c, s, n, tok.si, 1);
                    ctx.cands.push_back(c);
                    ++tok.cand_count;
                }
            }
        }
    }

    uint32_t added = tok.cand_count - start_count;
    if (added > MAX_CANDS) {
        auto b = ctx.cands.begin() + tok.cand_begin + start_count;
        std::partial_sort(b, b + MAX_CANDS, b + added, [](const Cand& a, const Cand& c) { return a.emis > c.emis; });
        ctx.cands.resize(tok.cand_begin + start_count + MAX_CANDS);
        tok.cand_count = start_count + MAX_CANDS;
    }
    return tok.cand_count > start_count;
}

void Diacritizer::build_oov(Ctx& ctx, Token& tok) const {
    const uint8_t* s = &ctx.syms[tok.off];
    const size_t n = tok.n;
    const SurfaceInfo& si = tok.si;
    const size_t cpos = (n >= 3 && s[n - 1] == S_ALEF) ? n - 2 : n - 1;
    const int32_t w_c = weights_[W_CONSTRAINT];

    uint16_t mask[MAX_WORD_LEN];
    for (size_t i = 0; i < n; ++i) {
        uint16_t m = MASK_ALL;
        const uint8_t c = s[i];
        if (i < si.al_len) {
            if (i == si.al_lam_pos && !is_sun_letter(s[si.al_len])) {
                m = BIT(DIAC_NONE) | BIT(DIAC_SUKUN);
            } else {
                m = BIT(si.pre_diac[i]);
            }
        } else {
            if (si.al_len > 0) {
                m &= static_cast<uint16_t>(~MASK_TAN);
                if (i == si.al_len) {
                    if (is_sun_letter(c)) m &= MASK_SH_VOWEL;
                    else m &= static_cast<uint16_t>(~MASK_SH);
                }
            }
            if (i == 0) {
                bool allow_loan_suk = (n >= 4 && si.al_len == 0 &&
                    s[0] != S_ALEF && s[0] != S_ALEF_HAMZA_A && s[0] != S_ALEF_HAMZA_B &&
                    s[0] != S_WAW && s[0] != S_FEH && s[0] != S_BEH && s[0] != S_LAM && s[0] != S_KAF &&
                    (s[1] == S_REH || s[1] == S_LAM || s[1] == S_YEH || s[1] == S_WAW || s[1] == S_BEH || s[1] == S_TEH));
                m &= static_cast<uint16_t>(allow_loan_suk ? ~MASK_SH : ~(BIT(DIAC_SUKUN) | MASK_SH));
                if (n > 1) m &= static_cast<uint16_t>(~MASK_TAN);
            }
            if (i == 1 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM) && n >= 4 && !is_alef_or_wasla(c)) {
                m &= static_cast<uint16_t>(~BIT(DIAC_SUKUN));
            }
            if (i != cpos) m &= static_cast<uint16_t>(~MASK_TAN);
            if (si.verb_like == 2) m &= static_cast<uint16_t>(~MASK_TAN);
            if (c == S_ALEF || c == S_ALEF_MADDA || c == S_ALEF_MAKSURA || c == S_ALEF_WASLA) {
                m = BIT(DIAC_NONE);
            } else if (c == S_TEH_MARBUTA) {
                m &= (i == n - 1) ? MASK_CASE : BIT(DIAC_NONE);
            } else if (i + 1 < n) {
                uint8_t nx = s[i + 1];
                bool al_alef_next = si.al_len >= 2 && i + 1 == static_cast<size_t>(si.al_len - 2);
                bool plural_waw_alef = (c == S_WAW && nx == S_ALEF && i + 1 == n - 1 && n >= 4);
                if (plural_waw_alef) {
                    m = BIT(DIAC_NONE) | BIT(DIAC_FATHA) | BIT(DIAC_FATHATAN);
                } else if ((nx == S_ALEF && !al_alef_next) ||
                    (i + 1 == n - 1 && (nx == S_TEH_MARBUTA || nx == S_ALEF_MAKSURA))) {
                    uint16_t fm = MASK_FATHA_ONLY;
                    if (i == cpos && nx == S_ALEF && i + 1 == n - 1) fm |= BIT(DIAC_FATHATAN) | BIT(DIAC_SHADDA_FATHATAN);
                    if (si.al_len > 0) fm &= static_cast<uint16_t>(~MASK_TAN);
                    uint16_t mm = m & fm;
                    m = mm ? mm : BIT(DIAC_FATHA);
                }
            }
            if (c == S_ALEF_HAMZA_B && i == 0) m &= BIT(DIAC_KASRA);
            if (c == S_ALEF_HAMZA_A && i == 0) m &= BIT(DIAC_FATHA) | BIT(DIAC_DAMMA);
            if (m == 0) m = BIT(DIAC_NONE);
        }
        mask[i] = m;
    }

    Cand base;
    std::memset(base.diac, 0, sizeof(base.diac));
    base.flags = CF_OOV | (si.al_len ? CF_HAS_AL : 0) | (si.has_prep ? CF_GEN : 0);
    base.repl_pos = 255;
    base.repl_sym = 0;
    int32_t case_scores[NUM_DIACRITICS];
    for (auto& v : case_scores) v = 0;

    int32_t feats[OOV_NUM_FEATURES];
    const int32_t stem_len = static_cast<int32_t>(n) - si.al_len;
    const int32_t is_sun_al = (si.al_len >= 2 && si.al_len < n && is_sun_letter(s[si.al_len])) ? 1 : 0;
    const int32_t starts_meem = (si.al_len < n && s[si.al_len] == S_MEEM) ? 1 : 0;
    const int32_t starts_imp = si.verb_like;
    const int32_t ends_tm = (s[n - 1] == S_TEH_MARBUTA) ? 1 : 0;
    const int32_t ends_at = (n >= 4 && s[n - 2] == S_ALEF && s[n - 1] == S_TEH) ? 1 : 0;
    const int32_t ends_wa = (n >= 4 && s[n - 2] == S_WAW && s[n - 1] == S_ALEF) ? 1 : 0;

    for (size_t i = 0; i < n; ++i) {
        int32_t sc[NUM_DIACRITICS];
        for (auto& v : sc) v = 0;
        if (oov_trees_.valid()) {
            feats[0] = i >= 3 ? s[i - 3] : -1;
            feats[1] = i >= 2 ? s[i - 2] : -1;
            feats[2] = i >= 1 ? s[i - 1] : -1;
            feats[3] = s[i];
            feats[4] = i + 1 < n ? s[i + 1] : -1;
            feats[5] = i + 2 < n ? s[i + 2] : -1;
            feats[6] = i + 3 < n ? s[i + 3] : -1;
            feats[7] = static_cast<int32_t>(i);
            feats[8] = static_cast<int32_t>(n - 1 - i);
            feats[9] = static_cast<int32_t>(n);
            feats[10] = si.al_len;
            feats[11] = static_cast<int32_t>(i) - si.al_len;
            feats[12] = i >= 1 ? base.diac[i - 1] : -1;
            feats[13] = i >= 2 ? base.diac[i - 2] : -1;
            feats[14] = stem_len;
            feats[15] = s[n - 1];
            if (oov_feat_count_ >= OOV_NUM_FEATURES) {
                feats[16] = is_sun_al;
                feats[17] = starts_meem;
                feats[18] = starts_imp;
                feats[19] = ends_tm;
                feats[20] = ends_at;
                feats[21] = ends_wa;
            }
            oov_trees_.predict_scores(feats, oov_feat_count_, sc);
            for (auto& v : sc) v = static_cast<int32_t>((static_cast<int64_t>(v) * weights_[W_OOV_TREE]) / 1000);
        } else {
            sc[DIAC_FATHA] = 300;
            sc[DIAC_SUKUN] = 100;
            sc[DIAC_KASRA] = 50;
        }
        if (i > 0 && base.diac[i - 1] == DIAC_SUKUN && i >= si.al_len) {
            sc[DIAC_SUKUN] -= 3 * w_c;
        }
        if (i > 0 && i + 1 < n && i != cpos) {
            if (s[i] == S_WAW && base.diac[i - 1] == DIAC_DAMMA) sc[DIAC_NONE] += 2 * w_c;
            if (s[i] == S_YEH && base.diac[i - 1] == DIAC_KASRA) sc[DIAC_NONE] += 2 * w_c;
        }
        if (i > 0 && i + 1 == n - 1 && s[i] == S_WAW && s[i + 1] == S_ALEF && n >= 4 &&
            diac_family(base.diac[i - 1]) == FAM_DAMMA) {
            sc[DIAC_NONE] += 3 * w_c;
        }
        // Foreign transliterations starting with وِيـ / فِيسـ (e.g. وِيكِيمِيدْيَا، وِيكِيمَانْيَا، فِيسْبُوك)
        if (i == 0 && n >= 6 && si.al_len == 0 && si.verb_like == 0 &&
            s[0] == S_WAW && s[1] == S_YEH &&
            ((s[n - 2] == S_YEH && s[n - 1] == S_ALEF) || (s[2] == S_KAF && s[3] == S_YEH)) &&
            fst_.lookup(s + 1, n - 1) == UINT32_MAX) {
            sc[DIAC_KASRA] += 3 * w_c;
        }
        if (i == 1 && n >= 6 && si.al_len == 0 && si.verb_like == 0 &&
            s[0] == S_WAW && s[1] == S_YEH && base.diac[0] == DIAC_KASRA) {
            sc[DIAC_NONE] += 3 * w_c;
        }
        if (i == cpos) {
            for (uint8_t d = 0; d < NUM_DIACRITICS; ++d) case_scores[d] = sc[d];
            base.diac[i] = DIAC_NONE;
            continue;
        }
        uint8_t best = DIAC_NONE;
        int32_t best_s = NEG_INF;
        for (uint8_t d = 0; d < NUM_DIACRITICS; ++d) {
            if (!(mask[i] & BIT(d))) continue;
            if (sc[d] > best_s) { best_s = sc[d]; best = d; }
        }
        base.diac[i] = best;
    }

    uint16_t cm = mask[cpos];
    static const uint8_t CASE_ORDER[] = {DIAC_DAMMA, DIAC_FATHA, DIAC_KASRA, DIAC_SUKUN, DIAC_DAMMATAN,
                                         DIAC_FATHATAN, DIAC_KASRATAN, DIAC_SHADDA_DAMMA, DIAC_SHADDA_FATHA,
                                         DIAC_SHADDA_KASRA, DIAC_NONE};
    std::pair<int32_t, uint8_t> ranked[16];
    size_t rc = 0;
    for (uint8_t d : CASE_ORDER) {
        if (!(cm & BIT(d))) continue;
        ranked[rc++] = {case_scores[d], d};
    }
    if (rc == 0) {
        for (uint8_t d = 0; d < NUM_DIACRITICS && rc < 16; ++d) if (cm & BIT(d)) ranked[rc++] = {case_scores[d], d};
    }
    std::sort(ranked, ranked + rc, [](const auto& a, const auto& b) { return a.first > b.first; });
    size_t keep = std::min<size_t>(rc, 7);
    if (keep == 0) {
        ranked[0] = {0, DIAC_NONE};
        keep = 1;
    }
    for (size_t k = 0; k < keep; ++k) {
        Cand c = base;
        c.diac[cpos] = ranked[k].second;
        if (cpos + 1 < n) c.diac[cpos + 1] = DIAC_NONE;
        c.emis = ranked[k].first;
        finalize_cand(c, s, n, si, 0);
        ctx.cands.push_back(c);
        ++tok.cand_count;
    }
}

void Diacritizer::expand_cases(Ctx& ctx, Token& tok) const {
    const uint8_t* s = &ctx.syms[tok.off];
    const size_t n = tok.n;
    if (n < 1) return;

    const bool declinable_part = (tok.si.particle == PART_IDAFA_NOUN || tok.si.particle == PART_NUM_IDAFA || tok.si.particle == PART_AY);
    if (tok.si.particle != PART_NONE && !declinable_part) {
        const uint32_t orig = tok.cand_count;
        const bool plural_m = is_plural_meem_pron(s, n);
        const bool man_w = (tok.surf_flags & SF_MAN) != 0;
        for (uint32_t k = 0; k < orig && tok.cand_count < MAX_CANDS + 2; ++k) {
            const Cand src = ctx.cands[tok.cand_begin + k];
            if (src.diac[n - 1] != DIAC_SUKUN) continue;
            if (plural_m) {
                Cand c = src;
                c.diac[n - 1] = DIAC_DAMMA;
                c.flags |= CF_ILTIQA | CF_EXPANDED;
                c.emis = src.emis - weights_[W_EXPAND_PEN] / 3;
                ctx.cands.push_back(c);
                ++tok.cand_count;
                continue;
            }
            uint8_t alts[2] = {DIAC_KASRA, DIAC_FATHA};
            size_t na = man_w ? 2 : 1;
            for (size_t a = 0; a < na; ++a) {
                Cand c = src;
                c.diac[n - 1] = alts[a];
                c.flags |= CF_ILTIQA | CF_EXPANDED;
                c.emis = src.emis - weights_[W_EXPAND_PEN] / 3;
                ctx.cands.push_back(c);
                ++tok.cand_count;
            }
        }
        return;
    }
    if (n < 2) return;
    const uint32_t orig = tok.cand_count;
    for (uint32_t k = 0; k < orig; ++k) {
        if (tok.cand_count >= MAX_CANDS + 4) break;
        const Cand src = ctx.cands[tok.cand_begin + k];
        if (src.flags & (CF_OOV | CF_EXPANDED)) continue;

        const size_t cp = src.case_pos;
        if (src.verb_pattern == 1) {
            if (cp == n - 1 && (s[cp] == S_YEH || s[cp] == S_WAW) && src.diac[cp] == DIAC_NONE &&
                starts_like_imperf(s, n)) {
                Cand c = src;
                c.diac[cp] = DIAC_FATHA;
                c.verb_mood_diac = DIAC_FATHA;
                c.flags |= CF_EXPANDED;
                c.emis = src.emis - weights_[W_EXPAND_PEN] / 3;
                ctx.cands.push_back(c);
                ++tok.cand_count;
            }
            if (is_plural_meem_pron(s, n) && src.diac[n - 1] == DIAC_SUKUN) {
                Cand c = src;
                c.diac[n - 1] = DIAC_DAMMA;
                c.flags |= CF_ILTIQA | CF_EXPANDED;
                c.emis = src.emis - weights_[W_EXPAND_PEN] / 3;
                ctx.cands.push_back(c);
                ++tok.cand_count;
            }
            if ((src.ext_flags & EF_PAST_TEH_FEM) && cp == n - 1 && src.diac[cp] == DIAC_SUKUN) {
                Cand c = src;
                c.diac[cp] = DIAC_KASRA;
                c.flags |= CF_ILTIQA | CF_EXPANDED;
                c.emis = src.emis - weights_[W_EXPAND_PEN] / 3;
                ctx.cands.push_back(c);
                ++tok.cand_count;
            }
            continue;
        }

        if (cp + 1 < n && !(src.flags & CF_SUFFIX)) continue;
        if (src.ext_flags & (EF_DUAL_NOON | EF_MASC_PL_NOON)) continue;
        if (s[cp] == S_ALEF || s[cp] == S_ALEF_MAKSURA) continue;
        if (n == 3 && (src.flags & CF_SUFFIX)) continue;

        // Defective nouns ending in ـي after kasra without shadda (e.g. الباقي / القاضي): only NONE or FATHA
        if (is_defective_yeh_noun(s, n, cp, src.diac, src.verb_pattern)) {
            uint8_t alt_d = (src.diac[cp] == DIAC_NONE) ? DIAC_FATHA : DIAC_NONE;
            if (alt_d == DIAC_FATHA && !(src.flags & CF_HAS_AL) && tok.si.al_len == 0) continue;
            Cand c = src;
            c.diac[cp] = alt_d;
            c.verb_mood_diac = alt_d;
            c.flags |= CF_EXPANDED;
            c.emis = src.emis - weights_[W_EXPAND_PEN] / 2;
            bool dup = false;
            for (uint32_t j = 0; j < tok.cand_count; ++j) {
                if (std::memcmp(ctx.cands[tok.cand_begin + j].diac, c.diac, n) == 0) { dup = true; break; }
            }
            if (!dup) {
                ctx.cands.push_back(c);
                ++tok.cand_count;
            }
            continue;
        }

        const uint8_t d = src.diac[cp];
        const uint8_t fam = diac_family(d);
        if (fam == FAM_NONE || fam == FAM_SUKUN) {
            if (fam == FAM_SUKUN && cp == n - 1) {
                if (is_plural_meem_pron(s, n)) {
                    Cand c = src;
                    c.diac[cp] = DIAC_DAMMA;
                    c.flags |= CF_ILTIQA | CF_EXPANDED;
                    c.emis = src.emis - weights_[W_EXPAND_PEN] / 3;
                    ctx.cands.push_back(c);
                    ++tok.cand_count;
                } else if (src.ext_flags & EF_PAST_TEH_FEM) {
                    Cand c = src;
                    c.diac[cp] = DIAC_KASRA;
                    c.flags |= CF_ILTIQA | CF_EXPANDED;
                    c.emis = src.emis - weights_[W_EXPAND_PEN] / 3;
                    ctx.cands.push_back(c);
                    ++tok.cand_count;
                }
            }
            continue;
        }
        const bool tan = diac_is_tanween(d);
        const bool sh = diac_has_shadda(d);
        auto push_variant = [&](uint8_t nd, int32_t extra) {
            Cand c = src;
            c.diac[cp] = nd;
            if ((src.flags & CF_SUFFIX) && cp + 1 < n && s[cp + 1] == S_HEH &&
                (c.diac[cp + 1] == DIAC_DAMMA || c.diac[cp + 1] == DIAC_KASRA)) {
                c.diac[cp + 1] = diac_family(nd) == FAM_KASRA ? DIAC_KASRA : DIAC_DAMMA;
            }
            for (uint32_t j = 0; j < tok.cand_count; ++j) {
                const Cand& e = ctx.cands[tok.cand_begin + j];
                if (std::memcmp(e.diac, c.diac, n) == 0) return;
            }
            c.verb_mood_diac = nd;
            c.flags |= CF_EXPANDED;
            int32_t exp_pen = weights_[W_EXPAND_PEN];
            if ((src.ext_flags & EF_DIPTOTE) && !tan && !(src.flags & CF_HAS_AL) &&
                (diac_family(nd) == FAM_KASRA || diac_family(nd) == FAM_FATHA)) {
                exp_pen = exp_pen / 4;
            }
            c.emis = src.emis - exp_pen - extra;
            ctx.cands.push_back(c);
            ++tok.cand_count;
        };
        for (uint8_t f : {FAM_DAMMA, FAM_FATHA, FAM_KASRA}) {
            if (f == fam) continue;
            if (tok.cand_count >= MAX_CANDS + 4) break;
            push_variant(make_diac(f, tan, sh), 0);
        }
        if (tan && tok.cand_count < MAX_CANDS + 4) {
            for (uint8_t f : {FAM_DAMMA, FAM_FATHA, FAM_KASRA}) {
                if (tok.cand_count >= MAX_CANDS + 4) break;
                push_variant(make_diac(f, false, sh), weights_[W_EXPAND_PEN] / 3);
            }
        }
    }
}

void Diacritizer::build_candidates(Ctx& ctx, Token& tok) const {
    tok.cand_begin = static_cast<uint32_t>(ctx.cands.size());
    tok.cand_count = 0;
    tok.has_noun_lex = 0;
    const uint8_t* s = &ctx.syms[tok.off];
    const size_t n = tok.n;
    tok.si = analyze_surface(s, n);
    tok.surf_flags = compute_surf_flags(s, n, tok.si);
    tok.is_fem_at = is_sound_fem_plural_stem(s, n, tok.si.al_len) ? 1 : 0;
    tok.is_diptote_lex = classify_diptote_surface(s, n, tok.si.al_len);
    tok.is_ibn = (tok.surf_flags & SF_IBN) ? 1 : 0;
    tok.is_adj_like = is_adj_surface(s, n, tok.si.al_len) ? 1 : 0;

    int rp = -1;
    uint8_t rs = 0;
    uint32_t wid = lookup_relaxed(s, n, rp, rs);
    if (wid != UINT32_MAX && meta_word_len(wid) == n) {
        tok.word_id = wid;
        tok.role = morph_to_role(meta_role_flags(wid));
        const uint32_t cs = meta_cand_start(wid);
        const uint16_t cnt = std::min<uint16_t>(meta_cand_count(wid), MAX_STEM_CANDS);
        for (uint16_t ci = 0; ci < cnt; ++ci) {
            uint8_t eid = read_cand(cs + ci).ending_id;
            if (diac_is_tanween(eid) || diac_family(eid) == FAM_KASRA) {
                tok.has_noun_lex = 1;
                break;
            }
        }
        uint8_t pre_dummy[1] = {0};
        bool al = tok.si.al_len > 0 && tok.si.al_len == 2;
        emit_stem(ctx, tok, wid, 0, pre_dummy, false, NO_SUFFIX, rp >= 0 ? weights_[W_RELAXED_PEN] : 0, 0, rp, rs);
        if (al) {
            for (uint32_t k = 0; k < tok.cand_count; ++k) ctx.cands[tok.cand_begin + k].flags |= CF_HAS_AL;
        }
        if (tok.cand_count > 0) {
            const Cand& tc = ctx.cands[tok.cand_begin];
            tok.lex_top_end = tc.diac[tc.case_pos];
            // Detect 3+ char attached prepositions (بِـ/لِـ/كَـ) even when the full surface word is in the FST
            const bool valid_prep_proclitic =
                ((s[0] == S_BEH || s[0] == S_LAM) && tc.diac[0] == DIAC_KASRA) ||
                (s[0] == S_KAF && tc.diac[0] == DIAC_FATHA);
            if (tok.si.particle == PART_NONE && tok.si.al_len == 0 && n >= 3 && valid_prep_proclitic &&
                !tc.verb_pattern && tc.diac[n - 1] != DIAC_NONE &&
                fst_.lookup(s + 1, n - 1) != UINT32_MAX) {
                tok.si.has_prep = 1;
                for (uint32_t k = 0; k < tok.cand_count; ++k) {
                    Cand& ck = ctx.cands[tok.cand_begin + k];
                    bool ok_p0 = (s[0] == S_KAF) ? (ck.diac[0] == DIAC_FATHA) : (ck.diac[0] == DIAC_KASRA);
                    if (ok_p0 && !ck.verb_pattern) ck.flags |= CF_GEN;
                }
            }
            if (rp >= 0 && rp == 0 && tok.si.particle == PART_NONE) {
                uint8_t fixed[MAX_WORD_LEN];
                std::memcpy(fixed, s, n);
                fixed[rp] = rs;
                SurfaceInfo fsi = analyze_surface(fixed, n);
                if (fsi.particle != PART_NONE) tok.si.particle = fsi.particle;
            }
            expand_cases(ctx, tok);
            return;
        }
    }

    if (analyze_morph(ctx, tok)) {
        tok.role = morph_to_role((tok.si.al_len ? MORPH_HAS_AL : 0) | (tok.si.verb_like == 2 ? MORPH_VERB_LIKE : 0));
        tok.lex_top_end = ctx.cands[tok.cand_begin].diac[ctx.cands[tok.cand_begin].case_pos];
        expand_cases(ctx, tok);
        return;
    }

    tok.role = morph_to_role((tok.si.al_len ? MORPH_HAS_AL : 0) | (tok.si.verb_like == 2 ? MORPH_VERB_LIKE : 0));
    build_oov(ctx, tok);
}

int32_t Diacritizer::unary_rules(const Ctx& ctx, size_t idx, const Cand& c) const {
    const int32_t B = 1500;
    int32_t r = 0;
    const Token& tok = ctx.toks[idx];
    const uint8_t* s = &ctx.syms[tok.off];
    const uint8_t d = c.diac[c.case_pos];
    const uint8_t fam = diac_family(d);
    const size_t n = tok.n;
    const Token* pt = (idx >= 1 && !tok.boundary_before) ? &ctx.toks[idx - 1] : nullptr;
    const Token* nx = (idx + 1 < ctx.toks.size() && !ctx.toks[idx + 1].boundary_before) ? &ctx.toks[idx + 1] : nullptr;
    const bool nx_wasl = nx && ((nx->surf_flags & SF_WASL) != 0);

    if ((c.flags & CF_HAS_AL) && diac_is_tanween(d)) r -= 4 * B;
    if (tok.si.particle == PART_IDAFA_NOUN && !tok.digit_after && !(nx && nx->si.particle == PART_GEN) &&
        diac_is_tanween(d)) r -= 2 * B;
    if ((c.ext_flags & EF_DIPTOTE) && diac_is_tanween(d)) r -= 3 * B + B / 2;
    if ((c.ext_flags & EF_DIPTOTE) && !(c.flags & CF_HAS_AL) && tok.is_diptote_lex == 2 && fam == FAM_KASRA) r -= 4 * B;
    if ((c.ext_flags & EF_FEM_PL_AT) && c.case_pos == n - 1 && fam == FAM_FATHA) r -= 4 * B;
    if ((c.ext_flags & EF_DUAL_NOON) && (fam == FAM_DAMMA || fam == FAM_FATHA)) r -= 3 * B;
    if ((c.ext_flags & EF_MASC_PL_NOON) && (fam == FAM_KASRA || fam == FAM_DAMMA)) r -= 3 * B;
    if (is_defective_yeh_noun(s, n, c.case_pos, c.diac, c.verb_pattern)) {
        if (fam == FAM_KASRA || fam == FAM_DAMMA || diac_is_tanween(d)) r -= 4 * B;
    }

    if ((c.flags & CF_GEN) && c.verb_pattern == 1 && !(c.ext_flags & EF_DIPTOTE)) r -= 4 * B;

    const bool terminal_alef_locked = !(c.flags & CF_SUFFIX) && static_cast<size_t>(c.case_pos) + 1 < n && s[c.case_pos + 1] == S_ALEF;
    if (terminal_alef_locked && s[c.case_pos] == S_YEH && fam == FAM_KASRA) r -= 4 * B;
    const bool comp_num_pair = (nx && (tok.surf_flags & SF_COMP_NUM) && (nx->surf_flags & SF_ASHAR)) ||
                               (pt && (tok.surf_flags & SF_ASHAR) && (pt->surf_flags & SF_COMP_NUM));

    const bool nx_mudaf_ilayh = nx && nx->si.particle == PART_NONE && nx->si.verb_like < 2 &&
                                nx->top_verb != 1 && !nx->is_adj_like && !nx->is_ibn && !nx->si.has_prep;
    const bool proper_dip = (tok.is_diptote_lex == 2) ||
                            (classify_diptote_stem(s, n, tok.si.al_len, c.diac) == 2);
    if (!terminal_alef_locked && !comp_num_pair && ((c.flags & CF_GEN) || (tok.si.has_prep && tok.si.al_len > 0))) {
        if (c.ext_flags & EF_DIPTOTE) {
            if (!(c.flags & CF_HAS_AL)) {
                if (proper_dip || !nx_mudaf_ilayh) {
                    if (fam == FAM_FATHA && !diac_is_tanween(d)) r += 2 * B;
                    else if (fam != FAM_NONE) r -= 2 * B;
                } else {
                    if (fam == FAM_KASRA) r += 2 * B;
                    else if (fam != FAM_NONE) r -= B;
                }
            } else {
                if (fam == FAM_KASRA) r += B + B / 2;
                else if (fam != FAM_NONE) r -= 2 * B;
            }
        } else {
            if (fam == FAM_KASRA) r += B + B / 2;
            else if (fam != FAM_NONE) r -= 2 * B;
        }
    } else if ((c.ext_flags & EF_DIPTOTE) && !(c.flags & CF_HAS_AL) && pt &&
               (pt->si.particle == PART_GEN || pt->si.particle == PART_IDAFA_NOUN || pt->si.particle == PART_NUM_IDAFA || pt->si.has_prep)) {
        if (proper_dip || !nx_mudaf_ilayh) {
            if (fam == FAM_FATHA && !diac_is_tanween(d)) r += 2 * B;
            else if (fam == FAM_KASRA || diac_is_tanween(d)) r -= 2 * B;
        } else {
            if (fam == FAM_KASRA && !diac_is_tanween(d)) r += 2 * B;
            else if (fam == FAM_FATHA || diac_is_tanween(d)) r -= B;
        }
    }
    if (c.flags & CF_LAM_NASB) {
        if (c.verb_pattern == 1 && starts_like_imperf(s + 1, n - 1)) {
            if (fam == FAM_FATHA && !diac_is_tanween(d)) r += B + B / 2;
            else if (fam != FAM_NONE) r -= B;
        } else if (c.verb_pattern == 1) {
            r -= 3 * B;
        }
    }
    if (c.flags & CF_FUT) {
        if (d == DIAC_DAMMA) r += B / 2;
        if (diac_is_tanween(d) || fam == FAM_KASRA) r -= B;
    }

    const bool nx_al = nx && nx->si.al_len > 0 && !nx->si.has_prep && !nx->si.has_conj;
    const bool is_man_w = (tok.surf_flags & SF_MAN) != 0;
    if (is_man_w && c.diac[n - 2] == DIAC_KASRA) {
        if (nx_al) {
            if (d == DIAC_KASRA) r -= 3 * B;
        } else if (nx_wasl) {
            if (d == DIAC_KASRA) r += B;
            else r -= 3 * B;
        } else {
            if (d == DIAC_SUKUN) r += B;
            else r -= 6 * B;
        }
    } else if (is_man_w && c.diac[n - 2] == DIAC_FATHA) {
        if (!nx_wasl && d == DIAC_KASRA) r -= 4 * B;
    } else if (c.ext_flags & EF_PLURAL_MEEM) {
        if (d == DIAC_KASRA) r -= 4 * B;
        if (!nx_wasl) {
            if (d == DIAC_SUKUN) r += B / 2;
            else if (d == DIAC_DAMMA) r -= 5 * B;
        }
    } else if (c.ext_flags & EF_PAST_TEH_FEM) {
        if (c.case_pos == n - 1) {
            if (!nx_wasl) {
                if (d == DIAC_SUKUN) r += B;
                else if (d == DIAC_KASRA) r -= 5 * B;
            }
        }
    } else if (tok.si.particle != PART_NONE &&
               (tok.si.particle == PART_GEN || tok.si.particle == PART_JAZM || tok.si.particle == PART_QAD ||
                tok.si.particle == PART_CONJ || tok.si.particle == PART_COND || tok.si.particle == PART_QUEST ||
                tok.si.particle == PART_IN) &&
               s[n - 1] != S_YEH && s[n - 1] != S_ALEF && s[n - 1] != S_ALEF_MAKSURA) {
        if (!nx_wasl && (d == DIAC_KASRA || (c.flags & CF_ILTIQA))) {
            r -= 5 * B;
        }
    } else if (c.flags & CF_ILTIQA) {
        if (!nx_wasl) r -= 5 * B;
    }

    if ((tok.surf_flags & SF_AMS) && !(c.flags & CF_HAS_AL)) {
        if (d == DIAC_KASRA) r += 2 * B;
        else r -= 3 * B;
    }

    if ((tok.surf_flags & SF_FIVE_NOUN) && nx && nx->si.particle == PART_NONE && diac_is_tanween(d)) {
        r -= 4 * B;
    }

    if (tok.si.particle == PART_NUM_IDAFA && nx &&
        (nx->si.particle == PART_NONE || nx->si.particle == PART_NUM_IDAFA) &&
        nx->si.verb_like < 2 && !(nx->surf_flags & SF_ASHAR) && diac_is_tanween(d)) {
        r -= 2 * B + B / 2;
    }

    if (tok.surf_flags & SF_BAAD_QABL) {
        bool is_qabl_or_baad_stem = (c.diac[n - 3] == DIAC_FATHA && c.diac[n - 2] == DIAC_SUKUN);
        bool after_amma = pt && pt->si.particle == PART_AMMA;
        bool before_feh = nx && ctx.syms[nx->off] == S_FEH && nx->n >= 2 && nx->si.particle != PART_NONE;
        if ((after_amma || !nx || before_feh) && is_qabl_or_baad_stem) {
            if (d == DIAC_DAMMA) r += 2 * B;
            else if (d == DIAC_FATHA) r -= B;
        }
    }

    if (nx && (tok.surf_flags & SF_COMP_NUM) && (nx->surf_flags & SF_ASHAR)) {
        if (d == DIAC_FATHA) r += 2 * B;
        else r -= 2 * B;
    }
    if (pt && (tok.surf_flags & SF_ASHAR) && (pt->surf_flags & SF_COMP_NUM)) {
        if (d == DIAC_FATHA) r += 2 * B;
        else r -= 2 * B;
    }

    if (nx && (nx->si.particle == PART_AN || nx->si.particle == PART_LI_AN) && (c.ext_flags & EF_PASSIVE)) {
        r += B / 2;
    }

    if (!(c.flags & (CF_HAS_AL | CF_SUFFIX | CF_OOV)) && c.verb_pattern == 0 && !tok.top_verb &&
        !(tok.surf_flags & (SF_BAAD_QABL | SF_AMS)) &&
        !(c.ext_flags & (EF_DIPTOTE | EF_PAST_TEH_FEM | EF_PLURAL_MEEM | EF_DUAL_NOON | EF_MASC_PL_NOON)) &&
        tok.si.particle == PART_NONE && tok.si.verb_like < 2 &&
        fam != FAM_NONE && fam != FAM_SUKUN && s[n - 1] != S_ALEF_MAKSURA && s[n - 1] != S_ALEF &&
        !(pt && (pt->si.particle == PART_LA || pt->si.particle == PART_YA))) {
        bool end_or_part = (idx + 1 == ctx.toks.size()) || ctx.toks[idx + 1].boundary_before ||
                           (nx && (nx->si.has_prep ||
                                   nx->si.particle == PART_GEN || nx->si.particle == PART_CONJ ||
                                   nx->si.particle == PART_INNA || nx->si.particle == PART_LI_AN ||
                                   nx->si.particle == PART_LAKIN || nx->si.particle == PART_LAMA ||
                                   nx->si.particle == PART_AY || nx->si.particle == PART_KANA ||
                                   nx->si.particle == PART_PREP_PRON || nx->si.particle == PART_AMMA ||
                                   nx->si.particle == PART_ILLA || nx->si.particle == PART_NASB ||
                                   nx->si.particle == PART_JAZM || nx->si.particle == PART_AN ||
                                   nx->si.particle == PART_IN || nx->si.particle == PART_LA ||
                                   nx->si.particle == PART_QAD || nx->si.particle == PART_SAWFA ||
                                   nx->si.particle == PART_REL || nx->si.particle == PART_COND ||
                                   nx->si.particle == PART_HATTA || nx->si.particle == PART_DEM ||
                                   nx->si.particle == PART_PRON ||
                                   nx->si.verb_like == 2 || nx->top_verb == 1));
        bool before_ibn = nx && ((nx->surf_flags & SF_IBN) != 0);
        if (tok.digit_after) {
            if (diac_is_tanween(d)) r -= 2 * B;
        } else if (end_or_part && !before_ibn) {
            bool has_tan = false;
            for (uint32_t k = 0; k < tok.cand_count; ++k) {
                const Cand& oc = ctx.cands[tok.cand_begin + k];
                if (diac_is_tanween(oc.diac[oc.case_pos])) { has_tan = true; break; }
            }
            if (has_tan) {
                if (diac_is_tanween(d)) r += B;
                else r -= B;
            }
        }
    }

    if ((c.flags & CF_OOV) && tok.si.verb_like == 2) {
        if (fam == FAM_KASRA || diac_is_tanween(d)) r -= B;
    }

    const bool is_idh_word = pt && pt->si.particle == PART_COND && pt->n <= 3 &&
                             (ctx.syms[pt->off] == S_ALEF_HAMZA_B || ctx.syms[pt->off] == S_ALEF) &&
                             ctx.syms[pt->off + pt->n - 1] == 15; // إِذْ / اذ
    const bool clause_head_cond = pt && (pt->si.particle == PART_AMMA || is_idh_word);
    if ((tok.boundary_before || clause_head_cond) && tok.si.particle == PART_NONE) {
        const bool looks_bos_noun = (c.verb_pattern == 0) ||
            (c.verb_pattern == 2 && nx && (nx->si.al_len > 0 || nx->has_noun_lex));
        if (!(c.flags & CF_GEN) && !(tok.si.has_prep && tok.si.al_len > 0) &&
            looks_bos_noun && tok.top_verb != 1 && tok.si.verb_like < 2 && !(c.ext_flags & EF_PAST_TEH_FEM)) {
            if (fam == FAM_DAMMA) r += (c.ext_flags & EF_DIPTOTE) ? (B + B / 2) : (B / 2);
            else if (fam == FAM_KASRA) r -= B + B / 2;
            else if ((c.ext_flags & EF_DIPTOTE) && tok.boundary_before && fam == FAM_FATHA) r -= B;
        } else if ((c.flags & CF_HAS_AL) && !tok.si.has_prep && !(c.flags & CF_GEN)) {
            if (fam == FAM_DAMMA) r += B / 2;
            else if (fam == FAM_KASRA) r -= B;
        }
        if ((tok.si.verb_like == 2 || c.verb_pattern == 1) && fam == FAM_DAMMA) r += B / 3;
    } else if (tok.si.has_conj && s[0] == S_FEH && tok.si.al_len >= 3 && !tok.si.has_prep &&
               !(c.flags & CF_GEN) && pt && pt->si.particle == PART_NONE &&
               diac_family(pt->lex_top_end) != FAM_KASRA) {
        // فَالْمَجْمُوعُ / فَالْبَدَنَانِ (jawab / clause-resumption nominal subject)
        if (fam == FAM_DAMMA) r += B / 2;
        else if (fam == FAM_KASRA) r -= B / 2;
    }
    return static_cast<int32_t>((static_cast<int64_t>(r) * weights_[W_UNARY_RULE]) / 1000);
}

int32_t Diacritizer::binary_rules(const Ctx& ctx, const Token* p3, const Token* p2, const Token& pt, const Cand& pc,
                                  const Token& ct, const Cand& cc, const Token* nx) const {
    if (ct.boundary_before) return 0;
    const int32_t B = 1500;
    int32_t r = 0;
    const uint8_t* ps = &ctx.syms[pt.off];
    const uint8_t* cs = &ctx.syms[ct.off];
    const uint8_t cd = cc.diac[cc.case_pos];
    const uint8_t cf = diac_family(cd);
    const bool ctan = diac_is_tanween(cd);
    const uint8_t pd = pc.diac[pc.case_pos];
    const uint8_t pf = diac_family(pd);
    const bool ptan = diac_is_tanween(pd);
    const uint8_t pp = pt.si.particle;
    const uint8_t cp = ct.si.particle;
    const bool c_al = (cc.flags & CF_HAS_AL) || ct.si.al_len > 0;
    const bool p_al = (pc.flags & CF_HAS_AL) || pt.si.al_len > 0;
    const uint8_t vmd = cc.verb_mood_diac;
    const uint8_t vmf = diac_family(vmd);
    const bool vmtan = diac_is_tanween(vmd);
    const bool c_is_idafa_head = (cp == PART_IDAFA_NOUN || cp == PART_NUM_IDAFA);
    const bool c_part = cp != PART_NONE && !c_is_idafa_head;
    const bool c_verb = (cc.verb_pattern == 1) || (cc.verb_pattern == 2 && ct.top_verb == 1) ||
                        (ct.si.verb_like == 2 && !c_al && !ctan && cf != FAM_KASRA && vmf != FAM_KASRA && !vmtan);
    const bool c_imperf = ((cc.verb_pattern == 1) && starts_like_imperf(cs, ct.n)) ||
                          (ct.si.verb_like == 2 && !c_al && !c_part && !ctan && !vmtan &&
                           cf != FAM_KASRA && vmf != FAM_KASRA && cs[ct.n - 1] != S_TEH_MARBUTA);
    const bool c_noun = !c_part && !c_verb && cc.verb_pattern != 2 && !(cc.flags & (CF_GEN | CF_LAM_NASB | CF_FUT));
    const bool c_own_gen = (cc.flags & CF_GEN) || ct.si.has_prep;
    const bool c_diptote = (cc.ext_flags & EF_DIPTOTE) != 0;
    const bool c_fem_at = (cc.ext_flags & EF_FEM_PL_AT) != 0;

    const bool prev_is_man = (pt.surf_flags & SF_MAN) != 0;
    const bool prev_is_man_pron = prev_is_man && pc.diac[pt.n - 2] == DIAC_FATHA;

    // 1. Register & Lunar AL sukun consistency
    if (p_al && c_al && pt.si.al_len >= 2 && ct.si.al_len >= 2 &&
        !is_sun_letter(ps[pt.si.al_len]) && !is_sun_letter(cs[ct.si.al_len])) {
        bool p_suk = (pc.ext_flags & EF_LAM_SUKUN) != 0;
        bool c_suk = (cc.ext_flags & EF_LAM_SUKUN) != 0;
        if (p_suk == c_suk) r += B / 2;
        else r -= B / 2;
    }
    if (prev_is_man && !prev_is_man_pron && c_al && ct.si.al_len >= 2 && !is_sun_letter(cs[ct.si.al_len])) {
        bool c_suk = (cc.ext_flags & EF_LAM_SUKUN) != 0;
        if (c_suk && pd == DIAC_FATHA) r += B;
        else if (!c_suk && pd == DIAC_SUKUN) r += B / 2;
    }
    if ((pc.ext_flags & EF_PAST_TEH_FEM) && pc.diac[pt.n - 1] == DIAC_KASRA && c_al) {
        if (cc.ext_flags & EF_LAM_SUKUN) r += B / 2;
    }

    // 2. على vs عليّ and أم vs أمّ
    if (ct.surf_flags & SF_ALI) {
        bool c_is_ala = (ct.surf_flags & SF_ALA) && cc.diac[ct.n - 1] == DIAC_NONE;
        bool next_ibn_or_rad = nx && ((nx->surf_flags & SF_IBN) ||
                                      (nx->n >= 3 && ctx.syms[nx->off] == S_REH && ctx.syms[nx->off + 1] == S_DAD));
        if (next_ibn_or_rad || (pc.verb_pattern == 1 && !c_is_ala && diac_has_shadda(cd))) {
            if (c_is_ala) r -= 2 * B;
            else if (diac_has_shadda(cd)) r += B + B / 2;
        }
        if (next_ibn_or_rad && ctan) r -= 3 * B;
    }
    if (ct.surf_flags & SF_AM) {
        bool c_is_um = diac_has_shadda(cd) && cc.diac[0] == DIAC_DAMMA;
        bool next_muminin = nx && nx->si.al_len > 0;
        bool next_walad = nx && nx->n >= 3 && ctx.syms[nx->off] == S_WAW && ctx.syms[nx->off + 1] == S_LAM;
        if ((pp == PART_KANA && c_is_um) || ((next_muminin || next_walad) && c_is_um)) {
            r += B + B / 2;
        }
    }

    // 3. من (مَنْ vs مِنْ) and من ثمَّ
    if (prev_is_man) {
        if (ct.surf_flags & SF_THEMMA) {
            if (!prev_is_man_pron && cc.diac[0] == DIAC_FATHA) r += 4 * B;
            else r -= 4 * B;
        }
        if (prev_is_man_pron && !(ct.surf_flags & SF_THEMMA)) {
            if (c_verb || c_imperf || cp == PART_JAZM || cp == PART_KANA || cp == PART_QAD || cp == PART_LA) r += 2 * B;
            else if (c_noun && (cf == FAM_KASRA || (p2 && (p2->surf_flags & SF_QUANT_IDAFA)))) {
                r -= (p2 && (p2->surf_flags & SF_QUANT_IDAFA)) ? (4 * B) : (2 * B);
            }
        } else {
            if (c_verb || c_imperf) r -= 2 * B;
            else if (c_noun) {
                if (cc.ext_flags & (EF_MASC_PL_NOON | EF_DUAL_NOON)) {
                    r += B;
                } else if (c_diptote && !c_al) {
                    const bool c_proper_dip = (ct.is_diptote_lex == 2);
                    const bool nx_mudaf = !c_proper_dip && nx && nx->si.particle == PART_NONE &&
                                          nx->si.verb_like < 2 && nx->top_verb != 1 && !nx->is_adj_like && !nx->is_ibn && !nx->si.has_prep;
                    if (nx_mudaf) {
                        if (cf == FAM_KASRA) r += B + B / 2;
                    } else {
                        if (cf == FAM_FATHA && !ctan) r += B + B / 2;
                        else r -= B;
                    }
                } else if (cf == FAM_KASRA) {
                    r += B + B / 2;
                } else if (cf != FAM_NONE && !(cc.flags & CF_OOV)) {
                    r -= B;
                }
            }
        }
    }

    // 4. Active vs Passive & Subject-Verb Gender Agreement
    if (cc.verb_pattern == 1 && nx) {
        const bool nx_is_al = nx->si.al_len > 0 && !nx->si.has_prep;
        const bool nx_is_fem_noun = nx->n >= 3 && (ctx.syms[nx->off + nx->n - 1] == S_TEH_MARBUTA || nx->is_fem_at);
        if ((cc.ext_flags & EF_PASSIVE) && nx->si.particle == PART_NONE && !nx->si.has_prep && nx->si.verb_like < 2) {
            uint8_t nxd = nx->lex_top_end;
            if (nxd == DIAC_FATHATAN || nxd == DIAC_SHADDA_FATHATAN) r -= B + B / 2;
        }
        if (ct.n >= 4 && cs[ct.n - 1] == S_TEH && (cc.ext_flags & EF_PAST_TEH_FEM)) {
            if (nx_is_al || nx_is_fem_noun) r += B;
        }
    }

    const bool c_terminal_alef_locked = !(cc.flags & CF_SUFFIX) && cc.case_pos + 1 < ct.n && cs[cc.case_pos + 1] == S_ALEF;
    if (!c_own_gen && !prev_is_man_pron) {
        switch (pp) {
            case PART_GEN:
                if (c_verb && !(ct.n >= 3 && cs[0] == S_WAW)) r -= 3 * B;
                if (is_an_prep_word(ps, pt.n) && is_bare_baad(cs, ct.n)) {
                    if (cc.diac[0] == DIAC_DAMMA && cf == FAM_KASRA) r += 4 * B;
                    else r -= 4 * B;
                } else if (c_noun && !c_terminal_alef_locked) {
                    const bool c_proper_dip = (ct.is_diptote_lex == 2);
                    const bool nx_mudaf_target = !c_proper_dip && nx && nx->si.particle == PART_NONE &&
                                                 nx->si.verb_like < 2 && nx->top_verb != 1 &&
                                                 !nx->is_adj_like && !nx->is_ibn && !nx->si.has_prep;
                    if (cc.ext_flags & (EF_MASC_PL_NOON | EF_DUAL_NOON)) {
                        r += B;
                    } else if (c_diptote && !c_al && !nx_mudaf_target) {
                        if (cf == FAM_FATHA && !ctan) r += 2 * B;
                        else if (cf == FAM_KASRA || ctan) r -= B + B / 2;
                    } else if (c_diptote && !c_al && nx_mudaf_target) {
                        if (cf == FAM_KASRA) r += 2 * B;
                        else if (cf != FAM_NONE) r -= B;
                    } else {
                        if (cf == FAM_KASRA) r += B + B / 2;
                        else if (cf != FAM_NONE) r -= B;
                    }
                } else if (cp == PART_AY) {
                    if (cf == FAM_KASRA) r += B;
                }
                break;

            case PART_HATTA:
                if (c_imperf || c_verb || cp == PART_LA) {
                    if (c_imperf || c_verb) {
                        if (vmd == DIAC_FATHA) r += 2 * B;
                        else if (vmf != FAM_NONE) r -= 2 * B;
                    }
                } else if (c_noun) {
                    if (cc.ext_flags & (EF_MASC_PL_NOON | EF_DUAL_NOON)) {
                        r += B;
                    } else if (c_diptote && !c_al && (ct.is_diptote_lex == 2 || !nx || nx->si.particle != PART_NONE || nx->is_adj_like)) {
                        if (cf == FAM_FATHA && !ctan) r += B + B / 2;
                        else if (cf == FAM_KASRA || ctan) r -= B;
                    } else {
                        if (cf == FAM_KASRA) r += B + B / 2;
                        else if (cf != FAM_NONE) r -= B;
                    }
                }
                break;

            case PART_IDAFA_NOUN:
            case PART_NUM_IDAFA:
                if (c_noun && !c_part) {
                    if (!c_terminal_alef_locked) {
                        if (cc.ext_flags & (EF_MASC_PL_NOON | EF_DUAL_NOON)) {
                            r += B;
                        } else if (c_diptote && !c_al && (ct.is_diptote_lex == 2 || !nx || nx->si.particle != PART_NONE || nx->is_adj_like)) {
                            if (cf == FAM_FATHA && !ctan) r += B + B / 2;
                            else r -= B;
                        } else {
                            if (cf == FAM_KASRA) r += B + B / 2;
                            else if (cf != FAM_NONE) r -= B;
                        }
                    }
                    if (ptan) r -= 2 * B;
                }
                break;

            case PART_AY: {
                bool ay_sh = diac_has_shadda(pd);
                bool next_is_indef_gen_target = c_noun && !c_al && !c_part && cf == FAM_KASRA &&
                                                (!p2 || p2->si.particle == PART_GEN || p2->si.particle == PART_IDAFA_NOUN || pf != FAM_KASRA);
                if (c_verb || cp == PART_GEN || cp == PART_CONJ || cp == PART_LA || cp == PART_AN ||
                    cp == PART_INNA || cp == PART_PREP_PRON || c_al ||
                    (c_noun && cf == FAM_DAMMA && (!nx || nx->si.particle == PART_NONE))) {
                    if (!ay_sh && pd == DIAC_SUKUN) r += 2 * B;
                    else if (!ay_sh) r -= B;
                }
                if (ay_sh && c_noun && !c_al && !c_part) {
                    if (cf == FAM_KASRA) r += B + B / 2;
                    else if (cf == FAM_DAMMA || cf == FAM_FATHA) r -= B;
                } else if (next_is_indef_gen_target) {
                    if (ay_sh) r += B;
                    else r -= B;
                }
                if (ay_sh && pf == FAM_KASRA && (!p2 || (p2->si.particle != PART_GEN && p2->si.particle != PART_IDAFA_NOUN &&
                    !p2->si.has_prep))) {
                    r -= 3 * B;
                }
                break;
            }

            case PART_LAMA: {
                bool lama_sh = diac_has_shadda(pc.diac[pt.n - 2]);
                bool prev_comp_or_conj = !p2 || (p2->surf_flags & SF_INNA_PRON) ||
                                         p2->si.particle == PART_CONJ || p2->si.particle == PART_LI_AN ||
                                         p2->si.particle == PART_INNA || p2->si.particle == PART_AN ||
                                         p2->si.particle == PART_COND;
                if (c_verb && !c_imperf) {
                    if (lama_sh) r += prev_comp_or_conj ? (2 * B) : (B + B / 2);
                    else if (prev_comp_or_conj) r -= 2 * B;
                } else if (c_imperf || cp == PART_JAZM) {
                    if (lama_sh) r += B;
                } else if (cp == PART_AN || cp == PART_GEN || cp == PART_PREP_PRON) {
                    if (!lama_sh) r += B + B / 2;
                }
                break;
            }

            case PART_PREP_PRON:
                if (c_noun && !c_part) {
                    if (cf == FAM_KASRA) r -= 2 * B;
                    else if (cf == FAM_DAMMA && (!p2 || p2->si.particle == PART_NONE)) r += B / 2;
                }
                break;

            case PART_ILLA:
                if (c_noun && !c_part && cf == FAM_KASRA) r -= B;
                break;

            case PART_AMMA:
                if (c_noun && !c_part && !(ct.surf_flags & SF_BAAD_QABL)) {
                    if (cf == FAM_DAMMA) r += B + B / 2;
                    else if (cf == FAM_KASRA) r -= 2 * B;
                }
                break;

            case PART_AN:
            case PART_LI_AN: {
                bool an_sh = diac_has_shadda(pd);
                bool next_verbish = c_imperf || c_verb || cp == PART_LA || cp == PART_JAZM ||
                                    cp == PART_KANA || cp == PART_QAD;
                if (next_verbish) {
                    if (!an_sh) {
                        r += 2 * B;
                        if (c_imperf || c_verb) {
                            if (vmd == DIAC_FATHA) r += B + B / 2;
                            else if (vmf == FAM_DAMMA || vmf == FAM_SUKUN || vmf == FAM_KASRA || vmtan) r -= B + B / 2;
                        }
                    } else {
                        r -= 2 * B + B / 2;
                    }
                } else if (c_noun || cp == PART_DEM || cp == PART_PRON || cp == PART_REL ||
                           cp == PART_PREP_PRON || cp == PART_GEN || c_own_gen) {
                    if (an_sh) {
                        r += 2 * B;
                        if (c_noun && !c_own_gen) {
                            if (c_fem_at) {
                                if (cf == FAM_KASRA) r += B + B / 2;
                                else r -= B + B / 2;
                            } else {
                                if (cf == FAM_FATHA) r += B + B / 2;
                                else if (cf == FAM_DAMMA || cf == FAM_KASRA) r -= B;
                            }
                        }
                    } else {
                        r -= 2 * B;
                    }
                }
                break;
            }

            case PART_NASB:
                if (c_imperf || c_verb) {
                    if (vmd == DIAC_FATHA) r += 2 * B;
                    else if (vmf != FAM_NONE) r -= 2 * B;
                }
                break;

            case PART_JAZM:
                if (c_imperf || c_verb) {
                    if (vmf == FAM_SUKUN) r += 2 * B;
                    else if (vmf == FAM_DAMMA || vmtan) r -= 4 * B;
                    else if (vmf == FAM_FATHA && cs[ct.n - 1] != S_ALEF_MAKSURA) r -= B;
                }
                break;

            case PART_INNA:
            case PART_LAKIN: {
                bool in_sh = diac_has_shadda(pd);
                bool next_verbish = c_imperf || c_verb || cp == PART_JAZM || cp == PART_NASB ||
                                    cp == PART_COND || cp == PART_QAD;
                if (pp == PART_LAKIN && (next_verbish || cp == PART_GEN || c_own_gen)) {
                    if (!in_sh) r += 2 * B;
                    else r -= 2 * B;
                } else if (c_noun || cp == PART_DEM || cp == PART_PRON || cp == PART_REL ||
                           cp == PART_PREP_PRON || (pp == PART_INNA && cp == PART_GEN)) {
                    if (in_sh) r += 2 * B;
                    else if (pp == PART_LAKIN) r -= 2 * B;
                    if (c_noun && !c_own_gen) {
                        if (c_fem_at) {
                            if (cf == FAM_KASRA) r += B + B / 2;
                            else r -= B + B / 2;
                        } else {
                            if (cf == FAM_FATHA) r += B + B / 2;
                            else if (cf == FAM_DAMMA || cf == FAM_KASRA) r -= B;
                        }
                    }
                }
                break;
            }

            case PART_IN: {
                bool in_sh = diac_has_shadda(pd);
                bool next_verbish = c_imperf || c_verb || cp == PART_KANA || cp == PART_LA ||
                                    cp == PART_JAZM || cp == PART_QAD;
                if (next_verbish) {
                    if (!in_sh) {
                        r += 2 * B;
                        if ((c_imperf || c_verb) && vmf == FAM_SUKUN) r += B;
                    } else {
                        r -= 2 * B + B / 2;
                    }
                } else if (c_noun || cp == PART_DEM || cp == PART_PRON || cp == PART_PREP_PRON || cp == PART_GEN) {
                    if (in_sh) {
                        r += 2 * B;
                        if (c_noun && !c_own_gen) {
                            if (c_fem_at) {
                                if (cf == FAM_KASRA) r += B + B / 2;
                                else r -= B + B / 2;
                            } else {
                                if (cf == FAM_FATHA) r += B + B / 2;
                                else if (cf == FAM_DAMMA || cf == FAM_KASRA) r -= B;
                            }
                        }
                    } else {
                        r -= 2 * B;
                    }
                }
                break;
            }

            case PART_KANA:
                if (c_noun) {
                    if (c_al || (cc.flags & CF_SUFFIX)) {
                        if (cf == FAM_DAMMA) r += B + B / 2;
                        else if (cf == FAM_KASRA) r -= B;
                    } else {
                        if (c_fem_at) {
                            if (cf == FAM_KASRA) r += B / 2;
                        } else if (!nx || nx->si.particle != PART_NONE) {
                            if (cf == FAM_FATHA) r += B + B / 2;
                            else if (cf == FAM_KASRA) r -= B;
                        } else {
                            if (cf == FAM_FATHA) r += B;
                            else if (cf == FAM_KASRA) r -= B;
                        }
                    }
                }
                break;

            case PART_LA:
                if (c_noun && !c_al) {
                    if (cd == DIAC_FATHA) r += B;
                    if (ctan) r -= B / 2;
                } else if (c_imperf) {
                    if (vmf == FAM_SUKUN) r += B / 2;
                }
                break;

            case PART_SAWFA:
            case PART_QAD:
                if (c_imperf || c_verb) {
                    if (vmf == FAM_DAMMA && !vmtan) r += B / 2;
                    if (vmf == FAM_KASRA || vmtan) r -= B;
                }
                break;

            case PART_YA:
                if (c_noun && !c_al) {
                    if (cd == DIAC_DAMMA) r += B;
                    if (ctan) r -= B;
                }
                break;

            case PART_PRON:
                if (c_noun) {
                    if (cf == FAM_DAMMA) r += B / 2;
                    else if (cf == FAM_KASRA) r -= B / 2;
                }
                break;

            case PART_DEM:
                if (c_al && c_noun && p2) {
                    if (p2->si.particle == PART_GEN || p2->si.particle == PART_IDAFA_NOUN ||
                        p2->si.particle == PART_NUM_IDAFA || p2->si.has_prep) {
                        if (cf == FAM_KASRA) r += B + B / 2;
                        else if (cf != FAM_NONE) r -= B;
                    } else if (p2->si.particle == PART_INNA || p2->si.particle == PART_AN ||
                               p2->si.particle == PART_LI_AN || p2->si.particle == PART_LAKIN) {
                        if (cf == FAM_FATHA || (c_fem_at && cf == FAM_KASRA)) r += B + B / 2;
                    } else if (p2->top_verb == 1) {
                        if (cf == FAM_FATHA || cf == FAM_DAMMA) r += B / 2;
                        else if (cf == FAM_KASRA && !c_fem_at) r -= B;
                    }
                } else if (!c_al && c_noun && p2 && p2->si.particle == PART_KANA && ctan) {
                    if (cf == FAM_FATHA) r += B + B / 2;
                }
                break;

            case PART_CONJ:
                if (p2 && c_noun && p2->si.particle == PART_NONE && p2->si.verb_like < 2 && !p2->top_verb) {
                    if (p3 && (p3->si.particle == PART_GEN || p3->si.particle == PART_IDAFA_NOUN ||
                               p3->si.particle == PART_NUM_IDAFA || p3->si.has_prep)) {
                        if (cf == FAM_KASRA) r += B;
                    } else if (p2->lex_top_end != 255) {
                        uint8_t p2f = diac_family(p2->lex_top_end);
                        if (p2f != FAM_NONE && p2f != FAM_SUKUN && cf == p2f) r += B;
                    }
                }
                break;

            default:
                break;
        }
    }

    if (p2 && (p2->si.particle == PART_INNA || p2->si.particle == PART_AN ||
               p2->si.particle == PART_LI_AN || p2->si.particle == PART_LAKIN) &&
        (pt.surf_flags & SF_SHORT_PREP_PRON) && c_noun && !c_al) {
        if (cf == FAM_FATHA) r += B + B / 2;
    }

    if (p2 && p2->si.particle == PART_KANA && (pt.surf_flags & SF_SHORT_PREP_PRON) && c_noun) {
        if (cf == FAM_DAMMA) r += B + B / 2;
    }

    if ((pp == PART_NONE || pp == PART_IDAFA_NOUN || pp == PART_NUM_IDAFA) &&
        !c_part && !c_own_gen && !(cc.flags & CF_LAM_NASB)) {
        const bool p_noun = (pc.verb_pattern == 0) && pt.si.verb_like < 2 && !(pc.flags & CF_FUT) &&
                            pp != PART_IDAFA_NOUN && pp != PART_NUM_IDAFA;
        const bool p_verb = (pc.verb_pattern == 1) || (pt.si.verb_like == 2 && !p_al);

        if (p_verb && c_noun) {
            if (pc.pos_tag == POS_VERB_PAST_SUBJ || pc.pos_tag == POS_VERB_IMPERF_12) {
                if (c_fem_at) {
                    if (cf == FAM_KASRA) r += B + B / 2;
                    else if (cf == FAM_FATHA) r -= 3 * B;
                    else if (cf == FAM_DAMMA) r -= B;
                } else {
                    if (cf == FAM_FATHA) r += B + B / 2;
                    else if (cf == FAM_DAMMA) r -= B;
                    else if (cf == FAM_KASRA) r -= 3 * B;
                }
            } else if (pc.pos_tag == POS_VERB_PASS) {
                if (cf == FAM_DAMMA) r += B + B / 2;
                else if (cf == FAM_FATHA) r -= 2 * B;
                else if (cf == FAM_KASRA) r -= 3 * B;
            } else {
                if (c_fem_at) {
                    if (cf == FAM_FATHA) r -= 3 * B;
                } else {
                    if (cf == FAM_KASRA) r -= 3 * B;
                }
                if ((pc.ext_flags & EF_PAST_TEH_FEM) && c_al &&
                    (cs[ct.n - 1] == S_TEH_MARBUTA || pd == DIAC_KASRA)) {
                    if (cf == FAM_DAMMA) r += B;
                    else if (cf == FAM_KASRA) r -= B;
                }
            }
        }

        if (ct.surf_flags & SF_IBN) {
            if (ctan) r -= 3 * B;
            if (ptan) r -= 3 * B;
            if (p2 && (p2->surf_flags & SF_NAME_HEAD)) {
                uint8_t head_fam = diac_family(p2->lex_top_end);
                const uint8_t* p2s = &ctx.syms[p2->off];
                size_t p2st = (p2->n >= 4 && (p2s[0] == S_WAW || p2s[0] == S_FEH || p2s[0] == S_BEH || p2s[0] == S_LAM)) ? 1 : 0;
                if (p2->n - p2st == 3 && p2s[p2st] == S_ALEF_HAMZA_A && p2s[p2st + 1] == S_BEH) {
                    if (p2s[p2st + 2] == S_WAW) head_fam = FAM_DAMMA;
                    else if (p2s[p2st + 2] == S_YEH) head_fam = FAM_KASRA;
                    else if (p2s[p2st + 2] == S_ALEF) head_fam = FAM_FATHA;
                } else if (p3 && (p3->si.particle == PART_GEN || p3->si.particle == PART_IDAFA_NOUN ||
                                  p3->si.has_prep) && !(p3->surf_flags & SF_MAN)) {
                    head_fam = FAM_KASRA;
                } else if (p3 && (p3->si.particle == PART_INNA || p3->si.particle == PART_AN ||
                                  p3->si.particle == PART_LI_AN)) {
                    head_fam = FAM_FATHA;
                }
                if (head_fam != FAM_NONE && head_fam != FAM_SUKUN) {
                    if (cf == head_fam) r += 2 * B;
                    else r -= B + B / 2;
                }
            } else if (p2 && (p2->si.particle == PART_GEN || p2->si.particle == PART_IDAFA_NOUN || p2->si.has_prep) &&
                       !(p2->surf_flags & SF_MAN)) {
                if (cf == FAM_KASRA) r += B + B / 2;
                else r -= B + B / 2;
            } else if (p2 && (p2->si.particle == PART_INNA || p2->si.particle == PART_AN ||
                              p2->si.particle == PART_LI_AN)) {
                if (cf == FAM_FATHA) r += B + B / 2;
                else r -= B;
            } else if (p_noun && pf != FAM_NONE && pf != FAM_SUKUN) {
                if (cf == pf) r += B + B / 2;
                else r -= B;
            }
        }

        if ((pt.surf_flags & SF_FIVE_NOUN) && c_noun) {
            if (ptan) r -= 3 * B;
            if (c_diptote && !c_al) {
                if (cf == FAM_FATHA && !ctan) r += B + B / 2;
            } else if (cf == FAM_KASRA) {
                r += B + B / 2;
            } else if (cf != FAM_NONE) {
                r -= B;
            }
        }

        if (p_noun && !p_al && !(pc.flags & CF_SUFFIX) && c_al && c_noun) {
            if (ct.si.particle != PART_REL) {
                if (cf == FAM_KASRA) {
                    r += B + B / 2;
                    if (ptan && !(pc.ext_flags & EF_DIPTOTE)) r -= 2 * B;
                } else {
                    if (cf != FAM_NONE) r -= B / 2;
                    if (ptan) r -= B + B / 2;
                }
            }
        }

        const bool likely_khabar_plus_adj = p2 && (p2->si.particle == PART_PRON || p2->si.particle == PART_KANA || p2->si.particle == PART_INNA) && ct.is_adj_like;
        if (p_noun && !p_al && !ptan && !(pc.flags & (CF_SUFFIX | CF_EXPANDED)) && !pt.top_verb && !likely_khabar_plus_adj &&
            !(pc.ext_flags & (EF_DIPTOTE | EF_DUAL_NOON | EF_MASC_PL_NOON)) &&
            pf != FAM_NONE && pf != FAM_SUKUN && ps[pt.n - 1] != S_ALEF_MAKSURA && ps[pt.n - 1] != S_ALEF &&
            !(p2 && (p2->si.particle == PART_LA || p2->si.particle == PART_YA)) &&
            c_noun && !c_al && !(ct.surf_flags & SF_IBN) && !ct.is_adj_like) {
            if (c_diptote && (ct.is_diptote_lex == 2 || !nx || nx->si.particle != PART_NONE || nx->is_adj_like)) {
                if (cf == FAM_FATHA && !ctan) r += B;
            } else {
                if (cf == FAM_KASRA) r += B;
                else if (cf != FAM_NONE) r -= B / 2;
            }
        }
        // Indefinite Noun + Indefinite Adjective (نكرة + صفة نكرة): agree in case and tanween
        const bool c_meem_participle = (ct.n > ct.si.al_len + 3 && cs[ct.si.al_len] == S_MEEM && cc.morph_class == MC_NOUN_SHADDA_CORE);
        if (p_noun && !p_al && !(pc.flags & CF_SUFFIX) && c_noun && !c_al && !(cc.flags & CF_SUFFIX) &&
            (ct.is_adj_like || c_meem_participle) && !(cc.ext_flags & EF_FEM_PL_AT)) {
            if (ptan && pf == cf && ctan) r += 2 * B;
            else if (ptan && pf != cf && cf == FAM_KASRA && pf != FAM_KASRA) r -= 2 * B;
            else if (!ptan && cf == FAM_KASRA && pf != FAM_KASRA) r -= B;
        }
        // Indefinite Noun before non-tanween genitive Mudaf Ilayh (دوري كرة القدم / إطفاء نار): drop tanween
        if (p_noun && !p_al && !(pc.flags & CF_SUFFIX) && !(pc.ext_flags & EF_DIPTOTE) && ptan &&
            c_noun && !c_al && !ct.is_adj_like && !(cc.flags & CF_OOV) && cf == FAM_KASRA && !ctan &&
            nx && nx->si.al_len > 0) {
            r -= B + B / 2;
        }

        if (p_noun && p_al && c_al && c_noun && pf == FAM_NONE) {
            if ((pc.flags & CF_GEN) || pt.si.has_prep ||
                (p2 && (p2->si.particle == PART_GEN || p2->si.particle == PART_IDAFA_NOUN || p2->si.particle == PART_HATTA))) {
                if (cf == FAM_KASRA) r += B + B / 2;
                else if (cf != FAM_NONE) r -= B;
            } else if (p2 && (p2->si.particle == PART_INNA || p2->si.particle == PART_AN || p2->si.particle == PART_LI_AN)) {
                if (cf == FAM_FATHA || (c_fem_at && cf == FAM_KASRA)) r += B;
            }
        }

        if (p_noun && p_al && c_al && c_noun && pf != FAM_NONE && pf != FAM_SUKUN) {
            if (cf == pf) r += B + B / 3;
            else if (!((pc.ext_flags & EF_FEM_PL_AT) && pf == FAM_KASRA && cf == FAM_FATHA)) r -= B / 2;
        }

        if (p_noun && !p_al && ptan && !c_al && c_noun && ctan) {
            if (cf == pf) r += B;
            else r -= B / 2;
        }

        if (p_noun && (pc.ext_flags & EF_DIPTOTE) && !p_al && pf == FAM_FATHA && !c_al && c_noun && ctan) {
            if (p2 && (p2->si.particle == PART_GEN || p2->si.has_prep)) {
                if (cf == FAM_KASRA) r += B;
            } else {
                if (cf == FAM_FATHA) r += B;
            }
        }

        if (p_noun && (pc.flags & CF_SUFFIX) && c_al && c_noun && pf != FAM_NONE && pf != FAM_SUKUN) {
            if (cf == pf) r += B;
        }

        if (p_noun && c_noun && ct.n >= 4 && (cs[0] == S_WAW || cs[0] == S_FEH) &&
            !ct.si.has_prep && pf != FAM_NONE && pf != FAM_SUKUN && (p_al == c_al || (ptan && ctan))) {
            if (cf == pf) r += B;
        }
    }
    return static_cast<int32_t>((static_cast<int64_t>(r) * weights_[W_RULE]) / 1000);
}

std::string Diacritizer::diacritize(const char* text, size_t len) const {
    if (!mapped_ptr_ || !text || len == 0) return text ? std::string(text, len) : std::string();

    struct Span {
        bool is_word;
        uint32_t a;
        uint32_t b;
    };
    struct Scratch {
        std::vector<Span> spans;
        std::string raw_pool;
        Ctx ctx;
        std::vector<uint32_t> dp_off;
        std::vector<int32_t> dp;
        std::vector<int32_t> back;
        std::vector<uint32_t> chosen;
        std::vector<int32_t> override_idx;
        std::vector<Cand> override_cands;
    };
    thread_local Scratch sc_buf;
    auto& spans = sc_buf.spans;
    auto& raw_pool = sc_buf.raw_pool;
    auto& ctx = sc_buf.ctx;
    spans.clear();
    raw_pool.clear();
    ctx.syms.clear();
    ctx.cps.clear();
    ctx.toks.clear();
    ctx.cands.clear();

    if (ctx.syms.capacity() < len / 2 + 8) {
        ctx.syms.reserve(len / 2 + 8);
        ctx.cps.reserve(len / 2 + 8);
    }

    size_t pos = 0;
    uint8_t pending_boundary = 1;
    size_t raw_start = raw_pool.size();
    while (pos < len) {
        size_t save = pos;
        uint32_t cp = utf8_next(text, len, pos);
        if (is_arabic_letter(cp)) {
            if (raw_pool.size() > raw_start) {
                spans.push_back({false, static_cast<uint32_t>(raw_start), static_cast<uint32_t>(raw_pool.size())});
            }
            Token tok;
            tok.off = static_cast<uint32_t>(ctx.syms.size());
            ctx.syms.push_back(char_to_sym(cp));
            ctx.cps.push_back(cp);
            size_t cnt = 1;
            while (pos < len) {
                size_t ns = pos;
                uint32_t ncp = utf8_next(text, len, pos);
                if (is_arabic_diacritic(ncp) || is_tatweel(ncp)) continue;
                if (is_arabic_letter(ncp)) {
                    ctx.syms.push_back(char_to_sym(ncp));
                    ctx.cps.push_back(ncp);
                    ++cnt;
                } else {
                    pos = ns;
                    break;
                }
            }
            tok.n = static_cast<uint8_t>(std::min<size_t>(cnt, 255));
            tok.skip = cnt > MAX_WORD_LEN;
            if (tok.skip) {
                ctx.syms.resize(tok.off);
                std::string w;
                for (size_t k = 0; k < cnt; ++k) utf8_append(w, ctx.cps[tok.off + k]);
                ctx.cps.resize(tok.off);
                raw_start = raw_pool.size();
                raw_pool += w;
                spans.push_back({false, static_cast<uint32_t>(raw_start), static_cast<uint32_t>(raw_pool.size())});
                raw_start = raw_pool.size();
                pending_boundary = 1;
                continue;
            }
            tok.boundary_before = pending_boundary;
            pending_boundary = 0;
            spans.push_back({true, static_cast<uint32_t>(ctx.toks.size()), 0});
            ctx.toks.push_back(tok);
            raw_start = raw_pool.size();
        } else {
            if (is_boundary_cp(cp)) pending_boundary = 1;
            if (!pending_boundary && !ctx.toks.empty() && ((cp >= '0' && cp <= '9') || (cp >= 0x0660 && cp <= 0x0669))) {
                ctx.toks.back().digit_after = 1;
            }
            if (!is_arabic_diacritic(cp)) raw_pool.append(text + save, pos - save);
        }
    }
    if (raw_pool.size() > raw_start) {
        spans.push_back({false, static_cast<uint32_t>(raw_start), static_cast<uint32_t>(raw_pool.size())});
    }
    if (ctx.toks.empty()) return std::string(text, len);

    for (auto& t : ctx.toks) {
        build_candidates(ctx, t);
        if (t.cand_count > 0) {
            t.top_verb = ctx.cands[t.cand_begin].verb_pattern;
            t.top_pos = ctx.cands[t.cand_begin].pos_tag;
        }
    }

    const size_t W = ctx.toks.size();
    for (size_t i = 0; i < W; ++i) {
        Token& t = ctx.toks[i];
        for (uint32_t k = 0; k < t.cand_count; ++k) {
            Cand& c = ctx.cands[t.cand_begin + k];
            c.emis += unary_rules(ctx, i, c);
        }
    }

    if (morph_trees_.valid() && weights_[W_MORPH_TREE] != 0) {
        int32_t mf[MORPH_NUM_FEATURES];
        for (size_t i = 0; i < W; ++i) {
            const Token& t = ctx.toks[i];
            if (t.cand_count < 2) continue;
            uint8_t first_mc = ctx.cands[t.cand_begin].morph_class;
            bool multi_mc = false;
            for (uint32_t k = 1; k < t.cand_count; ++k) {
                if (ctx.cands[t.cand_begin + k].morph_class != first_mc) { multi_mc = true; break; }
            }
            if (!multi_mc) continue;
            const uint8_t* s = &ctx.syms[t.off];
            const Token* p = (i >= 1 && !t.boundary_before) ? &ctx.toks[i - 1] : nullptr;
            const Token* p2 = (p && i >= 2 && !p->boundary_before) ? &ctx.toks[i - 2] : nullptr;
            const Token* nx = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? &ctx.toks[i + 1] : nullptr;
            const Token* nx2 = (nx && i + 2 < W && !ctx.toks[i + 2].boundary_before) ? &ctx.toks[i + 2] : nullptr;
            mf[0] = p2 ? p2->si.particle : -1;
            mf[1] = p ? p->si.particle : -1;
            mf[2] = t.si.particle;
            mf[3] = nx ? nx->si.particle : -1;
            mf[4] = nx2 ? nx2->si.particle : -1;
            mf[5] = p ? p->si.al_len : -1;
            mf[6] = t.si.al_len;
            mf[7] = nx ? nx->si.al_len : -1;
            mf[8] = nx2 ? nx2->si.al_len : -1;
            mf[9] = p ? p->si.has_prep : -1;
            mf[10] = t.si.has_prep;
            mf[11] = nx ? nx->si.has_prep : -1;
            mf[12] = p ? p->si.verb_like : -1;
            mf[13] = t.si.verb_like;
            mf[14] = nx ? nx->si.verb_like : -1;
            mf[15] = (p && p->lex_top_end != 255) ? p->lex_top_end : -1;
            mf[16] = (t.lex_top_end != 255) ? t.lex_top_end : -1;
            mf[17] = (nx && nx->lex_top_end != 255) ? nx->lex_top_end : -1;
            mf[18] = p ? p->top_verb : -1;
            mf[19] = t.top_verb;
            mf[20] = nx ? nx->top_verb : -1;
            mf[21] = p ? p->top_pos : -1;
            mf[22] = t.top_pos;
            mf[23] = nx ? nx->top_pos : -1;
            mf[24] = t.n;
            mf[25] = s[0];
            mf[26] = s[std::min<size_t>(t.si.al_len, t.n - 1)];
            mf[27] = t.n >= 2 ? s[t.n - 2] : -1;
            mf[28] = s[t.n - 1];
            mf[29] = t.boundary_before;
            mf[30] = nx ? ctx.syms[nx->off] : -1;
            mf[31] = nx ? ctx.syms[nx->off + nx->n - 1] : -1;
            int32_t sc[NUM_DIACRITICS];
            for (auto& v : sc) v = 0;
            morph_trees_.predict_scores(mf, MORPH_NUM_FEATURES, sc);
            for (uint32_t k = 0; k < t.cand_count; ++k) {
                Cand& c = ctx.cands[t.cand_begin + k];
                if (c.morph_class < morph_trees_.num_classes())
                    c.emis += static_cast<int32_t>((static_cast<int64_t>(sc[c.morph_class]) * weights_[W_MORPH_TREE]) / 1000);
            }
        }
    }

    if (irab_trees_.valid() && weights_[W_IRAB_TREE] != 0) {
        std::vector<uint8_t> cg_arr;
        std::vector<int8_t> dg_arr;
        std::vector<uint8_t> pic_arr;
        std::vector<uint8_t> ric_arr;
        const bool use_56 = (irab_feat_count_ >= IRAB_NUM_FEATURES);
        if (use_56) {
            cg_arr.resize(W, 0);
            dg_arr.resize(W, -1);
            pic_arr.resize(W, 0);
            ric_arr.resize(W, 0);
            uint8_t gs = 0;
            int lg = -1;
            int cs_idx = 0;
            for (size_t i = 0; i < W; ++i) {
                const Token& tk = ctx.toks[i];
                if (tk.boundary_before || tk.si.particle == PART_REL || tk.si.particle == PART_COND) {
                    gs = 0;
                    lg = -1;
                    if (tk.boundary_before) cs_idx = static_cast<int>(i);
                }
                cg_arr[i] = gs;
                dg_arr[i] = (lg >= 0) ? static_cast<int8_t>(std::min<int>(7, static_cast<int>(i) - lg)) : -1;
                pic_arr[i] = static_cast<uint8_t>(std::min<int>(15, static_cast<int>(i) - cs_idx));

                uint8_t top_mc = (tk.cand_count > 0) ? ctx.cands[tk.cand_begin].morph_class : 11;
                if (tk.si.particle == PART_INNA ||
                    ((tk.si.particle == PART_AN || tk.si.particle == PART_LI_AN || tk.si.particle == PART_LAKIN) &&
                     top_mc == MC_PART_SHADDA)) {
                    gs = 4; lg = static_cast<int>(i);
                } else if (tk.si.particle == PART_KANA) {
                    gs = 5; lg = static_cast<int>(i);
                } else if (tk.si.particle == PART_GEN) {
                    gs = 6; lg = static_cast<int>(i);
                } else if (tk.top_verb == 1 || (tk.si.verb_like == 2 && tk.si.particle == PART_NONE)) {
                    if (tk.top_pos == POS_VERB_PASS) gs = 3;
                    else if (tk.top_pos == POS_VERB_PAST_SUBJ || tk.top_pos == POS_VERB_IMPERF_12) gs = 2;
                    else gs = 1;
                    lg = static_cast<int>(i);
                } else if (gs == 6 && tk.si.particle == PART_NONE && tk.si.al_len == 0 &&
                           (tk.lex_top_end == DIAC_DAMMATAN || tk.lex_top_end == DIAC_FATHATAN || tk.lex_top_end == DIAC_KASRATAN)) {
                    gs = 0;
                }
            }
            int ce_idx = static_cast<int>(W);
            for (int i = static_cast<int>(W) - 1; i >= 0; --i) {
                ric_arr[i] = static_cast<uint8_t>(std::min<int>(15, ce_idx - 1 - i));
                if (ctx.toks[i].boundary_before) ce_idx = i;
            }
        }

        int32_t f[IRAB_NUM_FEATURES];
        for (size_t i = 0; i < W; ++i) {
            const Token& t = ctx.toks[i];
            if (t.cand_count < 2) continue;
            const uint8_t first_end = ctx.cands[t.cand_begin].diac[ctx.cands[t.cand_begin].case_pos];
            bool multi_end = false;
            for (uint32_t k = 1; k < t.cand_count; ++k) {
                const Cand& ck = ctx.cands[t.cand_begin + k];
                if (ck.diac[ck.case_pos] != first_end) { multi_end = true; break; }
            }
            if (!multi_end) continue;
            const uint8_t* s = &ctx.syms[t.off];
            const Token* p = (i >= 1 && !t.boundary_before) ? &ctx.toks[i - 1] : nullptr;
            const Token* p2 = (p && i >= 2 && !p->boundary_before) ? &ctx.toks[i - 2] : nullptr;
            const Token* nx = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? &ctx.toks[i + 1] : nullptr;
            const Token* nx2 = (nx && i + 2 < W && !ctx.toks[i + 2].boundary_before) ? &ctx.toks[i + 2] : nullptr;
            if (irab_feat_count_ >= IRAB_LEGACY_FEATURES) {
                f[0] = p2 ? p2->si.particle : -1;
                f[1] = p ? p->si.particle : -1;
                f[2] = t.si.particle;
                f[3] = nx ? nx->si.particle : -1;
                f[4] = nx2 ? nx2->si.particle : -1;
                f[5] = p2 ? p2->si.al_len : -1;
                f[6] = p ? p->si.al_len : -1;
                f[7] = t.si.al_len;
                f[8] = nx ? nx->si.al_len : -1;
                f[9] = nx2 ? nx2->si.al_len : -1;
                f[10] = p2 ? p2->si.has_prep : -1;
                f[11] = p ? p->si.has_prep : -1;
                f[12] = t.si.has_prep;
                f[13] = nx ? nx->si.has_prep : -1;
                f[14] = p ? p->si.verb_like : -1;
                f[15] = t.si.verb_like;
                f[16] = nx ? nx->si.verb_like : -1;
                f[17] = nx2 ? nx2->si.verb_like : -1;
                f[18] = (p && p->lex_top_end != 255) ? p->lex_top_end : -1;
                f[19] = (t.lex_top_end != 255) ? t.lex_top_end : -1;
                f[20] = (nx && nx->lex_top_end != 255) ? nx->lex_top_end : -1;
                f[21] = p ? p->top_verb : -1;
                f[22] = t.top_verb;
                f[23] = nx ? nx->top_verb : -1;
                f[24] = p ? p->top_pos : -1;
                f[25] = t.top_pos;
                f[26] = nx ? nx->top_pos : -1;
                f[27] = t.n;
                f[28] = s[t.n - 1];
                f[29] = t.n >= 2 ? s[t.n - 2] : -1;
                f[30] = s[std::min<size_t>(t.si.al_len, t.n - 1)];
                f[31] = p ? ctx.syms[p->off + p->n - 1] : -1;
                f[32] = nx ? ctx.syms[nx->off + nx->n - 1] : -1;
                f[33] = nx ? ctx.syms[nx->off + std::min<size_t>(nx->si.al_len, nx->n - 1)] : -1;
                f[34] = t.boundary_before;
                f[35] = t.is_fem_at;
                f[36] = t.is_diptote_lex ? 1 : 0;
                f[37] = t.is_ibn;
                f[38] = nx ? nx->is_ibn : -1;
                f[39] = nx ? nx->is_fem_at : -1;
                if (irab_feat_count_ >= IRAB_V5_BASE_FEATURES) {
                    f[40] = p ? p->is_fem_at : -1;
                    f[41] = (p && p->top_pos == POS_VERB_PAST_FEM) ? 1 : 0;
                    f[42] = (p && p->cand_count > 0 && (ctx.cands[p->cand_begin].flags & CF_SUFFIX)) ? 1 : 0;
                    f[43] = (t.cand_count > 0 && (ctx.cands[t.cand_begin].flags & CF_SUFFIX)) ? 1 : 0;
                }
                if (use_56) {
                    f[44] = cg_arr[i];
                    f[45] = dg_arr[i];
                    f[46] = pic_arr[i];
                    f[47] = ric_arr[i];
                    f[48] = t.is_adj_like;
                    f[49] = nx ? nx->is_adj_like : -1;
                    f[50] = p ? p->is_adj_like : -1;
                    f[51] = (nx && nx->si.al_len > 0 && !nx->si.has_prep) ? 1 : 0;
                    f[52] = (nx && (nx->si.verb_like == 2 || nx->top_verb == 1)) ? 1 : 0;
                    f[53] = (p && p->si.al_len > 0 && !p->si.has_prep) ? 1 : 0;
                    f[54] = (!nx || nx->si.particle != PART_NONE || nx->si.has_prep) ? 1 : 0;
                    f[55] = p2 ? p2->top_pos : -1;
                }
            } else {
                f[0] = p2 ? p2->si.particle : -1;
                f[1] = p ? p->si.particle : -1;
                f[2] = t.si.particle;
                f[3] = nx ? nx->si.particle : -1;
                f[4] = t.si.al_len;
                f[5] = p ? p->si.al_len : -1;
                f[6] = nx ? nx->si.al_len : -1;
                f[7] = t.si.verb_like;
                f[8] = t.n;
                f[9] = s[t.n - 1];
                f[10] = t.n >= 2 ? s[t.n - 2] : -1;
                f[11] = s[std::min<size_t>(t.si.al_len, t.n - 1)];
                f[12] = p ? ctx.syms[p->off + p->n - 1] : -1;
                f[13] = t.boundary_before;
                f[14] = t.si.has_prep;
                f[15] = (p && p->lex_top_end != 255) ? p->lex_top_end : -1;
                f[16] = p ? p->si.has_prep : -1;
                f[17] = p ? p->si.verb_like : -1;
                f[18] = nx ? nx->si.verb_like : -1;
                f[19] = nx ? nx->si.has_prep : -1;
                f[20] = p2 ? p2->si.al_len : -1;
                f[21] = p2 ? p2->si.has_prep : -1;
                f[22] = (t.lex_top_end != 255) ? t.lex_top_end : -1;
                f[23] = (t.top_verb == 1) ? 1 : 0;
            }
            int32_t sc[NUM_DIACRITICS];
            for (auto& v : sc) v = 0;
            irab_trees_.predict_scores(f, irab_feat_count_, sc);
            for (uint32_t k = 0; k < t.cand_count; ++k) {
                Cand& c = ctx.cands[t.cand_begin + k];
                uint8_t e = c.diac[c.case_pos];
                if (e < irab_trees_.num_classes())
                    c.emis += static_cast<int32_t>((static_cast<int64_t>(sc[e]) * weights_[W_IRAB_TREE]) / 1000);
            }
        }
    }

    constexpr uint32_t BEAM_K = 10;
    for (size_t i = 0; i < W; ++i) {
        Token& t = ctx.toks[i];
        if (t.cand_count > BEAM_K) {
            auto b = ctx.cands.begin() + t.cand_begin;
            std::partial_sort(b, b + BEAM_K, b + t.cand_count, [](const Cand& a, const Cand& c) { return a.emis > c.emis; });
            t.cand_count = BEAM_K;
        }
    }

    auto& dp_off = sc_buf.dp_off;
    auto& dp = sc_buf.dp;
    auto& back = sc_buf.back;
    dp_off.resize(W + 1);
    dp_off[0] = 0;
    for (size_t i = 0; i < W; ++i) dp_off[i + 1] = dp_off[i] + ctx.toks[i].cand_count;
    dp.assign(dp_off[W], NEG_INF);
    back.assign(dp_off[W], 0);

    const int32_t w_tr = weights_[W_TRANS];
    const int32_t w_bg = weights_[W_BIGRAM];
    const bool use_v5_bg = bigrams_.is_v5();
    const bool use_v3_bg = bigrams_.is_v3();

    for (size_t i = 0; i < W; ++i) {
        const Token& ct = ctx.toks[i];
        const bool fresh = (i == 0) || ct.boundary_before;
        const Token* pt = fresh ? nullptr : &ctx.toks[i - 1];
        const Token* p2 = (pt && i >= 2 && !pt->boundary_before) ? &ctx.toks[i - 2] : nullptr;
        const Token* p3 = (p2 && i >= 3 && !p2->boundary_before) ? &ctx.toks[i - 3] : nullptr;
        const Token* nx = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? &ctx.toks[i + 1] : nullptr;
        int32_t prev_best = 0;
        int prev_best_idx = 0;
        if (fresh && i > 0) {
            const Token& pp = ctx.toks[i - 1];
            prev_best = NEG_INF;
            for (uint32_t p = 0; p < pp.cand_count; ++p) {
                if (dp[dp_off[i - 1] + p] > prev_best) {
                    prev_best = dp[dp_off[i - 1] + p];
                    prev_best_idx = static_cast<int>(p);
                }
            }
        }
        const BigramEntry* bg_v2 = nullptr;
        const BigramEntryV3* bg_v3 = nullptr;
        int32_t bg_v5 = -1;
        uint16_t bg_mask = 0;
        if (pt && pt->word_id != UINT32_MAX && ct.word_id != UINT32_MAX) {
            if (use_v5_bg) {
                bg_v5 = bigrams_.find_v5(pt->word_id, ct.word_id);
                if (bg_v5 >= 0) bg_mask = meta_bg_mask(ct.word_id);
            } else if (use_v3_bg) {
                bg_v3 = bigrams_.find_v3(pt->word_id, ct.word_id);
                if (bg_v3) bg_mask = meta_bg_mask(ct.word_id);
            } else {
                bg_v2 = bigrams_.find(pt->word_id, ct.word_id);
            }
        }

        for (uint32_t c = 0; c < ct.cand_count; ++c) {
            const Cand& cc = ctx.cands[ct.cand_begin + c];
            const uint8_t ce = cc.diac[cc.case_pos];
            if (fresh) {
                uint8_t cr = has_v5_24roles_ ? cand_role_v5(ct, cc) : ct.role;
                int32_t t = static_cast<int32_t>((static_cast<int64_t>(trans_score(0, DIAC_NONE, cr, ce)) * w_tr) / 1000);
                dp[dp_off[i] + c] = prev_best + cc.emis + t;
                back[dp_off[i] + c] = prev_best_idx;
                continue;
            }
            int16_t raw_bg = (bg_v5 >= 0) ? bigrams_.bonus_v5(bg_v5, bg_mask, ce)
                           : (bg_v3 ? bigrams_.bonus_v3(bg_v3, bg_mask, ce)
                           : (bg_v2 ? bg_v2->bonus[ce] : 0));
            int32_t bgb = raw_bg ? static_cast<int32_t>((static_cast<int64_t>(raw_bg) * w_bg) / 1000) : 0;
            const uint8_t cr = has_v5_24roles_ ? cand_role_v5(ct, cc) : ct.role;
            int32_t best = NEG_INF;
            int best_p = 0;
            for (uint32_t p = 0; p < pt->cand_count; ++p) {
                const Cand& pc = ctx.cands[pt->cand_begin + p];
                const uint8_t pe = pc.diac[pc.case_pos];
                const uint8_t pr = has_v5_24roles_ ? cand_role_v5(*pt, pc) : pt->role;
                int32_t t = static_cast<int32_t>((static_cast<int64_t>(trans_score(pr, pe, cr, ce)) * w_tr) / 1000);
                int32_t v = dp[dp_off[i - 1] + p] + t + binary_rules(ctx, p3, p2, *pt, pc, ct, cc, nx);
                if (v > best) {
                    best = v;
                    best_p = static_cast<int>(p);
                }
            }
            dp[dp_off[i] + c] = best + cc.emis + bgb;
            back[dp_off[i] + c] = best_p;
        }
    }

    auto& chosen = sc_buf.chosen;
    chosen.assign(W, 0);
    {
        int32_t best = NEG_INF;
        for (uint32_t c = 0; c < ctx.toks[W - 1].cand_count; ++c) {
            if (dp[dp_off[W - 1] + c] > best) {
                best = dp[dp_off[W - 1] + c];
                chosen[W - 1] = c;
            }
        }
        for (size_t i = W - 1; i > 0; --i) chosen[i - 1] = static_cast<uint32_t>(back[dp_off[i] + chosen[i]]);
    }

    // Pass-2: Sentence-level Orthographic Register + ArAnaSyn Clause Government + Na't/Atf + Syntactic Laws
    auto& override_idx = sc_buf.override_idx;
    auto& override_cands = sc_buf.override_cands;
    override_idx.assign(W, -1);
    override_cands.clear();
    {
        auto get_cand = [&](size_t idx) -> const Cand& {
            return (override_idx[idx] >= 0) ? override_cands[override_idx[idx]] : ctx.cands[ctx.toks[idx].cand_begin + chosen[idx]];
        };
        int al_sukun_vote = 0;
        for (size_t i = 0; i < W; ++i) {
            const Token& t = ctx.toks[i];
            if (t.cand_count == 0) continue;
            const Cand& c = ctx.cands[t.cand_begin + chosen[i]];
            if (t.si.al_len >= 2 && !is_sun_letter(ctx.syms[t.off + t.si.al_len])) {
                al_sukun_vote += (c.ext_flags & EF_LAM_SUKUN) ? 1 : -1;
            }
        }
        if (al_sukun_vote != 0) {
            bool want_suk = al_sukun_vote > 0;
            for (size_t i = 0; i < W; ++i) {
                const Token& t = ctx.toks[i];
                if (t.cand_count < 2 || t.si.al_len < 2 || is_sun_letter(ctx.syms[t.off + t.si.al_len])) continue;
                const Cand& cur = ctx.cands[t.cand_begin + chosen[i]];
                if (((cur.ext_flags & EF_LAM_SUKUN) != 0) == want_suk) continue;
                const size_t lp = t.si.al_lam_pos;
                for (uint32_t k = 0; k < t.cand_count; ++k) {
                    if (k == chosen[i]) continue;
                    const Cand& alt = ctx.cands[t.cand_begin + k];
                    if (((alt.ext_flags & EF_LAM_SUKUN) != 0) != want_suk) continue;
                    if (alt.emis + 1200 < cur.emis) continue;
                    bool same_rest = true;
                    for (size_t p = 0; p < t.n; ++p) {
                        if (p != lp && alt.diac[p] != cur.diac[p]) { same_rest = false; break; }
                    }
                    if (same_rest) { chosen[i] = k; break; }
                }
            }
        }

        // tan_mode: -1=preserve cur tanween, 0=force non-tanween, 1=force tanween
        auto enforce_case_fam = [&](size_t i, uint8_t target_fam, int8_t tan_mode, int32_t max_emis_gap, bool force_synth) -> bool {
            const Token& t = ctx.toks[i];
            const Cand cur = get_cand(i);
            if (cur.flags & CF_OOV) return false;
            const uint8_t* s = &ctx.syms[t.off];
            const uint8_t cur_d = cur.diac[cur.case_pos];
            const uint8_t cur_fam = diac_family(cur_d);
            const bool cur_tan = diac_is_tanween(cur_d);
            const bool want_tan = (tan_mode < 0) ? cur_tan : (tan_mode != 0);
            if (cur_fam == target_fam && cur_tan == want_tan) return false;
            if (cur_tan && cur_fam == FAM_FATHA && max_emis_gap < 1100 && tan_mode < 0) return false;

            for (uint32_t k = 0; k < t.cand_count; ++k) {
                const Cand& alt = ctx.cands[t.cand_begin + k];
                const uint8_t ad = alt.diac[alt.case_pos];
                if (diac_family(ad) != target_fam) continue;
                if (tan_mode >= 0 && diac_is_tanween(ad) != want_tan) continue;
                if (tan_mode < 0 && diac_is_tanween(ad) != cur_tan) continue;
                int32_t eff_alt = alt.emis + (((alt.flags & CF_EXPANDED) && !(cur.flags & CF_EXPANDED)) ? weights_[W_EXPAND_PEN] : 0);
                if (!force_synth && eff_alt + max_emis_gap < cur.emis) continue;
                bool same_core = (alt.case_pos == cur.case_pos);
                const size_t lam_skip = (t.si.al_len >= 2) ? static_cast<size_t>(t.si.al_lam_pos) : static_cast<size_t>(-1);
                for (size_t p = 0; same_core && p < cur.case_pos; ++p) {
                    if (p != lam_skip && alt.diac[p] != cur.diac[p]) same_core = false;
                }
                if (same_core) {
                    override_idx[i] = -1;
                    chosen[i] = k;
                    return true;
                }
            }

            if (!force_synth) return false;
            if (cur.verb_pattern != 0 || (cur.ext_flags & (EF_DUAL_NOON | EF_MASC_PL_NOON))) return false;
            if ((cur.ext_flags & EF_FEM_PL_AT) && target_fam == FAM_FATHA) return false;
            if (t.surf_flags & (SF_BAAD_QABL | SF_AMS)) return false;
            if (cur_d == DIAC_NONE || cur_d == DIAC_SUKUN) return false;
            if (s[cur.case_pos] == S_ALEF || s[cur.case_pos] == S_ALEF_MAKSURA ||
                (cur.case_pos + 1 < t.n && !(cur.flags & CF_SUFFIX) && s[cur.case_pos + 1] == S_ALEF)) return false;
            if (is_defective_yeh_noun(s, t.n, cur.case_pos, cur.diac, cur.verb_pattern)) return false;

            Cand synth = cur;
            uint8_t nd = make_diac(target_fam, want_tan, diac_has_shadda(cur_d));
            synth.diac[cur.case_pos] = nd;
            synth.verb_mood_diac = nd;
            if ((synth.flags & CF_SUFFIX) && cur.case_pos + 1 < t.n && s[cur.case_pos + 1] == S_HEH &&
                (synth.diac[cur.case_pos + 1] == DIAC_DAMMA || synth.diac[cur.case_pos + 1] == DIAC_KASRA)) {
                synth.diac[cur.case_pos + 1] = (target_fam == FAM_KASRA) ? DIAC_KASRA : DIAC_DAMMA;
            }
            override_idx[i] = static_cast<int32_t>(override_cands.size());
            override_cands.push_back(synth);
            return true;
        };

        enum GovMode : uint8_t {
            GOV_NONE = 0,
            GOV_VERB_NEED_SUBJ = 1,
            GOV_VERB_HAVE_SUBJ = 2,
            GOV_VERB_PASS = 3,
            GOV_INNA_ISM = 4,
            GOV_KANA_ISM = 5
        };
        uint8_t gov = GOV_NONE;
        uint8_t head_np_fam = FAM_NONE;
        bool head_np_al = false;
        uint8_t prev_np_fam = FAM_NONE;
        bool prev_np_al = false;
        bool prev_np_suffix = false;
        bool prev_np_tan = false;

        for (size_t i = 0; i < W; ++i) {
            const Token& t = ctx.toks[i];
            if (t.cand_count == 0) continue;
            if (t.boundary_before) {
                gov = GOV_NONE;
                head_np_fam = FAM_NONE;
                head_np_al = false;
                prev_np_fam = FAM_NONE;
                prev_np_al = false;
                prev_np_suffix = false;
                prev_np_tan = false;
            }

            {
                const Cand& cur = get_cand(i);
                const uint8_t cf = diac_family(cur.diac[cur.case_pos]);
                const bool c_al = (cur.flags & CF_HAS_AL) || t.si.al_len > 0;
                const bool c_gen = (cur.flags & CF_GEN) || t.si.has_prep;
                const bool c_noun = (cur.verb_pattern == 0) && t.si.particle == PART_NONE && !c_gen;
                const bool c_noun_any = (cur.verb_pattern == 0) && t.si.particle == PART_NONE;
                const Token* pt = (!t.boundary_before && i >= 1) ? &ctx.toks[i - 1] : nullptr;
                const Cand* pc = pt ? &get_cand(i - 1) : nullptr;
                const Token* nx = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? &ctx.toks[i + 1] : nullptr;

                // Law A: Noun immediately before digits is Mudaf (no tanween: فِي سَنَةِ 2013)
                if (t.digit_after && c_noun_any && !c_al && !(cur.flags & CF_SUFFIX) &&
                    diac_is_tanween(cur.diac[cur.case_pos]) && !(cur.flags & CF_OOV)) {
                    enforce_case_fam(i, cf, 0, 2600, true);
                }

                const bool comp_num_tok = (nx && (t.surf_flags & SF_COMP_NUM) && (nx->surf_flags & SF_ASHAR)) ||
                                          (pt && (t.surf_flags & SF_ASHAR) && (pt->surf_flags & SF_COMP_NUM));
                if ((t.surf_flags & SF_COMP_NUM) && nx && (nx->surf_flags & SF_ASHAR)) {
                    enforce_case_fam(i, FAM_FATHA, 0, 4000, true);
                    enforce_case_fam(i + 1, FAM_FATHA, 0, 4000, true);
                }

                if (pt && !t.boundary_before && (pt->surf_flags & SF_MAN) && (t.surf_flags & SF_THEMMA)) {
                    int32_t best_sc = NEG_INF;
                    uint32_t best_k = UINT32_MAX;
                    for (uint32_t k = 0; k < pt->cand_count; ++k) {
                        const Cand& ac = ctx.cands[pt->cand_begin + k];
                        if (ac.diac[pt->n - 2] == DIAC_KASRA && ac.diac[pt->n - 1] == DIAC_SUKUN && ac.emis > best_sc) {
                            best_sc = ac.emis; best_k = k;
                        }
                    }
                    if (best_k != UINT32_MAX) { override_idx[i - 1] = -1; chosen[i - 1] = best_k; }
                    best_sc = NEG_INF; best_k = UINT32_MAX;
                    for (uint32_t k = 0; k < t.cand_count; ++k) {
                        const Cand& ac = ctx.cands[t.cand_begin + k];
                        if (ac.diac[0] == DIAC_FATHA && ac.emis > best_sc) {
                            best_sc = ac.emis; best_k = k;
                        }
                    }
                    if (best_k != UINT32_MAX) { override_idx[i] = -1; chosen[i] = best_k; }
                }

                const uint8_t* ts = &ctx.syms[t.off];
                if (pt && !t.boundary_before && is_an_prep_word(&ctx.syms[pt->off], pt->n) && is_bare_baad(ts, t.n)) {
                    const uint8_t want_end = (nx && nx->si.particle == PART_NONE && nx->si.verb_like < 2 && !nx->si.has_prep)
                                             ? DIAC_KASRA : DIAC_KASRATAN;
                    int32_t best_sc = NEG_INF;
                    uint32_t best_k = UINT32_MAX;
                    for (uint32_t k = 0; k < t.cand_count; ++k) {
                        const Cand& ac = ctx.cands[t.cand_begin + k];
                        if (ac.diac[0] == DIAC_DAMMA && diac_family(ac.diac[2]) == FAM_KASRA) {
                            int32_t sc = ac.emis + ((ac.diac[2] == want_end) ? 2000 : 0);
                            if (sc > best_sc) { best_sc = sc; best_k = k; }
                        }
                    }
                    if (best_k != UINT32_MAX) { override_idx[i] = -1; chosen[i] = best_k; }
                }

                // Law J: PART_JAZM + imperfect verb -> never DAMMA when FST has SUKUN/FATHA (or n >= 4)
                if (pt && !t.boundary_before && pt->si.particle == PART_JAZM &&
                    (cur.verb_pattern == 1 || (t.top_verb == 1 && starts_like_imperf(ts, t.n))) &&
                    !c_al && !(cur.flags & CF_SUFFIX) && cur.case_pos == t.n - 1 &&
                    ts[cur.case_pos] != S_WAW && ts[cur.case_pos] != S_YEH && ts[cur.case_pos] != S_ALEF_MAKSURA &&
                    cf == FAM_DAMMA) {
                    const uint8_t target_j = diac_has_shadda(cur.diac[cur.case_pos]) ? FAM_FATHA : FAM_SUKUN;
                    enforce_case_fam(i, target_j, 0, 3500, t.n >= 4);
                }

                // Law B: Diptotes governed by preposition, idafa, or proper diptote never taking KASRA
                const bool pt_is_mudaf_gen = pt && pt->si.particle == PART_NONE && pc->verb_pattern == 0 &&
                                             !(pc->flags & CF_HAS_AL) && !diac_is_tanween(pc->diac[pc->case_pos]) &&
                                             diac_family(pc->diac[pc->case_pos]) == FAM_KASRA;
                const bool dip_noun = c_noun_any ||
                    ((cur.ext_flags & EF_DIPTOTE) && t.has_noun_lex && pt &&
                     (pt->si.particle == PART_GEN || pt->si.has_prep));
                if ((cur.ext_flags & EF_DIPTOTE) && !c_al && dip_noun &&
                    !(pt && pt->digit_after) &&
                    ((c_gen && t.word_id == UINT32_MAX) ||
                     (pt && (pt->si.particle == PART_GEN || pt->si.particle == PART_IDAFA_NOUN ||
                             pt->si.particle == PART_NUM_IDAFA || pt->si.has_prep || pt_is_mudaf_gen)))) {
                    const bool proper_dip = (t.is_diptote_lex == 2) ||
                                            (classify_diptote_stem(&ctx.syms[t.off], t.n, t.si.al_len, cur.diac) == 2);
                    const bool nx_mudaf = !proper_dip && (t.digit_after ||
                                          (nx && nx->si.particle == PART_NONE && nx->si.verb_like < 2 &&
                                           nx->top_verb != 1 && !nx->is_ibn && !nx->si.has_prep &&
                                           (nx->si.al_len > 0 || !nx->is_adj_like)));
                    if (nx_mudaf) {
                        enforce_case_fam(i, FAM_KASRA, 0, 4000, true);
                    } else {
                        enforce_case_fam(i, FAM_FATHA, 0, 4000, true);
                    }
                } else if ((cur.ext_flags & EF_DIPTOTE) && !c_al && c_noun_any && t.is_diptote_lex == 2 && cf == FAM_KASRA) {
                    enforce_case_fam(i, FAM_FATHA, 0, 4000, true);
                }
                // Proper name شُعْبَةَ after عن / بن
                if (pt && !c_al && c_noun_any && t.n == 4 &&
                    ctx.syms[t.off] == S_SHEEN && ctx.syms[t.off + 1] == S_AIN &&
                    ctx.syms[t.off + 2] == S_BEH && ctx.syms[t.off + 3] == S_TEH_MARBUTA &&
                    (pt->si.particle == PART_GEN || (pt->surf_flags & SF_IBN))) {
                    enforce_case_fam(i, FAM_FATHA, 0, 4000, true);
                }

                // Law C: Preposition / shadda-أيّ + non-diptote noun -> mandatory KASRA
                if (c_noun_any && !comp_num_tok && !(cur.ext_flags & (EF_DIPTOTE | EF_DUAL_NOON | EF_MASC_PL_NOON)) &&
                    !(t.surf_flags & (SF_BAAD_QABL | SF_AMS | SF_INNA_PRON)) && !(pt && pt->digit_after) &&
                    cf != FAM_NONE && cf != FAM_SUKUN && cf != FAM_KASRA) {
                    const bool prev_man_pron = pt && (pt->surf_flags & SF_MAN) && pc->diac[pt->n - 2] == DIAC_FATHA;
                    const bool prev_prep = pt && !diac_is_tanween(pc->diac[pc->case_pos]) &&
                                                  ((pt->si.particle == PART_GEN && !prev_man_pron) ||
                                                   (pt->si.particle == PART_AY && diac_has_shadda(pc->diac[pt->n - 1])));
                    if ((c_gen && t.word_id == UINT32_MAX) || prev_prep) {
                        enforce_case_fam(i, FAM_KASRA, -1, 4000, true);
                    }
                }

                // Law D: Clause Government corrections on bare nouns (not governed by prep/idafa)
                if (pt && c_noun && prev_np_fam == FAM_NONE && !pt->digit_after) {
                    if (gov == GOV_VERB_NEED_SUBJ && (pc->ext_flags & EF_PAST_TEH_FEM) && c_al) {
                        if ((pc->diac[pt->n - 1] == DIAC_KASRA && cf == FAM_KASRA) ||
                            (cf == FAM_FATHA && ctx.syms[t.off + t.n - 1] == S_TEH_MARBUTA)) {
                            enforce_case_fam(i, FAM_DAMMA, -1, 2600, true);
                        }
                    }
                    // Law E: إنَّ وأخواتها + immediate bare noun -> FATHA (or KASRA for -ات)
                    if (pt->si.particle == PART_INNA ||
                        ((pt->si.particle == PART_AN || pt->si.particle == PART_LI_AN || pt->si.particle == PART_LAKIN) &&
                         pc->morph_class == MC_PART_SHADDA)) {
                        if (cur.ext_flags & EF_FEM_PL_AT) {
                            enforce_case_fam(i, FAM_KASRA, -1, 3000, true);
                        } else if (!(cur.ext_flags & (EF_DUAL_NOON | EF_MASC_PL_NOON))) {
                            enforce_case_fam(i, FAM_FATHA, -1, 3000, true);
                        }
                    }
                    // Law F: لَمْ يَكُنْ إِلَّا رَأْسٌ
                    if (i >= 2 && pt->si.particle == PART_ILLA && !pt->boundary_before &&
                        ctx.toks[i - 2].si.particle == PART_KANA &&
                        i >= 3 && !ctx.toks[i - 2].boundary_before &&
                        (ctx.toks[i - 3].si.particle == PART_JAZM || ctx.toks[i - 3].si.particle == PART_LA ||
                         ctx.toks[i - 3].si.particle == PART_NASB)) {
                        enforce_case_fam(i, FAM_DAMMA, -1, 3000, true);
                    }
                }

                // Law G1: Proper diptote name at BOS or after active past verb followed by بن -> DAMMA (عُمَرُ بْنُ الخَطَّابِ)
                if ((t.boundary_before || (pt && pt->top_pos == POS_VERB_PAST_3SG)) &&
                    (t.is_diptote_lex == 2 || classify_diptote_stem(&ctx.syms[t.off], t.n, t.si.al_len, cur.diac) == 2) &&
                    c_noun && nx && (nx->surf_flags & SF_IBN)) {
                    enforce_case_fam(i, FAM_DAMMA, 0, 3500, true);
                }

                // Law G2: Dynamic Pass-2 بْن / ابْن agreement using actual chosen case of preceding name
                if (pt && (t.surf_flags & SF_IBN)) {
                    uint8_t target_ibn = FAM_NONE;
                    if (i >= 2 && !pt->boundary_before && (ctx.toks[i - 2].surf_flags & SF_NAME_HEAD)) {
                        const Cand& p2c = get_cand(i - 2);
                        target_ibn = diac_family(p2c.diac[p2c.case_pos]);
                    } else if (pt->si.particle == PART_NONE && pc->verb_pattern == 0 && !(pc->flags & CF_OOV)) {
                        if (pc->ext_flags & EF_DIPTOTE) {
                            if (i >= 2 && !pt->boundary_before) {
                                const Token& p2t = ctx.toks[i - 2];
                                const Cand& p2c = get_cand(i - 2);
                                if (p2t.si.particle == PART_GEN || p2t.si.particle == PART_IDAFA_NOUN ||
                                    p2t.si.has_prep || (p2c.flags & CF_GEN) ||
                                    diac_family(p2c.diac[p2c.case_pos]) == FAM_KASRA) {
                                    target_ibn = FAM_KASRA;
                                } else {
                                    target_ibn = diac_family(pc->diac[pc->case_pos]);
                                }
                            } else {
                                target_ibn = diac_family(pc->diac[pc->case_pos]);
                            }
                        } else {
                            target_ibn = diac_family(pc->diac[pc->case_pos]);
                        }
                    }
                    if ((target_ibn == FAM_DAMMA || target_ibn == FAM_FATHA || target_ibn == FAM_KASRA)) {
                        enforce_case_fam(i, target_ibn, 0, 3500, true);
                    }
                }
            }

            // Law H: Multi-word Na't chain + attached-conjunction feminine-plural agreement
            {
                const Cand& cur = get_cand(i);
                const uint8_t cf = diac_family(cur.diac[cur.case_pos]);
                const bool c_al = (cur.flags & CF_HAS_AL) || t.si.al_len > 0;
                const bool c_gen = (cur.flags & CF_GEN) || t.si.has_prep;
                const bool c_noun = (cur.verb_pattern == 0) && t.si.particle == PART_NONE && !c_gen;
                if (!t.boundary_before && i >= 1 && !ctx.toks[i - 1].digit_after && c_noun && prev_np_fam != FAM_NONE &&
                    prev_np_fam != FAM_SUKUN && cf != prev_np_fam) {
                    if (!(cur.ext_flags & EF_FEM_PL_AT)) {
                        const bool c_indef_adj = t.is_adj_like || (t.n > t.si.al_len + 3 && ctx.syms[t.off + t.si.al_len] == S_MEEM &&
                                                                   cur.morph_class == MC_NOUN_SHADDA_CORE);
                        if (c_al && (prev_np_al || prev_np_suffix)) {
                            enforce_case_fam(i, prev_np_fam, 0, 2600, true);
                        } else if (!c_al && !(cur.flags & CF_SUFFIX) && c_indef_adj && prev_np_tan) {
                            enforce_case_fam(i, prev_np_fam, 1, 2600, true);
                        }
                    } else if ((t.si.has_conj || ((ctx.syms[t.off] == S_WAW || ctx.syms[t.off] == S_FEH) && cur.diac[0] == DIAC_FATHA)) &&
                               !t.si.has_prep && prev_np_fam == FAM_DAMMA &&
                               (prev_np_al == c_al) && (c_al || prev_np_tan) && cf == FAM_KASRA) {
                        // مُحَاضَرَاتٌ وَوَرَشَاتٌ (nominative sound feminine plural conjoined with وَ)
                        enforce_case_fam(i, FAM_DAMMA, c_al ? 0 : 1, 2600, true);
                    }
                }
            }

            // Refresh fin after potential Pass-2 switch
            const Cand& fin = get_cand(i);
            const bool c_al = (fin.flags & CF_HAS_AL) || t.si.al_len > 0;
            uint8_t fin_fam = diac_family(fin.diac[fin.case_pos]);
            const bool fin_gen = (fin.flags & CF_GEN) || t.si.has_prep;
            if (fin_fam == FAM_NONE) {
                if (fin_gen) {
                    fin_fam = FAM_KASRA;
                } else if (i >= 1 && !t.boundary_before) {
                    const Token& pt = ctx.toks[i - 1];
                    const Cand& pc = get_cand(i - 1);
                    if (pt.si.particle == PART_GEN || pt.si.particle == PART_IDAFA_NOUN || pt.si.particle == PART_NUM_IDAFA ||
                        (c_al && pc.verb_pattern == 0 && pt.si.particle == PART_NONE && !(pc.flags & CF_HAS_AL) && pt.si.al_len == 0)) {
                        // Implicit genitive on مقصور/منقوص after prep or idafa head (دَارِ الأُوبِرَا المَلَكِيَّةِ)
                        fin_fam = FAM_KASRA;
                    } else if (gov == GOV_INNA_ISM) {
                        fin_fam = FAM_FATHA;
                    } else if (gov == GOV_VERB_NEED_SUBJ || gov == GOV_VERB_PASS || gov == GOV_KANA_ISM) {
                        fin_fam = FAM_DAMMA;
                    }
                } else if (gov == GOV_INNA_ISM) {
                    fin_fam = FAM_FATHA;
                } else if (gov == GOV_VERB_NEED_SUBJ || gov == GOV_VERB_PASS || gov == GOV_KANA_ISM) {
                    fin_fam = FAM_DAMMA;
                }
            }

            // Update clause government state
            if (fin.verb_pattern == 1) {
                if (fin.pos_tag == POS_VERB_PASS) gov = GOV_VERB_PASS;
                else if (fin.pos_tag == POS_VERB_PAST_SUBJ || fin.pos_tag == POS_VERB_IMPERF_12) gov = GOV_VERB_HAVE_SUBJ;
                else gov = GOV_VERB_NEED_SUBJ;
                prev_np_fam = FAM_NONE;
                prev_np_al = false;
                prev_np_suffix = false;
                prev_np_tan = false;
                head_np_fam = FAM_NONE;
            } else if (t.si.particle == PART_INNA ||
                       ((t.si.particle == PART_AN || t.si.particle == PART_LI_AN || t.si.particle == PART_LAKIN) &&
                        fin.morph_class == MC_PART_SHADDA)) {
                gov = GOV_INNA_ISM;
                prev_np_fam = FAM_NONE;
                prev_np_al = false;
                prev_np_suffix = false;
                prev_np_tan = false;
                head_np_fam = FAM_NONE;
            } else if (t.si.particle == PART_KANA) {
                gov = GOV_KANA_ISM;
                prev_np_fam = FAM_NONE;
                prev_np_al = false;
                prev_np_suffix = false;
                prev_np_tan = false;
                head_np_fam = FAM_NONE;
            } else if (fin.verb_pattern == 0 && (t.si.particle == PART_NONE || t.si.particle == PART_IDAFA_NOUN)) {
                if (gov == GOV_VERB_NEED_SUBJ && fin_fam == FAM_DAMMA && !fin_gen) gov = GOV_VERB_HAVE_SUBJ;
                else if (gov == GOV_INNA_ISM && fin_fam == FAM_FATHA && !fin_gen) gov = GOV_NONE;
                else if (gov == GOV_KANA_ISM && fin_fam == FAM_DAMMA && !fin_gen) gov = GOV_NONE;

                if (fin_fam != FAM_NONE && fin_fam != FAM_SUKUN) {
                    if (!prev_np_al && c_al) {
                        if (i >= 1 && (ctx.toks[i - 1].si.particle == PART_GEN || ctx.toks[i - 1].si.has_prep)) {
                            head_np_fam = fin_fam;
                            head_np_al = c_al;
                        }
                    } else {
                        head_np_fam = fin_fam;
                        head_np_al = c_al;
                    }
                    prev_np_fam = fin_fam;
                    prev_np_al = c_al;
                    prev_np_suffix = (fin.flags & CF_SUFFIX) != 0;
                    prev_np_tan = diac_is_tanween(fin.diac[fin.case_pos]);
                } else {
                    prev_np_fam = FAM_NONE;
                    prev_np_al = false;
                    prev_np_suffix = false;
                    prev_np_tan = false;
                }
            } else if (t.si.particle == PART_CONJ && i + 1 < W && head_np_fam != FAM_NONE) {
                prev_np_fam = head_np_fam;
                prev_np_al = head_np_al;
                prev_np_suffix = false;
                prev_np_tan = false;
            } else if (t.si.particle != PART_NONE) {
                prev_np_fam = FAM_NONE;
                prev_np_al = false;
                prev_np_suffix = false;
                prev_np_tan = false;
                if (t.si.particle != PART_CONJ && t.si.particle != PART_PREP_PRON && t.si.particle != PART_DEM) {
                    head_np_fam = FAM_NONE;
                    head_np_al = false;
                }
                if (t.si.particle == PART_REL || t.si.particle == PART_COND) gov = GOV_NONE;
            }
        }
    }

    std::string out;
    out.reserve(len * 2 + 16);
    for (const auto& sp : spans) {
        if (!sp.is_word) {
            out.append(raw_pool, sp.a, sp.b - sp.a);
            continue;
        }
        const Token& t = ctx.toks[sp.a];
        if (t.cand_count == 0) {
            for (size_t k = 0; k < t.n; ++k) utf8_append(out, ctx.cps[t.off + k]);
            continue;
        }
        const Cand& c = (override_idx[sp.a] >= 0) ? override_cands[override_idx[sp.a]] : ctx.cands[t.cand_begin + chosen[sp.a]];
        for (size_t k = 0; k < t.n; ++k) {
            uint32_t cp = ctx.cps[t.off + k];
            if (c.repl_pos == k) cp = sym_to_cp(c.repl_sym);
            utf8_append(out, cp);
            append_diacritic(out, c.diac[k]);
        }
    }
    return out;
}

}
