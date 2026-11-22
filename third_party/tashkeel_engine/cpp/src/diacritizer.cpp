#include "tashkeel/diacritizer.hpp"
#include "tashkeel/tiny_inflate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
           s[al_len] != S_ALEF_HAMZA_A && s[al_len] != S_ALEF_HAMZA_B && s[al_len] != S_ALEF;
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
        if (sn == 4 && w[0] == S_SEEN && w[1] == S_LAM && w[2] == S_MEEM) return 2; // سلمة
        if (sn == 4 && w[0] == S_ZAIN && w[1] == S_REH && w[2] == S_AIN) return 2; // زرعة
        if (sn == 4 && w[0] == S_SHEEN && w[1] == S_AIN && w[2] == S_BEH) return 2; // شعبة
        if (sn == 4 && w[0] == S_SHEEN && w[1] == S_YEH && w[2] == S_BEH) return 2; // شيبة
        if (sn == 4 && w[0] == S_HAH && w[1] == S_FEH && w[2] == S_SAD) return 2; // حفصة
        if (sn == 4 && w[0] == S_SEEN && w[1] == S_WAW && w[2] == S_DAL) return 2; // سودة
        if (sn == 6 && w[0] == S_HEH && w[1] == S_REH && w[2] == S_YEH && w[3] == S_REH && w[4] == S_TEH_MARBUTA) return 2; // هريرة
        if (sn == 6 && w[0] == S_HAH && w[1] == S_NOON && w[2] == S_YEH && w[3] == S_FEH && w[4] == S_TEH_MARBUTA) return 2; // حنيفة
        if (sn == 6 && w[0] == S_QAF && w[1] == S_TEH && w[2] == S_YEH && w[3] == S_BEH && w[4] == S_TEH_MARBUTA) return 2; // قتيبة
        if (sn == 6 && w[0] == S_AIN && w[1] == S_YEH && w[2] == S_YEH && w[3] == S_NOON && w[4] == S_TEH_MARBUTA) return 2; // عيينة
        if (sn == 6 && w[0] == S_MEEM && w[1] == S_LAM && w[2] == S_YEH && w[3] == S_KAF && w[4] == S_TEH_MARBUTA) return 2; // مليكة
        if (sn == 6 && w[0] == S_AIN && w[1] == S_BEH && w[2] == S_YEH && w[3] == S_DAL && w[4] == S_TEH_MARBUTA) return 2; // عبيدة
        if (sn == 5 && w[0] == S_ALEF_HAMZA_A && w[1] == S_MEEM && w[2] == S_YEH && w[3] == S_TEH_MARBUTA) return 2; // أمية
        if (sn == 6 && w[0] == S_AIN && w[1] == S_NOON && w[2] == S_BEH && w[3] == S_SEEN && w[4] == S_TEH_MARBUTA) return 2; // عنبسة
        if (sn == 5 && w[0] == S_REH && w[1] == S_QAF && w[2] == S_YEH && w[3] == S_TEH_MARBUTA) return 2; // رقية
        if (sn == 5 && w[0] == S_SEEN && w[1] == S_MEEM && w[2] == S_YEH && w[3] == S_TEH_MARBUTA) return 2; // سمية
        if (sn == 5 && w[0] == S_KHAH && w[1] == S_WAW && w[2] == S_LAM && w[3] == S_TEH_MARBUTA) return 2; // خولة
        if (sn == 5 && w[0] == S_SAD && w[1] == S_FEH && w[2] == S_YEH && w[3] == S_TEH_MARBUTA) return 2; // صفية
        return 0;
    }
    if (sn == 5 && w[2] == S_ALEF && w[4] != S_ALEF && w[4] != S_ALEF_MAKSURA &&
        (w[3] == S_YEH_HAMZA || w[0] == S_MEEM || w[1] == S_WAW)) return 1;
    if (sn == 6 && w[2] == S_ALEF && w[4] == S_YEH && w[5] != S_YEH &&
        (w[0] == S_MEEM || w[0] == S_TEH || w[0] == S_ALEF_HAMZA_A)) return 1;
    if (sn >= 5 && w[sn - 2] == S_ALEF && w[sn - 1] == S_HAMZA &&
        w[0] != S_ALEF && w[0] != S_ALEF_HAMZA_B && w[sn - 3] != S_ALEF &&
        !(sn == 5 && w[0] == S_ALEF_HAMZA_A && !(w[1] == S_SHEEN && w[2] == S_YEH))) return 1;
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
    if (sn == 4 && w[0] == S_ALEF_HAMZA_A && w[1] == S_YEH && w[2] == S_WAW && w[3] == S_BEH) return 2; // أيوب
    if (sn == 5 && (w[0] == S_ALEF_HAMZA_B || w[0] == S_ALEF) && w[1] == S_LAM && w[2] == S_YEH && w[3] == S_ALEF && w[4] == S_SEEN) return 2; // إلياس
    if (sn == 4 && w[0] == S_YEH && w[1] == S_WAW && w[2] == S_NOON && w[3] == S_SEEN) return 2; // يونس
    if (sn == 5 && w[0] == S_HEH && w[1] == S_ALEF && w[2] == S_REH && w[3] == S_WAW && w[4] == S_NOON) return 2; // هارون
    if (sn == 5 && w[0] == S_ALEF_HAMZA_B && w[1] == S_SEEN && w[2] == S_HAH && w[3] == S_ALEF && w[4] == S_QAF) return 2; // إسحاق
    if (sn == 6 && w[0] == S_SEEN && w[1] == S_LAM && w[2] == S_YEH && w[3] == S_MEEM && w[4] == S_ALEF && w[5] == S_NOON) return 2; // سليمان
    if (sn == 7 && (w[0] == S_ALEF_HAMZA_B || w[0] == S_ALEF) && w[1] == S_SEEN && w[2] == S_REH && w[3] == S_ALEF && w[4] == S_YEH_HAMZA && w[5] == S_YEH && w[6] == S_LAM) return 2;
    if (sn == 7 && w[0] == S_ALEF_HAMZA_B && w[1] == S_BEH && w[2] == S_REH && w[3] == S_ALEF && w[4] == S_HEH && w[5] == S_YEH && w[6] == S_MEEM) return 2;
    if (sn == 7 && w[0] == S_ALEF_HAMZA_B && w[1] == S_SEEN && w[2] == S_MEEM && w[3] == S_ALEF && w[4] == S_AIN && w[5] == S_YEH && w[6] == S_LAM) return 2;
    if (sn == 6 && w[0] == S_DAL && w[1] == S_ALEF && w[2] == S_REH && w[3] == S_FEH && w[4] == S_WAW && w[5] == S_REH) return 2;
    if (sn == 5 && w[0] == S_QAF && w[1] == S_REH && w[2] == S_TAH && w[3] == S_ALEF && w[4] == S_JEEM) return 2;
    if (sn == 7 && w[0] == S_NOON && w[1] == S_YEH && w[2] == S_WAW && w[3] == S_YEH && w[4] == S_WAW && w[5] == S_REH && w[6] == S_KAF) return 2;
    if (sn == 6 && w[0] == S_WAW && w[1] == S_ALEF && w[2] == S_SHEEN && w[3] == S_NOON && w[4] == S_TAH && w[5] == S_NOON) return 2;
    if (sn == 5 && w[0] == S_MEEM && w[1] == S_WAW && w[2] == S_SEEN && w[3] == S_KAF && w[4] == S_WAW) return 2;
    if (sn == 5 && w[0] == S_TAH && w[1] == S_WAW && w[2] == S_KAF && w[3] == S_YEH && w[4] == S_WAW) return 2;
    if (sn == 4 && w[0] == S_BEH && w[1] == S_KAF && w[2] == S_YEH && w[3] == S_NOON) return 2;
    if (sn == 5 && w[0] == S_MEEM && w[1] == S_REH && w[2] == S_ALEF && w[3] == S_KAF && w[4] == S_SHEEN) return 2;
    if (sn == 5 && w[0] == S_SAD && w[1] == S_NOON && w[2] == S_AIN && w[3] == S_ALEF && w[4] == S_HAMZA) return 2;
    if (sn == 5 && w[0] == S_TAH && w[1] == S_HEH && w[2] == S_REH && w[3] == S_ALEF && w[4] == S_NOON) return 2;
    if (sn == 7 && (w[0] == S_ALEF_HAMZA_B || w[0] == S_ALEF) && w[1] == S_SEEN && w[2] == S_TAH && w[3] == S_NOON && w[4] == S_BEH && w[5] == S_WAW && w[6] == S_LAM) return 2;
    if (sn == 5 && w[0] == S_ALEF_HAMZA_A && w[1] == S_REH && w[2] == S_YEH && w[3] == S_HAH && w[4] == S_ALEF) return 2;
    if (sn == 5 && w[0] == S_NOON && w[1] == S_ALEF && w[2] == S_BEH && w[3] == S_LAM && w[4] == S_SEEN) return 2;
    return 0;
}

inline uint32_t fnv1a_32_syms(const uint8_t* s, size_t n) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; ++i) {
        h ^= s[i];
        h *= 0x01000193u;
    }
    return h;
}

inline bool is_month_word(const uint8_t* s, size_t n) {
    if (n < 3) return false;
    if (n == 5 && s[0] == S_YEH && s[1] == S_NOON && s[2] == S_ALEF && s[3] == S_YEH && s[4] == S_REH) return true;
    if (n == 6 && s[0] == S_FEH && s[1] == S_BEH && s[2] == S_REH && s[3] == S_ALEF && s[4] == S_YEH && s[5] == S_REH) return true;
    if (n == 4 && s[0] == S_MEEM && s[1] == S_ALEF && s[2] == S_REH && s[3] == S_SEEN) return true;
    if (n == 5 && (s[0] == S_ALEF_HAMZA_A || s[0] == S_ALEF) && s[1] == S_BEH && s[2] == S_REH && s[3] == S_YEH && s[4] == S_LAM) return true;
    if (n == 4 && s[0] == S_MEEM && s[1] == S_ALEF && s[2] == S_YEH && s[3] == S_WAW) return true;
    if (n == 5 && s[0] == S_YEH && s[1] == S_WAW && s[2] == S_NOON && s[3] == S_YEH && s[4] == S_WAW) return true;
    if (n == 5 && s[0] == S_YEH && s[1] == S_WAW && s[2] == S_LAM && s[3] == S_YEH && s[4] == S_WAW) return true;
    if (n == 5 && (s[0] == S_ALEF_HAMZA_A || s[0] == S_ALEF) && s[1] == S_GHAIN && s[2] == S_SEEN && s[3] == S_TAH && s[4] == S_SEEN) return true;
    if (n >= 6 && s[n - 2] == S_BEH && s[n - 1] == S_REH &&
        (s[0] == S_SEEN || s[0] == S_ALEF_HAMZA_A || s[0] == S_ALEF || s[0] == S_NOON || s[0] == S_DAL)) return true;
    if (n == 5 && s[0] == S_REH && s[1] == S_MEEM && s[2] == S_DAD && s[3] == S_ALEF && s[4] == S_NOON) return true;
    if (n == 5 && s[0] == S_SHEEN && s[1] == S_AIN && s[2] == S_BEH && s[3] == S_ALEF && s[4] == S_NOON) return true;
    if (n == 4 && s[0] == S_SHEEN && s[1] == S_WAW && s[2] == S_ALEF && s[3] == S_LAM) return true;
    if (n == 3 && s[0] == S_REH && s[1] == S_JEEM && s[2] == S_BEH) return true;
    if (n == 3 && s[0] == S_SAD && s[1] == S_FEH && s[2] == S_REH) return true;
    if (n == 4 && s[0] == S_MEEM && s[1] == S_HAH && s[2] == S_REH && s[3] == S_MEEM) return true;
    return false;
}

inline bool is_num_scale_word(const uint8_t* s, size_t n) {
    if (n == 3 && (s[0] == S_ALEF_HAMZA_A || s[0] == S_ALEF) && s[1] == S_LAM && s[2] == S_FEH) return true;
    if (n == 4 && s[0] == S_ALEF_MADDA && s[1] == S_LAM && s[2] == S_ALEF && s[3] == S_FEH) return true;
    if (n == 5 && s[0] == S_MEEM && s[1] == S_LAM && s[2] == S_YEH && s[3] == S_WAW && s[4] == S_NOON) return true;
    if (n == 6 && s[0] == S_MEEM && s[1] == S_LAM && s[2] == S_ALEF && s[3] == S_YEH && s[4] == S_YEH && s[5] == S_NOON) return true;
    if (n == 5 && s[0] == S_MEEM && s[1] == S_LAM && s[2] == S_YEH && s[3] == S_ALEF && s[4] == S_REH) return true;
    if (n == 5 && s[0] == S_BEH && s[1] == S_LAM && s[2] == S_YEH && s[3] == S_WAW && s[4] == S_NOON) return true;
    if ((n == 3 && s[0] == S_MEEM && s[1] == S_YEH_HAMZA && s[2] == S_TEH_MARBUTA) ||
        (n == 4 && s[0] == S_MEEM && s[1] == S_ALEF && s[2] == S_YEH_HAMZA && s[3] == S_TEH_MARBUTA)) return true;
    return false;
}

inline bool is_prep_cause_word(const uint8_t* s, size_t n) {
    // بِسَبَبِ / لِسَبَبِ
    return n == 4 && (s[0] == S_BEH || s[0] == S_LAM) && s[1] == S_SEEN && s[2] == S_BEH && s[3] == S_BEH;
}

inline bool is_haythu_word(const uint8_t* s, size_t n) {
    size_t st = 0;
    if (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH)) st = 1;
    if (n - st >= 4 && s[st] == S_BEH) ++st;
    return (n - st == 3) && s[st] == S_HAH && s[st + 1] == S_YEH && s[st + 2] == S_THEH;
}

inline bool is_allah_word(const uint8_t* s, size_t n) {
    return n == 4 && s[0] == S_ALEF && s[1] == S_LAM && s[2] == S_LAM && s[3] == S_HEH;
}

inline bool is_declinable_prep_noun(const uint8_t* s, size_t n) {
    if (n < 2) return false;
    if (n == 3 && s[0] == S_BEH && s[1] == S_YEH && s[2] == S_NOON) return true;
    if (n == 3 && s[0] == S_HAH && s[1] == S_YEH && s[2] == S_NOON) return true;
    if (n == 3 && s[0] == S_DAL && s[1] == S_WAW && s[2] == S_NOON) return true;
    if (n == 4 && s[0] == S_KHAH && s[1] == S_LAM && s[2] == S_ALEF && s[3] == S_LAM) return true;
    if (n == 3 && s[0] == S_WAW && s[1] == S_SEEN && s[2] == S_TAH) return true;
    if (n == 3 && s[0] == S_AIN && s[1] == S_NOON && s[2] == S_DAL) return true;
    if (n == 3 && s[0] == S_NOON && s[1] == S_HAH && s[2] == S_WAW) return true;
    if (n == 3 && s[0] == S_AIN && s[1] == S_BEH && s[2] == S_REH) return true;
    if (n == 3 && s[0] == S_FEH && s[1] == S_WAW && s[2] == S_QAF) return true;
    if (n == 3 && s[0] == S_TEH && s[1] == S_HAH && s[2] == S_TEH) return true;
    if (n == 3 && s[0] == S_HAH && s[1] == S_WAW && s[2] == S_LAM) return true;
    if (n == 4 && (s[0] == S_ALEF_HAMZA_A || s[0] == S_ALEF) && s[1] == S_MEEM && s[2] == S_ALEF && s[3] == S_MEEM) return true;
    if (n == 3 && s[0] == S_KHAH && s[1] == S_LAM && s[2] == S_FEH) return true;
    if (n == 4 && s[0] == S_WAW && s[1] == S_REH && s[2] == S_ALEF && s[3] == S_HAMZA) return true;
    return false;
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
        wd[2] == DIAC_FATHA && w[3] != S_ALEF && w[3] != S_ALEF_MAKSURA) {
        return 1;
    }
    if (sn == 3 && w[0] == S_ALEF_MADDA && w[1] == S_KHAH && w[2] == S_REH && wd[0] == DIAC_FATHA) {
        return 1;
    }
    if (sn >= 5 && w[sn - 2] == S_ALEF && w[sn - 1] == S_HAMZA &&
        w[0] != S_ALEF && w[0] != S_ALEF_HAMZA_B && w[sn - 3] != S_ALEF &&
        !(sn == 5 && w[0] == S_ALEF_HAMZA_A && wd[0] == DIAC_FATHA && !(w[1] == S_SHEEN && w[2] == S_YEH))) {
        if (sn >= 6 || wd[0] == DIAC_DAMMA || wd[0] == DIAC_FATHA) return 1;
    }
    if (sn == 3 && w[0] == S_ALEF_HAMZA_A && wd[0] == DIAC_FATHA && diac_has_shadda(wd[2]) && !diac_is_tanween(wd[2])) {
        return 1; // أَحَبّ، أَهَمّ، أَقَلّ، أَشَدّ
    }
    if (sn == 5 && !diac_is_tanween(wd[4]) && w[3] == S_ALEF && w[4] == S_NOON &&
        w[0] != S_ALEF && w[0] != S_ALEF_HAMZA_A && w[0] != S_ALEF_HAMZA_B &&
        !(w[0] == S_SEEN && w[1] == S_LAM && w[2] == S_TAH) &&
        !(w[0] == S_SHEEN && w[1] == S_YEH && w[2] == S_TAH) &&
        !(w[0] == S_BEH && w[1] == S_REH && w[2] == S_HEH) &&
        !(w[0] == S_QAF && w[1] == S_REH) &&
        !(w[0] == S_MEEM && w[1] == S_KAF) &&
        ((wd[1] == DIAC_SUKUN && (wd[0] == DIAC_FATHA || wd[0] == DIAC_DAMMA || wd[0] == DIAC_KASRA)) ||
         (wd[1] == DIAC_SHADDA_FATHA && wd[0] == DIAC_FATHA))) {
        return 1; // عُثْمَان، حَمْدَان، مَرْوَان، سَلْمَان، عَدْنَان، عَفَّان، حَسَّان
    }
    if (sn == 3 && w[0] == S_AIN && w[1] == S_MEEM && w[2] == S_REH && wd[0] == DIAC_DAMMA && wd[1] == DIAC_FATHA) return 2; // عُمَر
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

inline bool is_kull_word(const uint8_t* s, size_t n) {
    size_t st = (n == 3 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM || s[0] == S_KAF)) ? 1 :
                ((n == 4 && (s[0] == S_WAW || s[0] == S_FEH) && (s[1] == S_BEH || s[1] == S_LAM)) ? 2 : 0);
    return (n - st == 2) && s[st] == S_KAF && s[st + 1] == S_LAM;
}

inline bool is_umar_word(const uint8_t* s, size_t n) {
    size_t st = (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM)) ? 1 : 0;
    return (n - st == 3) && s[st] == S_AIN && s[st + 1] == S_MEEM && s[st + 2] == S_REH;
}

inline bool is_age_ctx_word(const uint8_t* s, size_t n) {
    if (n == 3 && s[0] == S_SEEN && s[1] == S_NOON && s[2] == S_TEH_MARBUTA) return true; // سنة
    if (n == 4 && s[0] == S_SEEN && s[1] == S_NOON && s[2] == S_YEH && s[3] == S_NOON) return true; // سنين
    if (n == 3 && s[0] == S_AIN && s[1] == S_ALEF && s[2] == S_MEEM) return true; // عام
    if (n == 5 && s[0] == S_ALEF_HAMZA_A && s[1] == S_AIN && s[2] == S_WAW && s[3] == S_ALEF && s[4] == S_MEEM) return true; // أعوام
    if (n == 5 && s[0] == S_YEH && s[1] == S_NOON && s[2] == S_ALEF && s[3] == S_HEH && s[4] == S_ZAIN) return true; // يناهز
    if (n == 5 && s[0] == S_YEH && s[1] == S_QAF && s[2] == S_ALEF && s[3] == S_REH && s[4] == S_BEH) return true; // يقارب
    return false;
}

inline bool is_ila_yeh_word(const uint8_t* s, size_t n) {
    size_t st = (n == 4 && (s[0] == S_WAW || s[0] == S_FEH)) ? 1 : 0;
    return (n - st == 3) && s[st] == S_ALEF_HAMZA_B && s[st + 1] == S_LAM && s[st + 2] == S_YEH;
}

inline bool is_aba_verb_word(const uint8_t* s, size_t n) {
    size_t st = (n == 4 && (s[0] == S_WAW || s[0] == S_FEH)) ? 1 : 0;
    return (n - st == 3) && s[st] == S_ALEF_HAMZA_A && s[st + 1] == S_BEH && s[st + 2] == S_ALEF_MAKSURA;
}

inline bool is_masdar_prep_adv(const uint8_t* s, size_t n) {
    // Indefinite verbal nouns taking FATHATAN before على / إلى / لـ / عن (بناءً على، استناداً إلى، وفقاً لـ...)
    if (n < 4) return false;
    if (s[n - 2] == S_ALEF && s[n - 1] == S_HAMZA) return true; // بناءً، فضلاً? (no, ماء/بناء/بقاء)
    if (s[n - 1] == S_ALEF) return true; // نظراً، وفقاً، طبقاً، استناداً، اعتماداً، خلافاً، بدلاً
    if (s[n - 1] == S_TEH_MARBUTA && n >= 5 && (s[0] == S_ALEF_HAMZA_B || s[1] == S_ALEF)) return true; // إضافةً، إشارةً، مقارنةً
    return false;
}

inline bool is_title_noun(const uint8_t* s, size_t n, size_t al_len) {
    if (n <= al_len + 2) return false;
    const size_t sn = n - al_len;
    const uint8_t* w = s + al_len;
    if (sn == 3 && w[0] == S_NOON && w[1] == S_BEH && w[2] == S_YEH) return true; // نبي
    if (sn == 4 && w[0] == S_REH && w[1] == S_SEEN && w[2] == S_WAW && w[3] == S_LAM) return true; // رسول
    if (sn == 5 && w[0] == S_DAL && w[1] == S_KAF && w[2] == S_TEH && w[3] == S_WAW && w[4] == S_REH) return true; // دكتور
    if (sn == 5 && w[0] == S_ALEF_HAMZA_A && w[1] == S_SEEN && w[2] == S_TEH && w[3] == S_ALEF && w[4] == S_THAL) return true; // أستاذ
    if (sn == 3 && w[0] == S_SHEEN && w[1] == S_YEH && w[2] == S_KHAH) return true; // شيخ
    if (sn == 4 && w[0] == S_ALEF_HAMZA_B && w[1] == S_MEEM && w[2] == S_ALEF && w[3] == S_MEEM) return true; // إمام
    if (sn == 4 && w[0] == S_REH && w[1] == S_ALEF_HAMZA_A && w[2] == S_YEH && w[3] == S_SEEN) return true; // رئيس
    if (sn == 4 && w[0] == S_REH && w[1] == S_YEH_HAMZA && w[2] == S_YEH && w[3] == S_SEEN) return true; // رئيس
    if (sn == 3 && w[0] == S_MEEM && w[1] == S_LAM && w[2] == S_KAF) return true; // ملك
    if (sn == 4 && w[0] == S_ALEF_HAMZA_A && w[1] == S_MEEM && w[2] == S_YEH && w[3] == S_REH) return true; // أمير
    if (sn == 4 && w[0] == S_WAW && w[1] == S_ZAIN && w[2] == S_YEH && w[3] == S_REH) return true; // وزير
    if (sn == 4 && w[0] == S_QAF && w[1] == S_ALEF && w[2] == S_DAD && w[3] == S_YEH) return true; // قاضي
    if (sn == 3 && w[0] == S_SEEN && w[1] == S_YEH && w[2] == S_DAL) return true; // سيد
    if (sn == 4 && w[0] == S_MEEM && w[1] == S_KHAH && w[2] == S_REH && w[3] == S_JEEM) return true; // مخرج
    if (sn == 4 && w[0] == S_LAM && w[1] == S_ALEF && w[2] == S_AIN && w[3] == S_BEH) return true; // لاعب
    if (sn == 4 && w[0] == S_MEEM && w[1] == S_DAL && w[2] == S_REH && w[3] == S_BEH) return true; // مدرب
    if (sn == 4 && w[0] == S_MEEM && w[1] == S_FEH && w[2] == S_TEH && w[3] == S_YEH) return true; // مفتي
    if (sn == 5 && w[0] == S_KHAH && w[1] == S_LAM && w[2] == S_YEH && w[3] == S_FEH && w[4] == S_TEH_MARBUTA) return true; // خليفة
    return false;
}

inline bool is_time_adverb_noun(const uint8_t* s, size_t n, size_t al_len) {
    if (n <= al_len + 2) return false;
    const size_t sn = n - al_len;
    const uint8_t* w = s + al_len;
    if (sn == 3 && w[0] == S_YEH && w[1] == S_WAW && w[2] == S_MEEM) return true; // يوم
    if (sn == 3 && w[0] == S_AIN && w[1] == S_ALEF && w[2] == S_MEEM) return true; // عام
    if (sn == 3 && w[0] == S_SEEN && w[1] == S_NOON && w[2] == S_TEH_MARBUTA) return true; // سنة
    if (sn == 3 && w[0] == S_SHEEN && w[1] == S_HEH && w[2] == S_REH) return true; // شهر
    if (sn == 4 && w[0] == S_SAD && w[1] == S_BEH && w[2] == S_ALEF && w[3] == S_HAH) return true; // صباح
    if (sn == 4 && w[0] == S_MEEM && w[1] == S_SEEN && w[2] == S_ALEF && w[3] == S_HAMZA) return true; // مساء
    if (sn == 3 && w[0] == S_LAM && w[1] == S_YEH && w[2] == S_LAM) return true; // ليل
    if (sn == 4 && w[0] == S_LAM && w[1] == S_YEH && w[2] == S_LAM && w[3] == S_TEH_MARBUTA) return true; // ليلة
    if (sn == 3 && w[0] == S_WAW && w[1] == S_QAF && w[2] == S_TEH) return true; // وقت
    if (sn == 3 && w[0] == S_HAH && w[1] == S_YEH && w[2] == S_NOON) return true; // حين
    if (sn == 4 && w[0] == S_SEEN && w[1] == S_ALEF && w[2] == S_AIN && w[3] == S_TEH_MARBUTA) return true; // ساعة
    if (sn == 3 && w[0] == S_MEEM && w[1] == S_REH && w[2] == S_TEH_MARBUTA) return true; // مرة
    return false;
}

inline bool is_kunya_dip_head(const uint8_t* s, size_t n) {
    size_t st = (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM)) ? 1 : 0;
    const size_t len = n - st;
    const uint8_t* w = s + st;
    if (len == 3 && w[0] == S_ALEF_HAMZA_A && w[1] == S_BEH && (w[2] == S_WAW || w[2] == S_YEH || w[2] == S_ALEF)) return true; // أبو/أبي/أبا
    if (len == 2 && w[0] == S_BEH && w[1] == S_NOON) return true; // بن
    if (len == 3 && (w[0] == S_ALEF || w[0] == S_ALEF_WASLA) && w[1] == S_BEH && w[2] == S_NOON) return true; // ابن
    if (len == 4 && w[0] == S_QAF && w[1] == S_NOON && w[2] == S_ALEF && w[3] == S_TEH_MARBUTA) return true; // قناة
    return false;
}

template <typename CandT>
inline bool is_invar_long_vowel_end(const uint8_t* s, size_t n, size_t cp, const CandT& c) {
    if (cp >= n) return true;
    if (s[cp] == S_ALEF || s[cp] == S_ALEF_MAKSURA) return true;
    if (!(c.flags & CF_SUFFIX) && cp + 1 == n && n >= 4 && s[cp] == S_WAW &&
        (c.diac[cp] == DIAC_NONE || c.diac[cp - 1] == DIAC_DAMMA ||
         ((c.flags & CF_OOV) && c.diac[cp - 1] != DIAC_SUKUN && c.diac[cp - 1] != DIAC_FATHA))) {
        return true;
    }
    return false;
}

template <typename TokT, typename CandT>
inline bool is_ali_proper_cand(const TokT& t, const CandT& c) {
    return (t.surf_flags & SF_ALI) && t.n >= 3 && diac_family(c.diac[t.n - 2]) == FAM_KASRA;
}

template <typename CandT>
inline bool has_verb_obj_suffix(const uint8_t* s, size_t n, const CandT& c) {
    if (c.verb_pattern != 1 || !(c.flags & CF_SUFFIX) || c.pos_tag == POS_VERB_PAST_SUBJ || c.pos_tag == POS_VERB_PASS) {
        return false;
    }
    size_t sl = n - (c.case_pos + 1);
    if (sl == 0) return false;
    const uint8_t* suf = s + c.case_pos + 1;
    if (suf[0] == S_HEH || suf[0] == S_KAF) return true; // -ه/ها/هم/هما/هن / -ك/كم/كما/كن
    if (sl == 2 && suf[0] == S_NOON && suf[1] == S_YEH) return true; // -ني
    if (sl == 2 && suf[0] == S_NOON && suf[1] == S_ALEF && diac_family(c.diac[c.case_pos]) != FAM_SUKUN) return true; // حدَّثَنا
    return false;
}

inline bool is_kana_verb_word(const uint8_t* s, size_t n) {
    if (n < 3) return false;
    size_t st = 0;
    if (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_LAM)) st = 1;
    if (n - st >= 5 && s[st] == S_SEEN) ++st;
    const size_t sn = n - st;
    const uint8_t* w = s + st;
    if (sn == 3 && w[0] == S_KAF && w[1] == S_ALEF && w[2] == S_NOON) return true; // كان
    if (sn == 4 && w[0] == S_KAF && w[1] == S_ALEF && w[2] == S_NOON &&
        (w[3] == S_TEH || w[3] == S_WAW)) return true; // كانت / كانو
    if (sn == 5 && w[0] == S_KAF && w[1] == S_ALEF && w[2] == S_NOON &&
        w[3] == S_WAW && w[4] == S_ALEF) return true; // كانوا
    if (sn == 4 && is_imperf_prefix(w[0]) && w[1] == S_KAF && w[2] == S_WAW && w[3] == S_NOON) {
        return true; // يكون / تكون / نكون / أكون
    }
    return false;
}

inline bool is_afal_maksura_elative(const uint8_t* w, size_t len) {
    if (len != 4 || w[0] != S_ALEF_HAMZA_A || w[3] != S_ALEF_MAKSURA) return false;
    return (w[1] == S_AIN && w[2] == S_LAM) || // أعلى
           (w[1] == S_DAL && w[2] == S_NOON) || // أدنى
           (w[1] == S_QAF && w[2] == S_SAD) ||  // أقصى
           (w[1] == S_SEEN && w[2] == S_MEEM) || // أسمى
           (w[1] == S_WAW && w[2] == S_LAM) ||  // أولى
           (w[1] == S_REH && w[2] == S_QAF) ||  // أرقى
           (w[1] == S_GHAIN && w[2] == S_LAM) || // أغلى
           (w[1] == S_HAH && w[2] == S_LAM);    // أحلى
}

inline bool is_ina_clause_word(const uint8_t* s, size_t n, uint8_t part, uint16_t surf_flags) {
    if (part == PART_INNA || part == PART_AN || (surf_flags & SF_INNA_PRON)) return true;
    return (n >= 3 && (s[0] == S_ALEF_HAMZA_A || s[0] == S_ALEF_HAMZA_B) && s[1] == S_NOON &&
            (n == 3 || s[2] == S_HEH || s[2] == S_KAF || s[2] == S_NOON));
}

inline bool is_day_of_week_word(const uint8_t* s, size_t n, size_t al_len) {
    if (n <= al_len + 2) return false;
    const size_t sn = n - al_len;
    const uint8_t* w = s + al_len;
    if (sn == 3 && (w[0] == S_ALEF_HAMZA_A || w[0] == S_ALEF) && w[1] == S_HAH && w[2] == S_DAL) return true; // أحد
    if (sn == 5 && (w[0] == S_ALEF || w[0] == S_ALEF_HAMZA_B) && w[1] == S_THEH && w[2] == S_NOON && w[3] == S_YEH && w[4] == S_NOON) return true; // اثنين/إثنين
    if (sn == 6 && w[0] == S_THEH && w[1] == S_LAM && w[2] == S_ALEF && w[3] == S_THEH && w[4] == S_ALEF && w[5] == S_HAMZA) return true; // ثلاثاء
    if (sn == 6 && (w[0] == S_ALEF_HAMZA_A || w[0] == S_ALEF) && w[1] == S_REH && w[2] == S_BEH && w[3] == S_AIN && w[4] == S_ALEF && w[5] == S_HAMZA) return true; // أربعاء
    if (sn == 4 && w[0] == S_KHAH && w[1] == S_MEEM && w[2] == S_YEH && w[3] == S_SEEN) return true; // خميس
    if (sn == 4 && w[0] == S_JEEM && w[1] == S_MEEM && w[2] == S_AIN && w[3] == S_TEH_MARBUTA) return true; // جمعة
    if (sn == 3 && w[0] == S_SEEN && w[1] == S_BEH && w[2] == S_TEH) return true; // سبت
    return false;
}

inline bool is_dem_det_word(const uint8_t* s, size_t n) {
    size_t st = 0;
    if (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM || s[0] == S_KAF)) st = 1;
    if (n - st >= 4 && (s[0] == S_WAW || s[0] == S_FEH) && (s[1] == S_BEH || s[1] == S_LAM || s[1] == S_KAF)) st = 2;
    const size_t sn = n - st;
    const uint8_t* w = s + st;
    // Exclude adverbs هنا / هناك / كذلك
    if (sn == 3 && w[0] == S_HEH && w[1] == S_NOON && w[2] == S_ALEF) return false;
    if (sn == 4 && w[0] == S_HEH && w[1] == S_NOON && w[2] == S_ALEF && w[3] == S_KAF) return false;
    if (sn == 4 && w[0] == S_KAF && w[1] == S_THAL && w[2] == S_LAM && w[3] == S_KAF) return false;
    return true;
}

inline bool is_decade_num_word(const uint8_t* s, size_t n) {
    if (n < 5) return false;
    size_t st = 0;
    if (n >= 6 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM)) st = 1;
    const size_t sn = n - st;
    const uint8_t* w = s + st;
    if (sn < 5 || w[sn - 1] != S_NOON || (w[sn - 2] != S_WAW && w[sn - 2] != S_YEH)) return false;
    const size_t stem = sn - 2;
    if (stem == 3 && w[0] == S_AIN && w[1] == S_SHEEN && w[2] == S_REH) return true; // عشرون/ين
    if (stem == 4 && w[0] == S_THEH && w[1] == S_LAM && w[2] == S_ALEF && w[3] == S_THEH) return true; // ثلاثون/ين
    if (stem == 4 && (w[0] == S_ALEF_HAMZA_A || w[0] == S_ALEF) && w[1] == S_REH && w[2] == S_BEH && w[3] == S_AIN) return true; // أربعون/ين
    if (stem == 3 && w[0] == S_KHAH && w[1] == S_MEEM && w[2] == S_SEEN) return true; // خمسون/ين
    if (stem == 2 && w[0] == S_SEEN && w[1] == S_TEH) return true; // ستون/ين
    if (stem == 3 && w[0] == S_SEEN && w[1] == S_BEH && w[2] == S_AIN) return true; // سبعون/ين
    if (stem == 4 && w[0] == S_THEH && w[1] == S_MEEM && w[2] == S_ALEF && w[3] == S_NOON) return true; // ثمانون/ين
    if (stem == 3 && w[0] == S_TEH && w[1] == S_SEEN && w[2] == S_AIN) return true; // تسعون/ين
    return false;
}

inline bool is_innama_word(const uint8_t* s, size_t n) {
    size_t st = (n >= 5 && (s[0] == S_WAW || s[0] == S_FEH)) ? 1 : 0;
    const size_t sn = n - st;
    const uint8_t* w = s + st;
    if (sn == 4 && (w[0] == S_ALEF_HAMZA_B || w[0] == S_ALEF_HAMZA_A || w[0] == S_ALEF) &&
        w[1] == S_NOON && w[2] == S_MEEM && w[3] == S_ALEF) return true; // إنما / أنما
    if (sn == 5 && w[0] == S_KAF && (w[1] == S_ALEF_HAMZA_A || w[1] == S_ALEF) &&
        w[2] == S_NOON && w[3] == S_MEEM && w[4] == S_ALEF) return true; // كأنما
    return false;
}

inline bool is_yeh_noun_stem(const uint8_t* s, size_t n, size_t st) {
    if (n <= st + 2 || s[st] != S_YEH) return false;
    const size_t sn = n - st;
    const uint8_t* w = s + st;
    if (is_month_word(w, sn)) return true; // يناير/يونيو/يوليو
    if (sn == 3 && w[1] == S_WAW && w[2] == S_MEEM) return true; // يوم
    if (sn == 3 && w[1] == S_MEEM && w[2] == S_NOON) return true; // يمن
    if (sn == 4 && w[1] == S_MEEM && w[2] == S_YEH && w[3] == S_NOON) return true; // يمين
    if (sn == 4 && w[1] == S_SEEN && w[2] == S_ALEF && w[3] == S_REH) return true; // يسار
    if (sn == 4 && w[1] == S_QAF && w[2] == S_YEH && w[3] == S_NOON) return true; // يقين
    if (sn == 4 && w[1] == S_TEH && w[2] == S_YEH && w[3] == S_MEEM) return true; // يتيم
    if (sn == 4 && w[1] == S_HEH && w[2] == S_WAW && w[3] == S_DAL) return true; // يهود
    return false;
}

inline bool is_alef_ibn_word(const uint8_t* s, size_t n) {
    size_t st = (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH)) ? 1 : 0;
    const size_t len = n - st;
    const uint8_t* w = s + st;
    return len == 3 && (w[0] == S_ALEF || w[0] == S_ALEF_WASLA) && w[1] == S_BEH && w[2] == S_NOON;
}

template <typename CandT>
inline uint8_t eff_case_fam(const uint8_t* s, const CandT& c, bool* out_oblique = nullptr) {
    if (out_oblique) *out_oblique = false;
    if (c.case_pos >= 2) {
        const uint8_t cd = c.diac[c.case_pos];
        if (c.ext_flags & EF_MASC_PL_NOON) {
            uint8_t prev = s[c.case_pos - 1];
            if (prev == S_WAW && cd == DIAC_FATHA) return FAM_DAMMA;
            if (prev == S_YEH && cd == DIAC_FATHA) {
                if (out_oblique) *out_oblique = true;
                return FAM_KASRA;
            }
        }
        if (c.ext_flags & EF_DUAL_NOON) {
            uint8_t prev = s[c.case_pos - 1];
            if (prev == S_ALEF && cd == DIAC_KASRA) return FAM_DAMMA;
            if (prev == S_YEH && cd == DIAC_KASRA) {
                if (out_oblique) *out_oblique = true;
                return FAM_KASRA;
            }
        }
    }
    return diac_family(c.diac[c.case_pos]);
}

enum FrameKind : uint8_t {
    FRM_NOM = 0,
    FRM_VERB_ACT = 1,
    FRM_VERB_PASS = 2,
    FRM_VERB_HAVE = 3,
    FRM_INNA = 4,
    FRM_KANA = 5,
    FRM_REL = 6
};

struct SynState {
    uint8_t kind[4] = {FRM_NOM, 0, 0, 0};
    uint8_t stage[4] = {0, 0, 0, 0};
    uint8_t gov_vtrans[4] = {0, 0, 0, 0};
    int8_t gov_vodds[4] = {0, 0, 0, 0};
    uint8_t depth = 0;
    uint8_t pp_state = 0;       // 0: none, 1: right after prep, 2: inside PP noun/idafa/adj tail
    uint8_t idafa_state = 0;    // 0: none, 1: preceded by construct head
    uint8_t mudaf_head_fam = FAM_NONE;
    uint8_t prev_np_fam = FAM_NONE;
    uint8_t prev_np_al = 0;
    uint8_t prev_np_tan = 0;
    uint8_t prev_np_adj = 0;
    uint8_t prev_np_suf = 0;
    uint8_t prev_np_pp = 0;
    uint8_t head_np_fam = FAM_NONE;
    uint8_t head_np_al = 0;
    uint8_t head_np_pp = 0;
    uint8_t mood_expect = FAM_NONE;
    uint8_t conj_pending = 0;
    uint8_t last_was_title = 0;
    uint8_t last_role = 0;
    uint8_t last_end = DIAC_NONE;
    uint8_t prev2_st3 = 0;
    uint8_t prev1_st3 = 0;
};

struct BeamHyp {
    SynState st;
    int32_t score = 0;
    uint32_t prev_node = UINT32_MAX;
    uint32_t cand_k = 0;
};

struct BeamNode {
    uint32_t prev_node = UINT32_MAX;
    uint32_t cand_k = 0;
};

} // namespace

Diacritizer::Diacritizer() {
    for (uint32_t i = 0; i < NUM_WEIGHTS; ++i) weights_[i] = DEFAULT_WEIGHTS[i];
    for (uint32_t i = 0; i < NUM_V7_WEIGHTS; ++i) v7_weights_[i] = DEFAULT_V7_WEIGHTS[i];
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
    owned_buf_.clear();
    owned_buf_.shrink_to_fit();
    is_v3_ = false;
    is_v5_ = false;
    is_v7_ = false;
    has_v5_24roles_ = false;
    vocab_count_ = 0;
    fst_nodes_count_ = 0;
    word_meta_ = nullptr;
    word_meta_v3_ = nullptr;
    candidates_ = nullptr;
    candidates_v3_ = nullptr;
    candidates_v5_ = nullptr;
    core_pool_ = nullptr;
    valency_ = ValencyTableV7();
    trans3_ = nullptr;
    trans3_size_ = 0;
    trans_matrix_ = nullptr;
    trans_matrix_size_ = 0;
    oov_feat_count_ = 16;
    irab_feat_count_ = 40;
    irab2_feat_count_ = IRAB2_NUM_FEATURES;
    fst_ = BitmappedFST();
    bigrams_ = BigramTable();
    ngrams_ = NgramTableV6();
    core_bg_ = CoreBgTableV7();
    oov_trees_ = FlatTreeEnsemble();
    irab_trees_ = FlatTreeEnsemble();
    irab2_trees_ = FlatTreeEnsemble();
    morph_trees_ = FlatTreeEnsemble();
    ++cache_epoch_;
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
    mapped_ptr_ = ptr;
    mapped_size_ = fsize;
    const auto* base = static_cast<const uint8_t*>(ptr);
    if (fsize >= sizeof(CompressedFileHeader) &&
        reinterpret_cast<const CompressedFileHeader*>(base)->magic == TASH_COMP_MAGIC) {
        const auto* ch = reinterpret_cast<const CompressedFileHeader*>(base);
        if (ch->method != TASH_COMP_DEFLATE ||
            ch->uncompressed_size < sizeof(FileHeader) ||
            ch->uncompressed_size > (512u << 20)) {
            unload();
            if (err_msg) *err_msg = "Invalid compressed model header";
            return false;
        }
        std::vector<uint8_t> decomp(ch->uncompressed_size);
        size_t written = 0;
        bool ok = tiny_inflate_raw(
            base + sizeof(CompressedFileHeader),
            fsize - sizeof(CompressedFileHeader),
            decomp.data(), decomp.size(), &written);
        if (!ok || written != ch->uncompressed_size ||
            crc32_ieee(decomp.data(), decomp.size()) != ch->crc32) {
            unload();
            if (err_msg) *err_msg = "Failed to decompress or verify model checksum";
            return false;
        }
        unload();
        owned_buf_ = std::move(decomp);
        base = owned_buf_.data();
        fsize = owned_buf_.size();
    }
    const auto* hdr2 = reinterpret_cast<const FileHeader*>(base);
    if (hdr2->magic != TASH_MAGIC ||
        (hdr2->version != TASH_VERSION_2 && hdr2->version != TASH_VERSION_3 &&
         hdr2->version != TASH_VERSION_4 && hdr2->version != TASH_VERSION_5 &&
         hdr2->version != TASH_VERSION_6 && hdr2->version != TASH_VERSION_7)) {
        unload();
        if (err_msg) *err_msg = "Bad magic or unsupported model version";
        return false;
    }
    auto in_range = [fsize](uint64_t off, uint64_t bytes) { return off <= fsize && bytes <= fsize - off; };

    if (hdr2->version == TASH_VERSION_5 || hdr2->version == TASH_VERSION_6 || hdr2->version == TASH_VERSION_7) {
        const size_t min_v56 = (hdr2->version == TASH_VERSION_7) ? sizeof(FileHeaderV7)
                             : ((hdr2->version == TASH_VERSION_6) ? sizeof(FileHeaderV6) : sizeof(FileHeaderV5));
        if (fsize < min_v56) {
            unload();
            if (err_msg) *err_msg = "Truncated header";
            return false;
        }
        const auto* hdr5 = reinterpret_cast<const FileHeaderV5*>(base);
        const auto* hdr6 = (hdr2->version >= TASH_VERSION_6) ? reinterpret_cast<const FileHeaderV6*>(base) : nullptr;
        const auto* hdr7 = (hdr2->version == TASH_VERSION_7) ? reinterpret_cast<const FileHeaderV7*>(base) : nullptr;
        const bool cand5 = (hdr5->flags & 2u) != 0;
        const bool bg_int8 = (hdr5->flags & 4u) != 0;
        const uint8_t tree_mode = (hdr5->flags & 1u) ? 2 : 1;
        const uint32_t v7f = hdr7 ? hdr7->v7_flags : 0u;
        const bool compact_ng = (v7f & V7F_COMPACT_NGRAM) != 0;
        const bool compact_cb = (v7f & V7F_COMPACT_CORE_BG) != 0;
        const bool compact_val = (v7f & V7F_COMPACT_VALENCY) != 0;
        const uint64_t ng_bytes = hdr6 ? (compact_ng ? uint64_t(hdr6->ngram_capacity) : uint64_t(hdr6->ngram_capacity) * sizeof(NgramEntryV6)) : 0;
        const uint64_t val_bytes = hdr7 ? (compact_val ? uint64_t(hdr7->valency_count) : uint64_t(hdr7->valency_count) * sizeof(uint32_t)) : 0;
        const uint64_t cb_bytes = hdr7 ? (compact_cb ? uint64_t(hdr7->core_bg_capacity) : uint64_t(hdr7->core_bg_capacity) * sizeof(CoreBgEntryV7)) : 0;
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
            !in_range(hdr5->weights_offset, uint64_t(hdr5->weights_count) * sizeof(int32_t)) ||
            (hdr6 && hdr6->ngram_capacity > 0 && !in_range(hdr6->ngram_entries_offset, ng_bytes)) ||
            (hdr7 && hdr7->valency_count > 0 && !in_range(hdr7->valency_offset, val_bytes)) ||
            (hdr7 && hdr7->trans3_size > 0 && !in_range(hdr7->trans3_offset, hdr7->trans3_size)) ||
            (hdr7 && hdr7->irab2_trees_size > 0 && !in_range(hdr7->irab2_trees_offset, hdr7->irab2_trees_size)) ||
            (hdr7 && hdr7->core_bg_capacity > 0 && !in_range(hdr7->core_bg_offset, cb_bytes)) ||
            (hdr7 && hdr7->v7_weights_count > 0 && !in_range(hdr7->v7_weights_offset, uint64_t(hdr7->v7_weights_count) * sizeof(int32_t)))) {
            unload();
            if (err_msg) *err_msg = "Model sections exceed file bounds";
            return false;
        }
        is_v5_ = true;
        is_v7_ = (hdr7 != nullptr);
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
        if (hdr6 && hdr6->ngram_capacity > 0) {
            if (compact_ng) {
                if (!ngrams_.init_compact(base + hdr6->ngram_entries_offset, hdr6->ngram_capacity)) {
                    unload();
                    if (err_msg) *err_msg = "Invalid compact V7 ngram blob";
                    return false;
                }
            } else {
                ngrams_.init(reinterpret_cast<const NgramEntryV6*>(base + hdr6->ngram_entries_offset), hdr6->ngram_capacity);
            }
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
        for (uint32_t i = 0; i < NUM_V7_WEIGHTS; ++i) v7_weights_[i] = DEFAULT_V7_WEIGHTS[i];
        if (hdr7) {
            if (hdr7->valency_count > 0) {
                if (compact_val) {
                    if (!valency_.init_compact(base + hdr7->valency_offset, hdr7->valency_count)) {
                        unload();
                        if (err_msg) *err_msg = "Invalid compact V7 valency blob";
                        return false;
                    }
                } else {
                    valency_.init_raw(reinterpret_cast<const uint32_t*>(base + hdr7->valency_offset), hdr7->valency_count);
                }
            }
            if (hdr7->trans3_size > 0) {
                trans3_ = reinterpret_cast<const int8_t*>(base + hdr7->trans3_offset);
                trans3_size_ = hdr7->trans3_size;
            }
            if (hdr7->irab2_trees_size > 0) {
                uint8_t irab2_mode = (hdr7->v7_flags & 1u) ? 2 : ((hdr7->v7_flags & 2u) ? 1 : 0);
                irab2_trees_.init(base + hdr7->irab2_trees_offset, hdr7->irab2_trees_size, irab2_mode);
                irab2_feat_count_ = static_cast<uint8_t>(hdr7->irab2_feat_count ? hdr7->irab2_feat_count : IRAB2_NUM_FEATURES);
            }
            if (hdr7->core_bg_capacity > 0) {
                if (compact_cb) {
                    if (!core_bg_.init_compact(base + hdr7->core_bg_offset, hdr7->core_bg_capacity)) {
                        unload();
                        if (err_msg) *err_msg = "Invalid compact V7 core_bg blob";
                        return false;
                    }
                } else {
                    core_bg_.init(reinterpret_cast<const CoreBgEntryV7*>(base + hdr7->core_bg_offset), hdr7->core_bg_capacity);
                }
            }
            if (hdr7->v7_weights_count > 0) {
                const auto* w7 = reinterpret_cast<const int32_t*>(base + hdr7->v7_weights_offset);
                for (uint32_t i = 0; i < hdr7->v7_weights_count && i < NUM_V7_WEIGHTS; ++i) v7_weights_[i] = w7[i];
            }
        }
        (void)particle_map();
        return true;
    }

    if (hdr2->version == TASH_VERSION_3 || hdr2->version == TASH_VERSION_4) {
        const size_t min_hdr = hdr2->version == TASH_VERSION_4 ? sizeof(FileHeaderV4) : sizeof(FileHeaderV3);
        if (fsize < min_hdr) {
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
            unload();
            if (err_msg) *err_msg = "v3/v4 model sections exceed file bounds";
            return false;
        }
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
        unload();
        if (err_msg) *err_msg = "Model sections exceed file bounds";
        return false;
    }
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
    if (is_ali_proper_cand(tok, c)) return 23;
    if (tok.surf_flags & SF_INNA_PRON) return 4;
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

uint8_t Diacritizer::cand_state3_v7(const Token& tok, const Cand& c, const uint8_t* ts) const {
    const uint8_t fam = eff_case_fam(ts, c);
    const uint8_t cd = c.diac[c.case_pos];
    const bool c_al = (c.flags & CF_HAS_AL) || tok.si.al_len > 0;
    const bool c_gen = (c.flags & CF_GEN) || tok.si.has_prep;
    const bool is_kana = (tok.si.particle == PART_KANA) || is_kana_verb_word(ts, tok.n);
    uint8_t sr = 11;
    if (tok.si.particle == PART_GEN || c_gen) {
        sr = 1;
    } else if (is_kana) {
        sr = 7;
    } else if (tok.si.particle == PART_INNA ||
               ((tok.si.particle == PART_AN || tok.si.particle == PART_LI_AN ||
                 tok.si.particle == PART_LAKIN || tok.si.particle == PART_IN) && diac_has_shadda(cd)) ||
               (tok.surf_flags & SF_INNA_PRON)) {
        sr = 6;
    } else if (c.verb_pattern == 1) {
        if (c.pos_tag == POS_VERB_PASS) {
            sr = 4;
        } else if (c.pos_tag == POS_VERB_PAST_SUBJ || c.pos_tag == POS_VERB_IMPERF_12 ||
                   has_verb_obj_suffix(ts, tok.n, c)) {
            sr = 5;
        } else {
            ValencyInfo vi = get_valency(tok.word_id);
            sr = (vi.v_trans == 1) ? 2 : 3;
        }
    } else if (tok.si.particle == PART_IDAFA_NOUN || tok.si.particle == PART_NUM_IDAFA) {
        sr = 10;
    } else if (tok.si.particle == PART_NONE || is_ali_proper_cand(tok, c)) {
        if (c_al || (c.flags & CF_SUFFIX)) sr = 8;
        else if (diac_is_tanween(cd)) sr = 9;
        else sr = 10;
    } else {
        sr = 11;
    }
    return static_cast<uint8_t>(sr * NUM_TRANS3_FAMS + (fam < NUM_TRANS3_FAMS ? fam : 0));
}

bool Diacritizer::word_has_multi_core(uint32_t wid, const uint8_t* syms, size_t len) const {
    if (wid >= vocab_count_ || len == 0 || len > MAX_WORD_LEN) return false;
    SurfaceInfo si = analyze_surface(syms, len);
    if (si.particle == PART_IN || si.particle == PART_AN || si.particle == PART_LI_AN ||
        si.particle == PART_LAKIN || si.particle == PART_AY || si.particle == PART_LAMA ||
        is_man_word(syms, len) || is_ali_word(syms, len) || is_am_word(syms, len) || is_themma_word(syms, len)) {
        return true;
    }
    uint32_t c_start = meta_cand_start(wid);
    uint16_t c_cnt = meta_cand_count(wid);
    if (c_cnt < 2) return false;
    uint8_t lam_skip = (si.al_len >= 2) ? si.al_lam_pos : 255;
    uint16_t first_h = 0;
    uint8_t diacs[MAX_WORD_LEN];
    for (uint16_t k = 0; k < c_cnt; ++k) {
        WordCandidate wc = read_cand(c_start + k);
        for (size_t p = 0; p + 1 < len; ++p) diacs[p] = read_core_diac(wc.core_offset, p);
        diacs[len - 1] = wc.ending_id;
        Cand tmp{};
        std::memcpy(tmp.diac, diacs, len);
        tmp.case_pos = static_cast<uint8_t>(len - 1);
        tmp.flags = wc.flags;
        finalize_cand(tmp, syms, len, si, 0);
        uint16_t h = compute_core_hash(syms, tmp.diac, tmp.case_pos, lam_skip);
        if (k == 0) first_h = h;
        else if (h != first_h) return true;
    }
    return false;
}

void Diacritizer::extract_word_top_info(uint32_t wid, const uint8_t* syms, size_t len,
                                        uint8_t& out_case_pos, uint8_t& out_top_end, uint8_t& out_top_verb) const {
    out_case_pos = static_cast<uint8_t>(len > 0 ? len - 1 : 0);
    out_top_end = 255;
    out_top_verb = 0;
    if (wid >= vocab_count_ || len == 0 || len > MAX_WORD_LEN) return;
    uint16_t c_cnt = meta_cand_count(wid);
    if (c_cnt == 0) return;
    SurfaceInfo si = analyze_surface(syms, len);
    WordCandidate wc = read_cand(meta_cand_start(wid));
    Cand tmp{};
    for (size_t p = 0; p + 1 < len; ++p) tmp.diac[p] = read_core_diac(wc.core_offset, p);
    tmp.diac[len - 1] = wc.ending_id;
    tmp.case_pos = static_cast<uint8_t>(len - 1);
    tmp.flags = wc.flags;
    finalize_cand(tmp, syms, len, si, 0);
    out_case_pos = tmp.case_pos;
    out_top_end = tmp.diac[tmp.case_pos];
    out_top_verb = tmp.verb_pattern;
}

void Diacritizer::extract_word_core_priors(uint32_t wid, const uint8_t* syms, size_t len,
                                           uint16_t out_hashes[4], float out_probs[4], int& out_cnt) const {
    out_cnt = 0;
    for (int i = 0; i < 4; ++i) { out_hashes[i] = 0; out_probs[i] = 0.0f; }
    if (wid >= vocab_count_ || len == 0 || len > MAX_WORD_LEN) return;
    uint32_t c_start = meta_cand_start(wid);
    uint16_t c_cnt = meta_cand_count(wid);
    if (c_cnt == 0) return;
    SurfaceInfo si = analyze_surface(syms, len);
    uint8_t lam_skip = (si.al_len >= 2) ? si.al_lam_pos : 255;
    uint16_t hs[16];
    double ws[16];
    int n_u = 0;
    double tot_w = 0.0;
    uint8_t diacs[MAX_WORD_LEN];
    for (uint16_t k = 0; k < c_cnt; ++k) {
        WordCandidate wc = read_cand(c_start + k);
        for (size_t p = 0; p + 1 < len; ++p) diacs[p] = read_core_diac(wc.core_offset, p);
        diacs[len - 1] = wc.ending_id;
        Cand tmp{};
        std::memcpy(tmp.diac, diacs, len);
        tmp.case_pos = static_cast<uint8_t>(len - 1);
        tmp.flags = wc.flags;
        finalize_cand(tmp, syms, len, si, 0);
        uint16_t h = compute_core_hash(syms, tmp.diac, tmp.case_pos, lam_skip);
        double w = std::exp(static_cast<double>(wc.log_prob) / 1000.0);
        tot_w += w;
        int f = -1;
        for (int j = 0; j < n_u; ++j) if (hs[j] == h) { f = j; break; }
        if (f >= 0) ws[f] += w;
        else if (n_u < 16) { hs[n_u] = h; ws[n_u] = w; ++n_u; }
    }
    if (tot_w <= 0.0) return;
    int ord[16];
    for (int j = 0; j < n_u; ++j) ord[j] = j;
    std::sort(ord, ord + n_u, [&](int a, int b) { return ws[a] > ws[b]; });
    out_cnt = std::min(4, n_u);
    for (int j = 0; j < out_cnt; ++j) {
        out_hashes[j] = hs[ord[j]];
        out_probs[j] = static_cast<float>(ws[ord[j]] / tot_w);
    }
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
        size_t ast = (n == 5 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM)) ? 1 : 0;
        if (n - ast == 4 && s[ast] == S_AIN && s[ast + 1] == S_MEEM && s[ast + 2] == S_REH && s[ast + 3] == S_WAW &&
            d[ast + 3] == DIAC_NONE) {
            c.case_pos = static_cast<uint8_t>(ast + 2); // عَمْرو: الإعراب على الراء
        }

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
    if (n >= 5 && !(c.flags & CF_SUFFIX) && s[n - 1] == S_NOON && d[n - 2] == DIAC_NONE && d[n - 1] == DIAC_FATHA &&
        ((s[n - 2] == S_WAW && diac_family(d[n - 3]) == FAM_DAMMA) ||
         (s[n - 2] == S_YEH && diac_family(d[n - 3]) == FAM_KASRA))) {
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
    } else if (!(c.flags & CF_HAS_AL) && (c.flags & CF_LAM_NASB) && st < sn && s[st] == S_LAM &&
               sn - st >= 5 && is_imperf_prefix(s[st + 1]) && d[st] == DIAC_KASRA) {
        ++st;
    }

    auto set_pos_and_mc = [&](bool is_imp_verb) {
        if (si.particle != PART_NONE && si.particle != PART_KANA && si.particle != PART_IDAFA_NOUN && si.particle != PART_NUM_IDAFA) {
            c.pos_tag = POS_PARTICLE;
            bool sh_any = false;
            for (size_t k = 0; k < n; ++k) if (diac_has_shadda(d[k])) { sh_any = true; break; }
            c.morph_class = sh_any ? MC_PART_SHADDA : MC_PART_SUKUN_LIGHT;
            return;
        }
        if (c.verb_pattern == 1) {
            bool is_pass = (c.ext_flags & EF_PASSIVE) != 0;
            if (is_imp_verb) {
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
        set_pos_and_mc(false);
        return;
    }
    if (si.particle != PART_NONE && si.particle != PART_KANA) {
        if (is_diptote_stem(s, sn, st, d)) c.ext_flags |= EF_DIPTOTE;
        set_pos_and_mc(false);
        return;
    }
    if (sn < 3 || diac_is_tanween(d[c.case_pos]) || s[sn - 1] == S_TEH_MARBUTA) {
        if (is_diptote_stem(s, sn, st, d)) c.ext_flags |= EF_DIPTOTE;
        set_pos_and_mc(false);
        return;
    }

    const size_t slen = sn - st;
    if (slen < 3) {
        set_pos_and_mc(false);
        return;
    }

    uint8_t mood_d = d[sn - 1];
    uint8_t mood_fam = diac_family(mood_d);
    bool valid_imperf_mood = (mood_fam == FAM_DAMMA || mood_fam == FAM_FATHA || mood_fam == FAM_SUKUN) ||
                             (mood_d == DIAC_NONE && (s[sn - 1] == S_YEH || s[sn - 1] == S_WAW || s[sn - 1] == S_ALEF_MAKSURA));
    const bool is_proper_dip_any = (classify_diptote_surface(s, sn, st) == 2);
    if (is_afal_maksura_elative(s + st, slen) && d[st] == DIAC_FATHA && d[st + 1] == DIAC_SUKUN) {
        c.verb_pattern = 2;
        set_pos_and_mc(false);
        return;
    }
    const bool is_afal_elative_noun = (!is_proper_dip_any && slen == 4 && !(c.flags & CF_SUFFIX) &&
                                       s[st] == S_ALEF_HAMZA_A && d[st] == DIAC_FATHA && d[st + 1] == DIAC_SUKUN &&
                                       s[sn - 1] != S_ALEF_MAKSURA && (has_noun_lex || is_diptote_stem(s, sn, st, d)));
    if (is_afal_elative_noun) {
        c.verb_pattern = 2;
        c.ext_flags |= EF_DIPTOTE;
        set_pos_and_mc(false);
        return;
    }
    const bool is_form4_past_3sg = (s[st] == S_ALEF_HAMZA_A && d[st] == DIAC_FATHA && d[st + 1] == DIAC_SUKUN &&
                                    ((slen == 4 && d[st + 2] == DIAC_FATHA &&
                                      (d[sn - 1] == DIAC_FATHA || s[sn - 1] == S_ALEF_MAKSURA)) ||
                                     (slen == 5 && s[sn - 1] == S_TEH && d[sn - 2] == DIAC_FATHA && d[sn - 1] == DIAC_SUKUN)));
    const bool is_yeh_nonverb_noun = (s[st] == S_YEH && ((slen == 3 && d[st + 1] == DIAC_SUKUN) || is_yeh_noun_stem(s, sn, st)));
    if (!is_proper_dip_any && !is_yeh_nonverb_noun && is_imperf_prefix(s[st]) && valid_imperf_mood && !is_form4_past_3sg) {
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
            set_pos_and_mc(true);
            return;
        }
        if (slen == 4 && !has_noun_lex &&
            ((s[st + 2] == S_WAW && d[st + 1] == DIAC_DAMMA && d[st + 2] == DIAC_NONE) ||
             (s[st + 2] == S_YEH && d[st + 1] == DIAC_KASRA && d[st + 2] == DIAC_NONE) ||
             (s[st + 2] == S_ALEF && d[st + 1] == DIAC_FATHA && d[st + 2] == DIAC_NONE))) {
            // Hollow imperfect verbs (يَعُودُ، تَعُودَ، تَكُونَ، تَسِيرُ، يَزِيدُ، نَقُولُ)
            c.verb_pattern = 1;
            set_pos_and_mc(true);
            return;
        }
        if ((s[st] == S_TEH || s[st] == S_NOON) && d[st] == DIAC_FATHA && slen == 3 &&
            (s[sn - 1] == S_YEH || s[sn - 1] == S_ALEF_MAKSURA)) {
            c.verb_pattern = 1;
            set_pos_and_mc(true);
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
        set_pos_and_mc(true);
        return;
    }

    if (slen == 3 && (d[st + 2] == DIAC_FATHA || d[st + 2] == DIAC_SHADDA_FATHA) &&
        ((d[st] == DIAC_FATHA && (d[st + 1] == DIAC_FATHA || d[st + 1] == DIAC_KASRA || d[st + 1] == DIAC_DAMMA ||
                                  d[st + 1] == DIAC_SHADDA_FATHA || d[st + 1] == DIAC_SHADDA_KASRA || d[st + 1] == DIAC_NONE)) ||
         (d[st] == DIAC_DAMMA && (d[st + 1] == DIAC_KASRA || d[st + 1] == DIAC_SHADDA_KASRA ||
                                  (s[st + 1] == S_YEH && d[st + 1] == DIAC_NONE))))) {
        if (d[st] == DIAC_FATHA && d[st + 1] == DIAC_FATHA && d[st + 2] == DIAC_FATHA && has_noun_lex) {
            c.verb_pattern = 2;
        } else {
            c.verb_pattern = 1;
        }
        if (d[st] == DIAC_DAMMA && (d[st + 1] == DIAC_KASRA || d[st + 1] == DIAC_SHADDA_KASRA ||
                                    (s[st + 1] == S_YEH && d[st + 1] == DIAC_NONE))) {
            c.ext_flags |= EF_PASSIVE;
        }
    } else if (slen == 3 && !si.has_prep && (!has_noun_lex || ((c.flags & CF_SUFFIX) && s[st + 2] == S_ALEF)) &&
               !is_five_noun_head(s, sn) && d[st] == DIAC_FATHA && d[st + 1] == DIAC_FATHA &&
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
                             ((s[sn - 1] == S_ALEF_MAKSURA || ((c.flags & CF_SUFFIX) && s[sn - 1] == S_ALEF)) && d[sn - 2] == DIAC_FATHA)) &&
               (s[st] == S_ALEF || s[st] == S_TEH || s[st] == S_ALEF_HAMZA_A) &&
               (d[sn - 2] == DIAC_FATHA || d[sn - 2] == DIAC_SHADDA_FATHA || s[sn - 2] == S_ALEF ||
                (d[st] == DIAC_DAMMA && d[sn - 2] == DIAC_KASRA))) {
        c.verb_pattern = 1;
        if (d[st] == DIAC_DAMMA && d[sn - 2] == DIAC_KASRA) c.ext_flags |= EF_PASSIVE;
    } else if ((slen >= 4 || (slen == 3 && s[st + 1] != S_WAW && s[st + 1] != S_YEH && s[st] != S_ALEF_HAMZA_A &&
                              (!has_noun_lex || (d[sn - 2] == DIAC_SUKUN && (d[st] == DIAC_DAMMA || d[st] == DIAC_KASRA) &&
                                                 (d[sn - 1] == DIAC_DAMMA || d[sn - 1] == DIAC_FATHA || d[sn - 1] == DIAC_KASRA))))) &&
               s[sn - 1] == S_TEH &&
               (d[sn - 1] == DIAC_SUKUN || d[sn - 1] == DIAC_KASRA || d[sn - 1] == DIAC_DAMMA || d[sn - 1] == DIAC_FATHA) &&
               (d[st] == DIAC_FATHA || d[st] == DIAC_DAMMA ||
                (slen >= 5 && is_alef_or_wasla(s[st]) && (d[st] == DIAC_NONE || d[st] == DIAC_KASRA)) ||
                (slen == 3 && d[st] == DIAC_KASRA && d[sn - 2] == DIAC_SUKUN))) {
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
    } else if (slen >= 4 && s[sn - 1] == S_ALEF && s[sn - 2] == S_WAW &&
               (d[sn - 3] == DIAC_DAMMA || (d[sn - 3] == DIAC_FATHA && d[sn - 2] == DIAC_SUKUN)) &&
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
    set_pos_and_mc(false);
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
                    if (is_alef_or_wasla(s[sun_pos]) && (s[sun_pos] == S_ALEF || s[sun_pos] == S_ALEF_WASLA) && n - sun_pos >= 3) {
                        c.diac[lam_pos] = DIAC_KASRA;
                    } else if (c.diac[lam_pos] != DIAC_NONE && c.diac[lam_pos] != DIAC_SUKUN) {
                        c.diac[lam_pos] = DIAC_NONE;
                    }
                }
            }
        }
        if ((n == 2 && s[0] == S_LAM && s[1] == S_ALEF) ||
            ((tok.si.particle == PART_LA || tok.si.particle == PART_ILLA) && n >= 3 && s[n - 2] == S_LAM && s[n - 1] == S_ALEF)) {
            if (c.diac[n - 1] == DIAC_FATHA && (c.diac[n - 2] == DIAC_NONE || c.diac[n - 2] == DIAC_SHADDA)) {
                c.diac[n - 2] = diac_has_shadda(c.diac[n - 2]) ? DIAC_SHADDA_FATHA : DIAC_FATHA;
                c.diac[n - 1] = DIAC_NONE;
            }
        }
        if (is_ila_yeh_word(s, n)) {
            size_t st = (n == 4) ? 1 : 0;
            c.diac[st] = DIAC_KASRA;
            c.diac[st + 1] = DIAC_FATHA;
            c.diac[st + 2] = DIAC_SHADDA_FATHA;
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
                if ((s[si.al_len] == S_ALEF || s[si.al_len] == S_ALEF_WASLA) && n - si.al_len >= 3) {
                    m = BIT(DIAC_KASRA);
                } else {
                    m = BIT(DIAC_NONE) | BIT(DIAC_SUKUN);
                }
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
        if (cm & BIT(d)) ranked[rc++] = {case_scores[d], d};
    }
    if (n >= 4 && s[n - 1] == S_WAW && cpos == n - 1 &&
        (base.diac[n - 2] == DIAC_DAMMA || (base.diac[n - 2] != DIAC_SUKUN && base.diac[n - 2] != DIAC_FATHA))) {
        ranked[0] = {case_scores[DIAC_NONE], DIAC_NONE};
        rc = 1;
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

    const bool declinable_part = (tok.si.particle == PART_IDAFA_NOUN || tok.si.particle == PART_NUM_IDAFA ||
                                  tok.si.particle == PART_AY || ((tok.surf_flags & SF_ALI) && !(tok.surf_flags & SF_ALA)));
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
            if (n >= 5 && !(src.flags & CF_SUFFIX) && cp == n - 1 && s[n - 1] == S_TEH &&
                !is_imperf_prefix(s[0]) && src.diac[n - 2] == DIAC_SUKUN &&
                (src.diac[n - 1] == DIAC_DAMMA || src.diac[n - 1] == DIAC_FATHA || src.diac[n - 1] == DIAC_KASRA)) {
                Cand c = src;
                c.diac[n - 2] = DIAC_FATHA;
                c.diac[n - 1] = DIAC_SUKUN;
                c.verb_mood_diac = DIAC_SUKUN;
                c.ext_flags |= EF_PAST_TEH_FEM;
                c.pos_tag = POS_VERB_PAST_FEM;
                c.flags |= CF_EXPANDED;
                c.emis = src.emis - weights_[W_EXPAND_PEN] / 2;
                bool dup = false;
                for (uint32_t j = 0; j < tok.cand_count; ++j) {
                    if (std::memcmp(ctx.cands[tok.cand_begin + j].diac, c.diac, n) == 0) { dup = true; break; }
                }
                if (!dup && tok.cand_count < MAX_CANDS + 4) {
                    ctx.cands.push_back(c);
                    ++tok.cand_count;
                }
            }
            continue;
        }

        if (cp + 1 < n && !(src.flags & CF_SUFFIX)) continue;
        if (src.ext_flags & (EF_DUAL_NOON | EF_MASC_PL_NOON)) continue;
        if ((tok.surf_flags & SF_ALI) && src.diac[n - 2] != DIAC_KASRA) continue;
        if (is_invar_long_vowel_end(s, n, cp, src)) {
            if (is_aba_verb_word(s, n)) {
                size_t st = (n == 4) ? 1 : 0;
                bool have_aba = false;
                for (uint32_t j = 0; j < tok.cand_count; ++j) {
                    const Cand& e = ctx.cands[tok.cand_begin + j];
                    if (e.diac[st] == DIAC_FATHA && e.diac[st + 1] == DIAC_FATHA && e.diac[st + 2] == DIAC_NONE) {
                        have_aba = true; break;
                    }
                }
                if (!have_aba) {
                    Cand c = src;
                    c.diac[st] = DIAC_FATHA;
                    c.diac[st + 1] = DIAC_FATHA;
                    c.diac[st + 2] = DIAC_NONE;
                    c.verb_pattern = 1;
                    c.pos_tag = POS_VERB_PAST_3SG;
                    c.flags |= CF_EXPANDED;
                    c.emis = src.emis - weights_[W_EXPAND_PEN] / 2;
                    ctx.cands.push_back(c);
                    ++tok.cand_count;
                }
            }
            continue;
        }
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
            if (tok.si.verb_like == 2 && tc.verb_pattern == 0 &&
                ((tok.has_noun_lex && (tc.diac[0] == DIAC_KASRA || diac_is_tanween(tc.diac[tc.case_pos]))) ||
                 tok.is_diptote_lex == 2 || is_yeh_noun_stem(s, n, tok.si.al_len) ||
                 (n == 3 && tc.diac[1] == DIAC_SUKUN))) {
                tok.si.verb_like = 0;
            }
            // Detect 3+ char attached prepositions (بِـ/لِـ/كَـ) even when the full surface word is in the FST
            const bool valid_prep_proclitic =
                ((s[0] == S_BEH || s[0] == S_LAM) && tc.diac[0] == DIAC_KASRA) ||
                (s[0] == S_KAF && tc.diac[0] == DIAC_FATHA);
            if (tok.si.particle == PART_NONE && tok.si.al_len == 0 && n >= 3 && valid_prep_proclitic &&
                !(tok.surf_flags & SF_INNA_PRON) &&
                !tc.verb_pattern && tc.diac[n - 1] != DIAC_NONE &&
                !(s[0] == S_BEH && n == 4 && is_masdar_prep_adv(s, n)) &&
                fst_.lookup(s + 1, n - 1) != UINT32_MAX) {
                if (s[0] == S_LAM && n >= 5 && is_imperf_prefix(s[1]) && tc.diac[0] == DIAC_KASRA &&
                    !tok.has_noun_lex && diac_family(tc.diac[n - 1]) != FAM_KASRA && !diac_is_tanween(tc.diac[n - 1])) {
                    for (uint32_t k = 0; k < tok.cand_count; ++k) {
                        Cand& ck = ctx.cands[tok.cand_begin + k];
                        if (ck.diac[0] == DIAC_KASRA && diac_family(ck.diac[n - 1]) != FAM_KASRA &&
                            !diac_is_tanween(ck.diac[n - 1])) {
                            ck.flags |= CF_LAM_NASB;
                            finalize_cand(ck, s, n, tok.si, tok.has_noun_lex);
                        }
                    }
                } else {
                    tok.si.has_prep = 1;
                    for (uint32_t k = 0; k < tok.cand_count; ++k) {
                        Cand& ck = ctx.cands[tok.cand_begin + k];
                        bool ok_p0 = (s[0] == S_KAF) ? (ck.diac[0] == DIAC_FATHA) : (ck.diac[0] == DIAC_KASRA);
                        if (ok_p0 && !ck.verb_pattern) ck.flags |= CF_GEN;
                    }
                }
            }
            if (rp >= 0 && rp == 0 && tok.si.particle == PART_NONE) {
                uint8_t fixed[MAX_WORD_LEN];
                std::memcpy(fixed, s, n);
                fixed[rp] = rs;
                SurfaceInfo fsi = analyze_surface(fixed, n);
                if (fsi.particle != PART_NONE) tok.si.particle = fsi.particle;
            }
            if (n >= 4 && s[n - 1] == S_NOON) {
                bool is_tritote_noon = false;
                for (uint32_t k = 0; k < tok.cand_count; ++k) {
                    const Cand& ck = ctx.cands[tok.cand_begin + k];
                    if (ck.case_pos == n - 1) {
                        uint8_t kd = ck.diac[n - 1];
                        if (diac_family(kd) == FAM_DAMMA || diac_is_tanween(kd)) {
                            is_tritote_noon = true;
                            break;
                        }
                    }
                }
                if (is_tritote_noon) {
                    for (uint32_t k = 0; k < tok.cand_count; ++k) {
                        ctx.cands[tok.cand_begin + k].ext_flags &= static_cast<uint8_t>(~(EF_DUAL_NOON | EF_MASC_PL_NOON));
                    }
                }
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
    if (!terminal_alef_locked && !comp_num_pair && !is_invar_long_vowel_end(s, n, c.case_pos, c) &&
        !(tok.surf_flags & SF_INNA_PRON) &&
        ((c.flags & CF_GEN) || (tok.si.has_prep && tok.si.al_len > 0))) {
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
    } else if ((c.ext_flags & EF_DIPTOTE) && !(c.flags & CF_HAS_AL) && !is_invar_long_vowel_end(s, n, c.case_pos, c) &&
               pt && (pt->si.particle == PART_GEN || pt->si.particle == PART_IDAFA_NOUN || pt->si.particle == PART_NUM_IDAFA || pt->si.has_prep)) {
        if (proper_dip || !nx_mudaf_ilayh) {
            if (fam == FAM_FATHA && !diac_is_tanween(d)) r += 2 * B;
            else if (fam == FAM_KASRA || diac_is_tanween(d)) r -= 2 * B;
        } else {
            if (fam == FAM_KASRA && !diac_is_tanween(d)) r += 2 * B;
            else if (fam == FAM_FATHA || diac_is_tanween(d)) r -= B;
        }
    }
    if (tok.surf_flags & SF_INNA_PRON) {
        if (c.diac[c.case_pos] == DIAC_SHADDA_FATHA) r += 3 * B;
        else r -= 4 * B;
        if (static_cast<size_t>(c.case_pos) + 1 < n && s[c.case_pos + 1] == S_HEH) {
            if (c.diac[c.case_pos + 1] == DIAC_DAMMA) r += 2 * B;
            else if (c.diac[c.case_pos + 1] == DIAC_KASRA) r -= 4 * B;
        }
    }
    if (is_kull_word(s, n) && fam != FAM_SUKUN) {
        if (diac_has_shadda(d)) r += 3 * B;
        else r -= 4 * B;
    }
    if (is_ila_yeh_word(s, n)) {
        if (d == DIAC_SHADDA_FATHA) r += 4 * B;
        else r -= 4 * B;
    }
    if (is_aba_verb_word(s, n)) {
        size_t st_aba = (n == 4) ? 1 : 0;
        bool non_verb_kunya = pt && (pt->si.particle == PART_GEN || (pt->surf_flags & (SF_IBN | SF_NAME_HEAD)));
        if (non_verb_kunya) {
            if (c.diac[st_aba + 1] == DIAC_KASRA) r += 3 * B;
            else if (c.diac[st_aba + 1] == DIAC_FATHA) r -= 3 * B;
        } else {
            if (c.diac[st_aba + 1] == DIAC_FATHA) r += 2 * B;
            else if (c.diac[st_aba + 1] == DIAC_KASRA) r -= 3 * B;
        }
    }
    if ((c.flags & CF_SUFFIX) && c.case_pos >= 3 && s[0] == S_ALEF_HAMZA_A && c.diac[0] == DIAC_FATHA &&
        c.diac[1] == DIAC_SUKUN && c.diac[2] == DIAC_FATHA) {
        if (c.diac[c.case_pos] == DIAC_FATHA) r += 2 * B;
        else if (c.diac[c.case_pos] == DIAC_KASRA) r -= 4 * B;
    }
    if (is_umar_word(s, n)) {
        size_t st_um = (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM)) ? 1 : 0;
        if (c.diac[st_um] == DIAC_DAMMA) {
            bool age_ctx = (nx && is_age_ctx_word(&ctx.syms[nx->off], nx->n)) ||
                           (nx && nx->digit_after);
            bool name_ctx = (nx && (nx->surf_flags & SF_IBN)) ||
                            (pt && is_an_prep_word(&ctx.syms[pt->off], pt->n) && !age_ctx);
            if (age_ctx) {
                if (c.diac[st_um + 1] == DIAC_SUKUN) r += 3 * B;
                else if (c.diac[st_um + 1] == DIAC_FATHA) r -= 2 * B;
            } else if (name_ctx) {
                if (c.diac[st_um + 1] == DIAC_FATHA) r += 2 * B + B / 2;
                else if (c.diac[st_um + 1] == DIAC_SUKUN) r -= 2 * B;
            }
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

    if ((tok.surf_flags & SF_AMS) && !(c.flags & CF_HAS_AL) && !(pt && pt->si.particle == PART_JAZM)) {
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
                                   nx->si.particle == PART_PRON || (nx->surf_flags & SF_INNA_PRON) ||
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

    // Default imperfect indicative (DAMMA) when not governed by nasb/jazm
    size_t st_imp = (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH)) ? 1 : 0;
    if (n - st_imp >= 4 && s[st_imp] == S_SEEN) ++st_imp;
    if ((c.verb_pattern == 1) && !is_kana_verb_word(s, n) &&
        starts_like_imperf(s, n) && (n - st_imp) >= 6 && s[n - 2] == S_WAW && s[n - 1] == S_NOON &&
        !(c.flags & CF_HAS_AL)) {
        if (d == DIAC_FATHA) r += B;
        else r -= 3 * B;
    } else if ((c.verb_pattern == 1 || tok.si.particle == PART_KANA) &&
               starts_like_imperf(s, n) && c.case_pos == n - 1 &&
               s[c.case_pos] != S_WAW && s[c.case_pos] != S_YEH && s[c.case_pos] != S_ALEF_MAKSURA &&
               !(c.flags & (CF_SUFFIX | CF_LAM_NASB | CF_HAS_AL))) {
        const Token* p2u = (pt && !pt->boundary_before && idx >= 2 && !ctx.toks[idx - 1].boundary_before)
                           ? &ctx.toks[idx - 2] : nullptr;
        auto is_nasb_or_jazm_tok = [](const Token* t) -> bool {
            if (!t) return false;
            uint8_t p = t->si.particle;
            return p == PART_NASB || p == PART_JAZM || p == PART_AN || p == PART_LI_AN ||
                   p == PART_HATTA || p == PART_IN || p == PART_LAMA || p == PART_COND;
        };
        bool governed = is_nasb_or_jazm_tok(pt) ||
                        (pt && pt->si.particle == PART_LA && (is_nasb_or_jazm_tok(p2u) || s[0] == S_TEH));
        if (!governed) {
            if (fam == FAM_DAMMA && !diac_is_tanween(d)) r += B;
            else if (fam == FAM_FATHA) r -= B / 2;
        }
    } else if ((c.verb_pattern == 1 || tok.si.particle == PART_KANA) &&
               starts_like_imperf(s, n) && c.case_pos == n - 1 && s[c.case_pos] == S_YEH &&
               diac_family(c.diac[c.case_pos - 1]) == FAM_KASRA &&
               !(c.flags & (CF_SUFFIX | CF_LAM_NASB | CF_HAS_AL))) {
        const Token* p2u = (pt && !pt->boundary_before && idx >= 2 && !ctx.toks[idx - 1].boundary_before)
                           ? &ctx.toks[idx - 2] : nullptr;
        auto is_nasb_or_jazm_tok = [](const Token* t) -> bool {
            if (!t) return false;
            uint8_t p = t->si.particle;
            return p == PART_NASB || p == PART_JAZM || p == PART_AN || p == PART_LI_AN ||
                   p == PART_HATTA || p == PART_IN || p == PART_LAMA || p == PART_COND;
        };
        bool governed = is_nasb_or_jazm_tok(pt) ||
                        (pt && pt->si.particle == PART_LA && (is_nasb_or_jazm_tok(p2u) || s[0] == S_TEH));
        if (!governed) {
            if (d == DIAC_NONE) r += B + B / 2;
            else if (fam == FAM_FATHA) r -= 2 * B;
        }
    }

    if (tok.boundary_before && tok.si.particle == PART_NONE && !(c.flags & (CF_HAS_AL | CF_GEN)) &&
        !tok.si.has_prep && is_masdar_prep_adv(s, n) && nx && nx->si.particle == PART_GEN) {
        if (fam == FAM_FATHA && diac_is_tanween(d)) r += 3 * B;
        else if (fam == FAM_KASRA) r -= 3 * B;
    }

    const bool is_idh_word = pt && pt->si.particle == PART_COND && pt->n <= 3 &&
                             (ctx.syms[pt->off] == S_ALEF_HAMZA_B || ctx.syms[pt->off] == S_ALEF) &&
                             ctx.syms[pt->off + pt->n - 1] == 15; // إِذْ / اذ
    const bool clause_head_cond = pt && (pt->si.particle == PART_AMMA || is_idh_word);
    const bool comma_coord_waw = tok.boundary_before && (tok.bkind_mask & BK_COMMA) && !(tok.bkind_mask & BK_HARD) &&
                                 tok.si.has_conj && s[0] == S_WAW && !tok.si.has_prep;
    if (((tok.boundary_before && !comma_coord_waw) || clause_head_cond) && tok.si.particle == PART_NONE) {
        const bool bos_past_before_subj = (c.verb_pattern == 2 && fam == FAM_FATHA && !diac_is_tanween(d) &&
                                           nx && (nx->is_ibn || nx->si.particle == PART_AN || nx->si.al_len > 0 || (nx->surf_flags & SF_NAME_HEAD)));
        const bool looks_bos_noun = (c.verb_pattern == 0) ||
            (c.verb_pattern == 2 && !bos_past_before_subj && nx && (nx->si.al_len > 0 || nx->has_noun_lex));
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
    const uint8_t pp = is_ali_proper_cand(pt, pc) ? static_cast<uint8_t>(PART_NONE) : pt.si.particle;
    const uint8_t cp = is_ali_proper_cand(ct, cc) ? static_cast<uint8_t>(PART_NONE) : ct.si.particle;
    const bool c_al = (cc.flags & CF_HAS_AL) || ct.si.al_len > 0;
    const bool p_al = (pc.flags & CF_HAS_AL) || pt.si.al_len > 0;
    const uint8_t vmd = cc.verb_mood_diac;
    const uint8_t vmf = diac_family(vmd);
    const bool vmtan = diac_is_tanween(vmd);
    const bool c_is_idafa_head = (cp == PART_IDAFA_NOUN || cp == PART_NUM_IDAFA);
    const bool c_part = cp != PART_NONE && !c_is_idafa_head;
    const bool c_verb = (cc.verb_pattern == 1) || (cc.verb_pattern == 2 && ct.top_verb == 1) ||
                        (ct.si.verb_like == 2 && (ct.word_id == UINT32_MAX || ct.top_verb == 1) &&
                         !c_al && !ctan && cf != FAM_KASRA && vmf != FAM_KASRA && !vmtan);
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
        bool c_is_ali_yya = !c_is_ala && ct.n >= 3 && cc.diac[ct.n - 2] == DIAC_FATHA && diac_has_shadda(cd);
        bool c_is_ali_prop = !c_is_ala && ct.n >= 3 && cc.diac[ct.n - 2] == DIAC_KASRA;
        bool next_ibn_or_rad = nx && ((nx->surf_flags & SF_IBN) ||
                                      (nx->n >= 3 && ctx.syms[nx->off] == S_REH && ctx.syms[nx->off + 1] == S_DAD));
        bool proper_ctx = next_ibn_or_rad || pp == PART_GEN || (pt.surf_flags & SF_IBN) ||
                          (pc.verb_pattern == 1 && !c_is_ala) || is_title_noun(ps, pt.n, pt.si.al_len);
        if (proper_ctx) {
            if (c_is_ali_prop) {
                r += 2 * B + B / 2;
                if (pf != FAM_NONE && pf != FAM_SUKUN && is_title_noun(ps, pt.n, pt.si.al_len) && cf == pf) r += B;
            } else if (c_is_ali_yya || c_is_ala) {
                r -= 3 * B;
            }
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
            else if (c_noun) {
                if (cp == PART_IDAFA_NOUN || cp == PART_NUM_IDAFA || cf == FAM_FATHA || c_al) r -= 4 * B;
                else if (cf == FAM_KASRA || (p2 && (p2->surf_flags & SF_QUANT_IDAFA))) {
                    r -= (p2 && (p2->surf_flags & SF_QUANT_IDAFA)) ? (4 * B) : (2 * B);
                }
            }
        } else {
            if (c_verb || c_imperf) r -= 2 * B;
            else if (c_noun) {
                const bool inv_end = is_invar_long_vowel_end(cs, ct.n, cc.case_pos, cc);
                if (cc.ext_flags & (EF_MASC_PL_NOON | EF_DUAL_NOON)) {
                    r += B;
                } else if (inv_end) {
                    if (cf == FAM_NONE) r += B;
                } else if (c_diptote && !c_al) {
                    const bool c_proper_dip = (ct.is_diptote_lex == 2);
                    const bool nx_mudaf = !c_proper_dip && nx && nx->si.particle == PART_NONE &&
                                          nx->si.verb_like < 2 && nx->top_verb != 1 && !nx->is_adj_like && !nx->is_ibn && !nx->si.has_prep;
                    if (nx_mudaf) {
                        if (cf == FAM_KASRA) r += 2 * B;
                    } else {
                        if (cf == FAM_FATHA && !ctan) r += 2 * B;
                        else r -= B + B / 2;
                    }
                } else if (cf == FAM_KASRA) {
                    r += (cp == PART_IDAFA_NOUN || cp == PART_NUM_IDAFA || c_al) ? (2 * B + B / 2) : (B + B / 2);
                } else if (cf != FAM_NONE && !(cc.flags & CF_OOV)) {
                    r -= B + B / 2;
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
            if (nx_is_fem_noun) r += 2 * B + B / 2;
            else if (nx_is_al) r += B;
        } else if (ct.n >= 5 && cs[ct.n - 1] == S_TEH && cc.pos_tag == POS_VERB_PAST_SUBJ && nx_is_fem_noun) {
            r -= 2 * B;
        }
    }
    if (pt.n == 3 && ps[0] == S_ALEF_HAMZA_A && ps[1] == S_NOON && ps[2] == S_TEH &&
        ct.n == 4 && ((cs[0] == S_TAH && cs[1] == S_ALEF && cs[2] == S_LAM && cs[3] == S_QAF) ||
                      (cs[0] == S_HAH && cs[1] == S_ALEF && cs[2] == S_YEH_HAMZA && cs[3] == S_DAD))) {
        if (pc.diac[2] == DIAC_KASRA) r += 3 * B;
        else if (pc.diac[2] == DIAC_FATHA) r -= 2 * B;
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
                if (pp == PART_NUM_IDAFA && (pt.surf_flags & SF_ASHAR) && p2 && (p2->surf_flags & SF_COMP_NUM)) {
                    // Compound numbers 11-19 (أحد عشر..تسع عشرة) take singular accusative Tamyiz, not genitive!
                    if (c_noun && !c_part && !c_al && !(cc.flags & CF_SUFFIX) && !c_terminal_alef_locked) {
                        const bool nx_mudaf_tm = nx && nx->si.particle == PART_NONE && !nx->si.has_prep &&
                                                 (nx->si.al_len > 0 || (nx->surf_flags & SF_NAME_HEAD));
                        if (cf == FAM_FATHA && (nx_mudaf_tm || c_diptote || ctan)) r += 2 * B;
                        else if (cf == FAM_FATHA) r += B;
                        else if (cf == FAM_KASRA) r -= 2 * B;
                    }
                    break;
                }
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
                bool next_is_indef_gen_target = c_noun && !c_al && !c_part && !ct.si.has_prep && cf == FAM_KASRA;
                if (c_verb || cp == PART_GEN || cp == PART_CONJ || cp == PART_LA || cp == PART_AN ||
                    cp == PART_INNA || cp == PART_PREP_PRON || c_al ||
                    (c_noun && cf == FAM_DAMMA && (!nx || nx->si.particle == PART_NONE))) {
                    if (!ay_sh && pd == DIAC_SUKUN) r += 2 * B;
                    else if (!ay_sh) r -= B;
                }
                if (next_is_indef_gen_target) {
                    if (ay_sh) r += 2 * B;
                    else r -= B + B / 2;
                } else if (ay_sh && c_noun && !c_al && !c_part && (cf == FAM_DAMMA || cf == FAM_FATHA)) {
                    r -= B;
                }
                if (ay_sh && pf == FAM_KASRA && (!p2 || (p2->si.particle != PART_GEN && p2->si.particle != PART_IDAFA_NOUN &&
                    !p2->si.has_prep && !(p2->si.particle == PART_NONE && p2->si.al_len == 0 && p2->top_verb != 1)))) {
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
                                    cp == PART_KANA || cp == PART_QAD ||
                                    (ct.n >= 4 && cs[ct.n - 2] == S_WAW && cs[ct.n - 1] == S_ALEF);
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
                        r += (c_noun && !c_al && ctan && cf == FAM_FATHA) ? (3 * B) : (2 * B);
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
                        r -= (c_noun && !c_al && ctan && cf == FAM_FATHA) ? (3 * B) : (2 * B);
                    }
                }
                break;
            }

            case PART_NASB:
                if (c_imperf || c_verb) {
                    if (vmd == DIAC_FATHA) r += 2 * B;
                    else if (vmf != FAM_NONE) r -= 2 * B;
                } else if (c_noun && !c_part) {
                    r -= 2 * B;
                }
                break;

            case PART_JAZM:
                if (c_imperf || c_verb) {
                    if (vmf == FAM_SUKUN) r += 2 * B;
                    else if (vmf == FAM_DAMMA || vmtan) r -= 4 * B;
                    else if (vmf == FAM_FATHA && cs[ct.n - 1] != S_ALEF_MAKSURA) r -= B;
                } else if (c_noun && !c_part) {
                    r -= 2 * B + B / 2;
                }
                break;

            case PART_INNA:
            case PART_LAKIN: {
                bool in_sh = diac_has_shadda(pd);
                bool next_verbish = c_imperf || c_verb || cp == PART_JAZM || cp == PART_NASB ||
                                    cp == PART_COND || cp == PART_QAD ||
                                    (ct.n >= 4 && cs[ct.n - 2] == S_WAW && cs[ct.n - 1] == S_ALEF);
                if (pp == PART_LAKIN && (next_verbish || cp == PART_GEN || c_own_gen)) {
                    if (!in_sh) r += 2 * B;
                    else r -= 2 * B;
                } else if (c_noun || cp == PART_DEM || cp == PART_PRON || cp == PART_REL ||
                           cp == PART_PREP_PRON || (pp == PART_INNA && cp == PART_GEN)) {
                    if (in_sh) r += (c_noun && !c_al && ctan && cf == FAM_FATHA) ? (3 * B) : (2 * B);
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
                                    cp == PART_JAZM || cp == PART_QAD ||
                                    is_aba_verb_word(cs, ct.n) ||
                                    (ct.n >= 4 && cs[ct.n - 2] == S_WAW && cs[ct.n - 1] == S_ALEF) ||
                                    (cc.verb_pattern == 2 && cf == FAM_FATHA && !ctan && !c_al);
                if (next_verbish) {
                    if (!in_sh) {
                        r += 2 * B;
                        if ((c_imperf || c_verb) && vmf == FAM_SUKUN) r += B;
                    } else {
                        r -= 2 * B + B / 2;
                    }
                } else if (c_noun || cp == PART_DEM || cp == PART_PRON || cp == PART_PREP_PRON || cp == PART_GEN ||
                           (ct.surf_flags & SF_INNA_PRON)) {
                    if (in_sh) {
                        r += (c_noun && !c_al && ctan && cf == FAM_FATHA) ? (3 * B) : (2 * B);
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
                        r -= (c_noun && !c_al && ctan && cf == FAM_FATHA) ? (3 * B) : (2 * B);
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
                if (c_imperf || c_verb) {
                    if (vmf == FAM_DAMMA && !vmtan) r += B / 2;
                    if (vmf == FAM_KASRA || vmtan) r -= B;
                } else if (c_noun && !c_part) {
                    r -= 2 * B;
                }
                break;

            case PART_QAD: {
                const bool is_laqad = (pt.n == 3 && ps[0] == S_LAM && ps[1] == S_QAF && ps[2] == S_DAL);
                if (c_imperf || c_verb) {
                    if (!is_laqad && c_imperf && !(cs[0] == S_TEH && ct.n >= 4 && cc.diac[1] == DIAC_FATHA) &&
                        vmf == FAM_DAMMA && !vmtan) {
                        r += B / 2;
                    }
                    if (vmf == FAM_KASRA || vmtan) r -= B;
                }
                break;
            }

            case PART_YA:
                if (c_noun && !c_al) {
                    const bool voc_mudaf = nx && !nx->si.has_prep && !nx->si.has_conj &&
                                           (nx->si.al_len > 0 || is_allah_word(&ctx.syms[nx->off], nx->n));
                    if (voc_mudaf) {
                        if (cd == DIAC_FATHA) r += B + B / 2;
                        else if (cf == FAM_DAMMA || cf == FAM_KASRA) r -= B;
                    } else {
                        if (cd == DIAC_DAMMA) r += B + B / 2;
                        else if (cf == FAM_KASRA) r -= B;
                    }
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
                if (is_dem_det_word(ps, pt.n) && c_al && c_noun && p2) {
                    const bool p2_mudaf = (p2->si.particle == PART_NONE && p2->si.al_len == 0 &&
                                           p2->top_verb != 1 && p2->si.verb_like < 2 &&
                                           !diac_is_tanween(p2->lex_top_end) &&
                                           !(p2->surf_flags & (SF_AMS | SF_THEMMA | SF_ASHAR)));
                    if (p2->si.particle == PART_GEN || p2->si.particle == PART_IDAFA_NOUN ||
                        p2->si.particle == PART_NUM_IDAFA || p2->si.has_prep || p2_mudaf) {
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
        const bool p_known_nonverb = (pt.word_id != UINT32_MAX && pc.verb_pattern == 0 && pt.top_verb == 0);
        const bool p_noun = (pc.verb_pattern == 0) && (pt.si.verb_like < 2 || p_known_nonverb) && !(pc.flags & CF_FUT) &&
                            pp != PART_IDAFA_NOUN && pp != PART_NUM_IDAFA;
        const bool p_verb = (pc.verb_pattern == 1) || (pt.si.verb_like == 2 && !p_known_nonverb && !p_al);

        if (is_innama_word(ps, pt.n) && c_noun) {
            if (cf == FAM_DAMMA) r += B + B / 2;
            else if (cf == FAM_KASRA) r -= 2 * B;
        }
        if ((pt.surf_flags & SF_AMS) && c_noun && is_day_of_week_word(cs, ct.n, ct.si.al_len)) {
            if (cf == FAM_FATHA) r += 2 * B;
            else if (cf == FAM_KASRA) r -= 2 * B;
        }
        if (is_decade_num_word(ps, pt.n) && c_noun && !c_al && !(cc.flags & CF_SUFFIX) &&
            !(cc.ext_flags & (EF_FEM_PL_AT | EF_MASC_PL_NOON | EF_DUAL_NOON)) && !c_diptote && !c_terminal_alef_locked) {
            const bool nx_mudaf_d = nx && nx->si.particle == PART_NONE && !nx->si.has_prep &&
                                    (nx->si.al_len > 0 || (nx->surf_flags & SF_NAME_HEAD));
            if (cf == FAM_FATHA && (nx_mudaf_d || ctan)) r += B + B / 2;
            else if (cf == FAM_KASRA && ctan) r -= B;
        }

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
            } else if (has_verb_obj_suffix(ps, pt.n, pc)) {
                if (!is_time_adverb_noun(cs, ct.n, ct.si.al_len)) {
                    if (cf == FAM_DAMMA) r += 2 * B + B / 2;
                    else if (cf == FAM_FATHA) r -= 2 * B;
                }
                if (cf == FAM_KASRA) r -= 4 * B;
            } else if (pc.pos_tag == POS_VERB_PASS) {
                if (!is_time_adverb_noun(cs, ct.n, ct.si.al_len)) {
                    if (cf == FAM_DAMMA) r += B;
                    else if (cf == FAM_FATHA) r -= 2 * B;
                }
                if (cf == FAM_KASRA) r -= 3 * B;
            } else {
                if (c_fem_at) {
                    if (cf == FAM_FATHA) r -= 3 * B;
                } else {
                    if (cf == FAM_KASRA) r -= 3 * B;
                }
                if ((c_al || is_title_noun(cs, ct.n, ct.si.al_len)) && !is_time_adverb_noun(cs, ct.n, ct.si.al_len)) {
                    if (cf == FAM_DAMMA) r += B;
                    else if (cf == FAM_KASRA) r -= B;
                } else if ((pc.ext_flags & EF_PAST_TEH_FEM) && c_al &&
                           (cs[ct.n - 1] == S_TEH_MARBUTA || pd == DIAC_KASRA)) {
                    if (cf == FAM_DAMMA) r += B;
                    else if (cf == FAM_KASRA) r -= B;
                }
            }
        }
        if ((pt.surf_flags & SF_INNA_PRON) && c_noun && !c_own_gen) {
            if (cf == FAM_DAMMA) r += B + B / 2;
            else if (cf == FAM_KASRA) r -= 2 * B;
        }
        if (p_noun && c_noun && !c_al && is_title_noun(ps, pt.n, pt.si.al_len) && !(ct.surf_flags & SF_IBN) &&
            pf != FAM_NONE && pf != FAM_SUKUN && cf != FAM_NONE && cf != FAM_SUKUN) {
            if (cf == pf) r += 2 * B;
            else r -= B + B / 2;
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

        const bool p_non_idafa_mabni = (pt.surf_flags & (SF_AMS | SF_THEMMA | SF_ASHAR)) != 0 ||
                                       is_innama_word(ps, pt.n) || is_decade_num_word(ps, pt.n);
        if (p_noun && !p_al && !(pc.flags & CF_SUFFIX) && !p_non_idafa_mabni && c_al && c_noun) {
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
        if (p_noun && !p_al && !ptan && !(pc.flags & (CF_SUFFIX | CF_EXPANDED)) && !p_non_idafa_mabni &&
            !pt.top_verb && !likely_khabar_plus_adj &&
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
        if (p_noun && !p_al && !(pc.flags & CF_SUFFIX) && !p_non_idafa_mabni && c_noun && !c_al && !(cc.flags & CF_SUFFIX) &&
            (ct.is_adj_like || c_meem_participle) && !(cc.ext_flags & EF_FEM_PL_AT)) {
            if (ptan && pf == cf && ctan) r += 2 * B;
            else if (ptan && pf != cf && cf == FAM_KASRA && pf != FAM_KASRA) r -= 2 * B;
            else if (!ptan && ctan && pf == cf && !(pc.ext_flags & EF_DIPTOTE)) r -= B;
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

        if (p_noun && (c_noun || cp == PART_IDAFA_NOUN) && ct.n >= 4 && (cs[0] == S_WAW || cs[0] == S_FEH) &&
            !ct.si.has_prep && pf != FAM_NONE && pf != FAM_SUKUN && (p_al == c_al || (ptan && ctan))) {
            if (cf == pf) r += B + B / 2;
        }
    }
    return static_cast<int32_t>((static_cast<int64_t>(r) * weights_[W_RULE]) / 1000);
}

std::string Diacritizer::diacritize(const char* text, size_t len) const {
    return diacritize_internal(text, len, nullptr, nullptr);
}

std::string Diacritizer::diacritize_internal(
    const char* text, size_t len,
    std::vector<Irab2DumpSample>* dump_out,
    const std::vector<std::array<uint8_t, MAX_WORD_LEN>>* gold_diacs) const {
    if (!is_loaded() || !text || len == 0) return text ? std::string(text, len) : std::string();

    struct Span {
        bool is_word;
        uint32_t a;
        uint32_t b;
    };
    struct CandCacheSlot {
        const Diacritizer* owner = nullptr;
        uint64_t epoch = 0;
        uint32_t hash = 0;
        uint8_t n = 0;
        uint8_t syms[MAX_WORD_LEN];
        uint32_t word_id = UINT32_MAX;
        uint8_t role = 0;
        uint8_t lex_top_end = 255;
        uint8_t top_verb = 0;
        uint8_t has_noun_lex = 0;
        uint8_t is_fem_at = 0;
        uint8_t is_diptote_lex = 0;
        uint8_t is_ibn = 0;
        uint8_t is_adj_like = 0;
        uint8_t top_pos = POS_UNK;
        uint16_t surf_flags = 0;
        SurfaceInfo si;
        uint8_t cand_count = 0;
        Cand cands[MAX_CANDS];
    };
    struct Scratch {
        std::vector<Span> spans;
        std::string raw_pool;
        Ctx ctx;
        std::vector<uint32_t> dp_off;
        std::vector<int32_t> dp;
        std::vector<int32_t> dp_bg;
        std::vector<int32_t> back;
        std::vector<uint32_t> chosen;
        std::vector<BeamHyp> beam_cur;
        std::vector<BeamHyp> beam_nxt;
        std::vector<BeamNode> beam_bp;
        std::vector<uint32_t> beam_opts;
        std::vector<int32_t> override_idx;
        std::vector<Cand> override_cands;
        std::vector<CandCacheSlot> cand_cache;
        std::vector<int32_t> irab2_bonus;
        std::vector<int32_t> p1_irab1_scores;
        std::vector<int8_t> p1_irab1_best;
        std::vector<int8_t> p1_bg_best;
        std::vector<int16_t> p1_bg_val;
        std::vector<int8_t> p1_rbg_best;
        std::vector<int16_t> p1_rbg_val;
        std::vector<int8_t> p1_ng_best;
        std::vector<int8_t> p1_skip_best;
    };
    thread_local Scratch sc_buf;
    if (sc_buf.cand_cache.size() != 4096) sc_buf.cand_cache.resize(4096);
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

    auto is_digit_cp = [](uint32_t c) -> int {
        if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
        if (c >= 0x0660 && c <= 0x0669) return static_cast<int>(c - 0x0660);
        return -1;
    };
    auto classify_bmask = [](uint32_t c) -> uint8_t {
        if (c == '.' || c == '!' || c == '?' || c == 0x061Fu || c == ':' || c == ';' || c == 0x061Bu || c == '\n' || c == '\r')
            return BK_HARD;
        if (c == ',' || c == 0x060Cu) return BK_COMMA;
        if (c == '(' || c == '[' || c == '{' || c == 0x00ABu) return BK_OPEN;
        if (c == ')' || c == ']' || c == '}' || c == 0x00BBu) return BK_CLOSE;
        return BK_OTHER;
    };
    size_t pos = 0;
    uint8_t pending_boundary = 1;
    uint8_t pending_bmask = BK_HARD;
    bool in_num = false;
    uint64_t num_val = 0;
    bool num_dec = false;
    uint8_t num_digits = 0;
    size_t raw_start = raw_pool.size();
    while (pos < len) {
        size_t save = pos;
        uint32_t cp = utf8_next(text, len, pos);
        if (is_arabic_letter(cp)) {
            if (in_num && !pending_boundary && !ctx.toks.empty()) {
                Token& prev_t = ctx.toks.back();
                prev_t.digit_after = 1;
                if (num_dec) prev_t.num_after_class = 6;
                else if (num_digits == 4 && num_val >= 1900 && num_val <= 2100) prev_t.num_after_class = 5;
                else if (num_val >= 1 && num_val <= 2) prev_t.num_after_class = 1;
                else if ((num_val % 100) >= 3 && (num_val % 100) <= 10) prev_t.num_after_class = 2;
                else if ((num_val % 100) >= 11 && (num_val % 100) <= 99) prev_t.num_after_class = 3;
                else if (num_val >= 100 && (num_val % 100) == 0) prev_t.num_after_class = 4;
                else prev_t.num_after_class = 1;
            }
            in_num = false;
            num_val = 0;
            num_dec = false;
            num_digits = 0;
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
                pending_bmask |= BK_OTHER;
                continue;
            }
            tok.boundary_before = pending_boundary;
            tok.bkind_mask = pending_bmask;
            pending_boundary = 0;
            pending_bmask = BK_NONE;
            spans.push_back({true, static_cast<uint32_t>(ctx.toks.size()), 0});
            ctx.toks.push_back(tok);
            raw_start = raw_pool.size();
        } else {
            int dv = is_digit_cp(cp);
            if (dv >= 0) {
                if (!in_num) {
                    in_num = true;
                    num_val = static_cast<uint64_t>(dv);
                    num_dec = false;
                    num_digits = 1;
                } else if (!num_dec && num_digits < 12) {
                    num_val = num_val * 10u + static_cast<uint64_t>(dv);
                    ++num_digits;
                }
                if (!pending_boundary && !ctx.toks.empty()) ctx.toks.back().digit_after = 1;
            } else if (in_num && (cp == '.' || cp == ',' || cp == 0x066B) && pos < len) {
                size_t pk = pos;
                uint32_t nxt = utf8_next(text, len, pk);
                if (is_digit_cp(nxt) >= 0) {
                    num_dec = true;
                } else {
                    in_num = false;
                    if (is_boundary_cp(cp)) {
                        pending_boundary = 1;
                        pending_bmask |= classify_bmask(cp);
                    }
                }
            } else {
                if (is_boundary_cp(cp)) {
                    pending_boundary = 1;
                    pending_bmask |= classify_bmask(cp);
                    in_num = false;
                }
            }
            if (!is_arabic_diacritic(cp)) raw_pool.append(text + save, pos - save);
        }
    }
    if (raw_pool.size() > raw_start) {
        spans.push_back({false, static_cast<uint32_t>(raw_start), static_cast<uint32_t>(raw_pool.size())});
    }
    if (ctx.toks.empty()) return std::string(text, len);

    for (auto& t : ctx.toks) {
        const uint8_t* ts = &ctx.syms[t.off];
        const uint32_t h = fnv1a_32_syms(ts, t.n);
        CandCacheSlot& slot = sc_buf.cand_cache[h & 4095u];
        if (slot.owner == this && slot.epoch == cache_epoch_ &&
            slot.hash == h && slot.n == t.n && std::memcmp(slot.syms, ts, t.n) == 0) {
            t.cand_begin = static_cast<uint32_t>(ctx.cands.size());
            t.cand_count = slot.cand_count;
            t.word_id = slot.word_id;
            t.role = slot.role;
            t.lex_top_end = slot.lex_top_end;
            t.top_verb = slot.top_verb;
            t.has_noun_lex = slot.has_noun_lex;
            t.is_fem_at = slot.is_fem_at;
            t.is_diptote_lex = slot.is_diptote_lex;
            t.is_ibn = slot.is_ibn;
            t.is_adj_like = slot.is_adj_like;
            t.top_pos = slot.top_pos;
            t.surf_flags = slot.surf_flags;
            t.si = slot.si;
            for (uint32_t k = 0; k < slot.cand_count; ++k) ctx.cands.push_back(slot.cands[k]);
        } else {
            build_candidates(ctx, t);
            if (t.cand_count > 0) {
                t.top_verb = ctx.cands[t.cand_begin].verb_pattern;
                t.top_pos = ctx.cands[t.cand_begin].pos_tag;
            }
            if (t.n <= MAX_WORD_LEN && t.cand_count <= MAX_CANDS) {
                slot.owner = this;
                slot.epoch = cache_epoch_;
                slot.hash = h;
                slot.n = t.n;
                std::memcpy(slot.syms, ts, t.n);
                slot.word_id = t.word_id;
                slot.role = t.role;
                slot.lex_top_end = t.lex_top_end;
                slot.top_verb = t.top_verb;
                slot.has_noun_lex = t.has_noun_lex;
                slot.is_fem_at = t.is_fem_at;
                slot.is_diptote_lex = t.is_diptote_lex;
                slot.is_ibn = t.is_ibn;
                slot.is_adj_like = t.is_adj_like;
                slot.top_pos = t.top_pos;
                slot.surf_flags = t.surf_flags;
                slot.si = t.si;
                slot.cand_count = static_cast<uint8_t>(t.cand_count);
                for (uint32_t k = 0; k < t.cand_count; ++k) slot.cands[k] = ctx.cands[t.cand_begin + k];
            }
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

    // V7 Lexical Core-Disambiguation Table (left bigram + right bigram + centered trigram for homographs / passive / من / إن / أن)
    if (is_v7_ && !core_bg_.empty() &&
        (v7_weights_[V7W_CORE_BG_L] != 0 || v7_weights_[V7W_CORE_BG_R] != 0 || v7_weights_[V7W_CORE_BG_C] != 0)) {
        const int32_t w_cbl = v7_weights_[V7W_CORE_BG_L];
        const int32_t w_cbr = v7_weights_[V7W_CORE_BG_R];
        const int32_t w_cbc = v7_weights_[V7W_CORE_BG_C];
        for (size_t i = 0; i < W; ++i) {
            const Token& t = ctx.toks[i];
            if (t.word_id == UINT32_MAX || t.cand_count < 2) continue;
            const uint8_t* ts = &ctx.syms[t.off];
            const uint8_t lam_skip = (t.si.al_len >= 2) ? t.si.al_lam_pos : 255;
            uint16_t ch0 = compute_core_hash(ts, ctx.cands[t.cand_begin].diac, ctx.cands[t.cand_begin].case_pos, lam_skip);
            bool multi_core = false;
            uint16_t ch_arr[MAX_CANDS];
            ch_arr[0] = ch0;
            const uint32_t cc_lim = std::min<uint32_t>(t.cand_count, MAX_CANDS);
            for (uint32_t k = 1; k < cc_lim; ++k) {
                const Cand& ck = ctx.cands[t.cand_begin + k];
                uint16_t chk = compute_core_hash(ts, ck.diac, ck.case_pos, lam_skip);
                ch_arr[k] = chk;
                if (chk != ch0) multi_core = true;
            }
            if (!multi_core) continue;

            const Token* p = (i >= 1 && !t.boundary_before) ? &ctx.toks[i - 1] : nullptr;
            const Token* nx = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? &ctx.toks[i + 1] : nullptr;
            CoreBgLookupResult el_v{}, er_v{}, ec_v{};
            bool has_el = false, has_er = false, has_ec = false;
            if (w_cbl != 0 && p && p->word_id != UINT32_MAX) {
                uint32_t ws[2] = {p->word_id, t.word_id};
                has_el = core_bg_.find_val(CORE_DIR_LEFT, ws, 2, el_v);
            }
            if (w_cbr != 0 && nx && nx->word_id != UINT32_MAX) {
                uint32_t ws[2] = {t.word_id, nx->word_id};
                has_er = core_bg_.find_val(CORE_DIR_RIGHT, ws, 2, er_v);
            }
            if (w_cbc != 0 && p && nx && p->word_id != UINT32_MAX && nx->word_id != UINT32_MAX) {
                uint32_t ws[3] = {p->word_id, t.word_id, nx->word_id};
                has_ec = core_bg_.find_val(CORE_DIR_CTR3, ws, 3, ec_v);
            }
            if (!has_el && !has_er && !has_ec) continue;

            for (uint32_t k = 0; k < cc_lim; ++k) {
                Cand& c = ctx.cands[t.cand_begin + k];
                uint16_t h = ch_arr[k];
                int32_t bon = 0;
                if (has_el) {
                    if (h == el_v.best_core_h) bon += (static_cast<int32_t>(el_v.best_bonus) * w_cbl) / 1000;
                    else if (h == el_v.alt_core_h) bon += (static_cast<int32_t>(el_v.alt_bonus) * w_cbl) / 1000;
                }
                if (has_er) {
                    if (h == er_v.best_core_h) bon += (static_cast<int32_t>(er_v.best_bonus) * w_cbr) / 1000;
                    else if (h == er_v.alt_core_h) bon += (static_cast<int32_t>(er_v.alt_bonus) * w_cbr) / 1000;
                }
                if (has_ec) {
                    if (h == ec_v.best_core_h) bon += (static_cast<int32_t>(ec_v.best_bonus) * w_cbc) / 1000;
                    else if (h == ec_v.alt_core_h) bon += (static_cast<int32_t>(ec_v.alt_bonus) * w_cbc) / 1000;
                }
                c.emis += bon;
            }
        }
    }

    const bool track_p2_sigs = (dump_out != nullptr) || (is_v7_ && irab2_trees_.valid() && v7_weights_[V7W_IRAB2] != 0);
    if (track_p2_sigs) {
        sc_buf.p1_irab1_scores.assign(W * NUM_DIACRITICS, 0);
        sc_buf.p1_irab1_best.assign(W, -1);
        sc_buf.p1_bg_best.assign(W, -1);
        sc_buf.p1_bg_val.assign(W, 0);
        sc_buf.p1_rbg_best.assign(W, -1);
        sc_buf.p1_rbg_val.assign(W, 0);
        sc_buf.p1_ng_best.assign(W, -1);
        sc_buf.p1_skip_best.assign(W, -1);
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
            if (track_p2_sigs) {
                uint32_t nc = irab_trees_.num_classes();
                int32_t b_sc = NEG_INF;
                int8_t b_id = -1;
                for (uint32_t c_id = 0; c_id < nc; ++c_id) {
                    sc_buf.p1_irab1_scores[i * NUM_DIACRITICS + c_id] =
                        static_cast<int32_t>((static_cast<int64_t>(sc[c_id]) * weights_[W_IRAB_TREE]) / 1000);
                    if (sc[c_id] > b_sc) { b_sc = sc[c_id]; b_id = static_cast<int8_t>(c_id); }
                }
                sc_buf.p1_irab1_best[i] = b_id;
            }
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
    sc_buf.dp_bg.assign(dp_off[W], 0);
    auto& dp_bg = sc_buf.dp_bg;
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
        NgramLookupResult ng_hit_v{}, ng_rbg_v{}, ng_ctr3_v{}, ng_skip1_v{};
        bool has_ng_hit = false, has_ng_rbg = false, has_ng_ctr3 = false, has_ng_skip1 = false;
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
            if (!ngrams_.empty() && p2 && p2->word_id != UINT32_MAX) {
                const Token* p4 = (p3 && i >= 4 && !p3->boundary_before) ? &ctx.toks[i - 4] : nullptr;
                if (p4 && p3 && p4->word_id != UINT32_MAX && p3->word_id != UINT32_MAX) {
                    uint32_t w5[5] = {p4->word_id, p3->word_id, p2->word_id, pt->word_id, ct.word_id};
                    has_ng_hit = ngrams_.find_val(w5, 5, ng_hit_v, 5);
                }
                if (!has_ng_hit && p3 && p3->word_id != UINT32_MAX) {
                    uint32_t w4[4] = {p3->word_id, p2->word_id, pt->word_id, ct.word_id};
                    has_ng_hit = ngrams_.find_val(w4, 4, ng_hit_v, 4);
                }
                if (!has_ng_hit) {
                    uint32_t w3[3] = {p2->word_id, pt->word_id, ct.word_id};
                    has_ng_hit = ngrams_.find_val(w3, 3, ng_hit_v, 3);
                }
            }
        }
        if (is_v7_ && !ngrams_.empty() && ct.word_id != UINT32_MAX) {
            if (nx && nx->word_id != UINT32_MAX) {
                uint32_t wr[2] = {ct.word_id, nx->word_id};
                has_ng_rbg = ngrams_.find_val(wr, NG_ORDER_RBG, ng_rbg_v, 2);
                if (pt && pt->word_id != UINT32_MAX) {
                    uint32_t wc3[3] = {pt->word_id, ct.word_id, nx->word_id};
                    has_ng_ctr3 = ngrams_.find_val(wc3, NG_ORDER_CTR3, ng_ctr3_v, 3);
                }
            }
            if (p2 && p2->word_id != UINT32_MAX) {
                uint32_t ws1[2] = {p2->word_id, ct.word_id};
                has_ng_skip1 = ngrams_.find_val(ws1, NG_ORDER_SKIP1, ng_skip1_v, 2);
            }
        }
        if (track_p2_sigs) {
            if (bg_v5 >= 0 || bg_v3 || bg_v2) {
                int16_t bv = -32000;
                int8_t be = -1;
                for (uint8_t d = 0; d < NUM_DIACRITICS; ++d) {
                    int16_t v = (bg_v5 >= 0) ? bigrams_.bonus_v5(bg_v5, bg_mask, d)
                              : (bg_v3 ? bigrams_.bonus_v3(bg_v3, bg_mask, d) : bg_v2->bonus[d]);
                    if (v > bv) { bv = v; be = static_cast<int8_t>(d); }
                }
                if (bv > 0) { sc_buf.p1_bg_best[i] = be; sc_buf.p1_bg_val[i] = bv; }
            }
            if (has_ng_rbg && ng_rbg_v.best_bonus > 0) {
                sc_buf.p1_rbg_best[i] = static_cast<int8_t>(ng_rbg_v.best_ending_id);
                sc_buf.p1_rbg_val[i] = ng_rbg_v.best_bonus;
            }
            if (has_ng_ctr3 && ng_ctr3_v.best_bonus > 0) {
                sc_buf.p1_ng_best[i] = static_cast<int8_t>(ng_ctr3_v.best_ending_id);
            } else if (has_ng_hit && ng_hit_v.best_bonus > 0) {
                sc_buf.p1_ng_best[i] = static_cast<int8_t>(ng_hit_v.best_ending_id);
            }
            if (has_ng_skip1 && ng_skip1_v.best_bonus > 0) {
                sc_buf.p1_skip_best[i] = static_cast<int8_t>(ng_skip1_v.best_ending_id);
            }
        }

        auto calc_ng_entry = [](bool hit, const NgramLookupResult& e, uint8_t ce, int32_t w_scale) -> int32_t {
            if (!hit || w_scale == 0) return 0;
            int32_t raw = 0;
            if (ce == e.best_ending_id) raw = e.best_bonus;
            else if (ce == e.alt_ending_id) raw = e.alt_bonus;
            if (raw == 0) return 0;
            return static_cast<int32_t>((static_cast<int64_t>(raw) * w_scale) / 1000);
        };

        for (uint32_t c = 0; c < ct.cand_count; ++c) {
            const Cand& cc = ctx.cands[ct.cand_begin + c];
            const uint8_t ce = cc.diac[cc.case_pos];
            if (fresh) {
                uint8_t cr = has_v5_24roles_ ? cand_role_v5(ct, cc) : ct.role;
                int32_t t = static_cast<int32_t>((static_cast<int64_t>(trans_score(0, DIAC_NONE, cr, ce)) * w_tr) / 1000);
                int32_t bgb_f = is_v7_ ? calc_ng_entry(has_ng_rbg, ng_rbg_v, ce, v7_weights_[V7W_RBG]) : 0;
                dp[dp_off[i] + c] = prev_best + cc.emis + t + bgb_f;
                dp_bg[dp_off[i] + c] = bgb_f;
                back[dp_off[i] + c] = prev_best_idx;
                continue;
            }
            int16_t raw_bg = (bg_v5 >= 0) ? bigrams_.bonus_v5(bg_v5, bg_mask, ce)
                           : (bg_v3 ? bigrams_.bonus_v3(bg_v3, bg_mask, ce)
                           : (bg_v2 ? bg_v2->bonus[ce] : 0));
            int32_t bgb = raw_bg ? static_cast<int32_t>((static_cast<int64_t>(raw_bg) * w_bg) / 1000) : 0;
            if (has_ng_hit) {
                int32_t ng_raw = 0;
                if (ce == ng_hit_v.best_ending_id) ng_raw = ng_hit_v.best_bonus;
                else if (ce == ng_hit_v.alt_ending_id) ng_raw = ng_hit_v.alt_bonus;
                if (ng_raw != 0) {
                    int32_t w_ng_eff = is_v7_ ? v7_weights_[V7W_NGRAM_LEFT] : w_bg;
                    int32_t mult = (ng_hit_v.order >= 5) ? 1500 : ((ng_hit_v.order == 4) ? 1250 : 1000);
                    bgb += static_cast<int32_t>((static_cast<int64_t>(ng_raw) * w_ng_eff * mult) / 1000000);
                }
            }
            if (is_v7_) {
                bgb += calc_ng_entry(has_ng_rbg, ng_rbg_v, ce, v7_weights_[V7W_RBG]);
                bgb += calc_ng_entry(has_ng_ctr3, ng_ctr3_v, ce, v7_weights_[V7W_CTR3]);
                bgb += calc_ng_entry(has_ng_skip1, ng_skip1_v, ce, v7_weights_[V7W_SKIP1]);
            }
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
            dp_bg[dp_off[i] + c] = bgb;
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

    // Sentence-Level Case-Lattice Beam Search (Solutions 1-5: ClauseStack + PP/NP Chunking + Hard Invariants + Atf Anchors)
    {
        constexpr size_t BEAM_W = 4;
        auto& beam_cur = sc_buf.beam_cur;
        auto& beam_nxt = sc_buf.beam_nxt;
        auto& beam_bp = sc_buf.beam_bp;
        auto& opts = sc_buf.beam_opts;

        auto step_syn = [&](SynState& st, size_t idx, const Token& t, const Cand& c,
                            const Token* pt, const Cand* pc, const Token* nx, bool is_seg_end) -> int32_t {
            int32_t syn = 0;
            const uint8_t* ts = &ctx.syms[t.off];
            bool ambig_obl = false;
            const uint8_t cf = eff_case_fam(ts, c, &ambig_obl);
            const uint8_t cd = c.diac[c.case_pos];
            const bool tan = diac_is_tanween(cd);
            const bool c_al = (c.flags & CF_HAS_AL) || t.si.al_len > 0;
            const bool c_gen = (c.flags & CF_GEN) || t.si.has_prep;
            const bool c_ali_prop = is_ali_proper_cand(t, c);
            const bool c_part_non_noun = (t.si.particle != PART_NONE && !c_ali_prop);
            const bool is_kana_v = (t.si.particle == PART_KANA) || is_kana_verb_word(ts, t.n);
            const bool is_invar = is_invar_long_vowel_end(ts, t.n, c.case_pos, c) ||
                                  (!(c.flags & CF_SUFFIX) && c.case_pos + 1 < t.n && ts[c.case_pos + 1] == S_ALEF);
            const bool is_idafa_decl = (t.si.particle == PART_IDAFA_NOUN && !(t.surf_flags & SF_FIVE_NOUN) && !is_invar);
            const bool is_decl_noun = (c.verb_pattern == 0) && (!c_part_non_noun || is_idafa_decl) && !is_invar &&
                                      !(t.surf_flags & (SF_BAAD_QABL | SF_AMS | SF_INNA_PRON | SF_FIVE_NOUN | SF_COMP_NUM | SF_ASHAR)) &&
                                      !t.digit_after && cf != FAM_NONE && cf != FAM_SUKUN;
            const bool is_bare_noun = is_decl_noun && !c_gen;
            const bool is_conj_tok = (t.si.particle == PART_CONJ);
            const bool has_attached_conj = (!c_part_non_noun && t.si.has_conj && !t.si.has_prep);
            const bool is_verb_cand = (c.verb_pattern == 1) && !is_kana_v;
            const bool is_fem_at = (c.ext_flags & EF_FEM_PL_AT) != 0;
            const bool is_dip = (c.ext_flags & EF_DIPTOTE) != 0;
            const bool is_dual_pl = (c.ext_flags & (EF_DUAL_NOON | EF_MASC_PL_NOON)) != 0;
            const bool is_nom_ok = (cf == FAM_DAMMA);
            const bool is_acc_ok = (!is_fem_at && cf == FAM_FATHA) || (is_fem_at && cf == FAM_KASRA) || ambig_obl;

            // 1. PP Chunk Closure Check (Solution 2): close active PP tail when syntax breaks PP
            const bool cur_is_coord = (st.conj_pending || has_attached_conj) && !is_verb_cand &&
                                      (!c_part_non_noun || t.si.particle == PART_IDAFA_NOUN);
            if (st.pp_state == 2) {
                bool stay_pp = false;
                if (st.idafa_state == 1 && !c_part_non_noun && !is_verb_cand && !c_gen) {
                    stay_pp = true;
                } else if (cur_is_coord && !c_gen && (st.head_np_pp || st.prev_np_pp) &&
                           (c_al == st.head_np_al || c_al == st.prev_np_al)) {
                    stay_pp = true;
                } else if (!c_part_non_noun && !is_verb_cand && !c_gen && !st.conj_pending && !has_attached_conj) {
                    if (c_al && st.prev_np_al) stay_pp = true;
                    else if (!c_al && st.prev_np_tan && (t.is_adj_like || t.is_ibn || (t.surf_flags & SF_NAME_HEAD))) stay_pp = true;
                    else if (!c_al && !st.prev_np_al && (t.is_ibn || c.morph_class == MC_NOUN_SHADDA_CORE)) stay_pp = true;
                }
                if (!stay_pp) {
                    st.pp_state = 0;
                }
            }

            // 2. Score Verbs (Mood expectation after ان / لن / لم / حتى) & أَنْ/أَنَّ & إِنْ/إِنَّ & مَنْ/مِنْ
            if (t.si.particle == PART_AN || t.si.particle == PART_LI_AN || t.si.particle == PART_IN) {
                const bool sh = diac_has_shadda(cd);
                const bool nx_past_pl = nx && nx->n >= 4 && ctx.syms[nx->off + nx->n - 2] == S_WAW &&
                                        ctx.syms[nx->off + nx->n - 1] == S_ALEF;
                const bool nx_verb = nx && (nx->top_verb == 1 || nx->top_pos == POS_VERB_PAST_SUBJ || nx_past_pl ||
                                            nx->si.verb_like == 2 ||
                                            (nx->si.verb_like == 1 && !nx->has_noun_lex) ||
                                            nx->si.particle == PART_LA || nx->si.particle == PART_JAZM ||
                                            nx->si.particle == PART_QAD ||
                                            (t.si.particle == PART_IN && (nx->si.particle == PART_KANA ||
                                             is_kana_verb_word(&ctx.syms[nx->off], nx->n))));
                if (nx_verb) {
                    if (!sh) syn += 1200;
                    else syn -= 1200;
                } else if (nx && ((nx->si.particle == PART_NONE && nx->si.verb_like == 0 && !(nx->surf_flags & SF_THEMMA)) ||
                                  (t.si.particle == PART_IN && (nx->si.particle == PART_PRON || nx->si.particle == PART_DEM ||
                                                                (nx->surf_flags & SF_INNA_PRON))))) {
                    if (sh) syn += 850;
                    else syn -= 650;
                }
            }
            if ((t.surf_flags & SF_MAN) && !(nx && (nx->surf_flags & SF_THEMMA)) &&
                t.n >= 2 && c.case_pos == t.n - 1) {
                const bool man_fatha = (c.diac[t.n - 2] == DIAC_FATHA);
                if (nx && (nx->si.particle == PART_PRON || (nx->top_verb == 1 && !nx->has_noun_lex))) {
                    if (man_fatha) syn += 1100;
                    else syn -= 1100;
                }
            }
            if ((is_verb_cand || is_kana_v) && starts_like_imperf(ts, t.n) && !c_al &&
                !(c.flags & (CF_SUFFIX | CF_LAM_NASB)) && c.case_pos == t.n - 1 && !is_invar) {
                size_t st_imp = (t.n >= 4 && (ts[0] == S_WAW || ts[0] == S_FEH)) ? 1 : 0;
                if (t.n - st_imp >= 4 && ts[st_imp] == S_SEEN) ++st_imp;
                if (!is_kana_v && (t.n - st_imp) >= 6 && ts[t.n - 2] == S_WAW && ts[t.n - 1] == S_NOON) {
                    if (cd == DIAC_FATHA) syn += 1200;
                    else syn -= 1500;
                } else if (st.mood_expect == FAM_FATHA) {
                    if (cf == FAM_FATHA) syn += 900;
                    else if (cf == FAM_DAMMA) syn -= 900;
                } else if (st.mood_expect == FAM_SUKUN) {
                    if (cf == FAM_SUKUN || cf == FAM_FATHA) syn += 700;
                    else if (cf == FAM_DAMMA) syn -= 1200;
                }
            }

            // 3. Score Declinable Nouns with Chunking, ClauseStack & Hard Negative Invariants
            const bool is_pp_after_prep = (st.pp_state == 1);
            const bool is_adj_agree = !st.conj_pending && !has_attached_conj && !is_pp_after_prep &&
                                      st.prev_np_fam != FAM_NONE && st.prev_np_fam != FAM_SUKUN &&
                                      ((c_al && st.prev_np_al) ||
                                       (!c_al && !(c.flags & CF_SUFFIX) && st.prev_np_tan && t.is_adj_like));
            const bool is_coord_agree = (st.conj_pending || has_attached_conj) &&
                                        st.head_np_fam != FAM_NONE && st.head_np_fam != FAM_SUKUN &&
                                        (c_al == st.head_np_al || (!c_al && nx && nx->si.al_len > 0) ||
                                         (t.boundary_before && (t.bkind_mask & BK_COMMA) && has_attached_conj && ts[0] == S_WAW));

            const bool foreign_sukun_noun = (cf == FAM_SUKUN && c.verb_pattern == 0 && !c_part_non_noun &&
                                             t.si.al_len == 0 && !(c.flags & CF_EXPANDED));
            const bool is_elative_dip = (c.verb_pattern == 2 && (c.ext_flags & EF_DIPTOTE) &&
                                         (st.pp_state > 0 || st.idafa_state == 1 || c_gen));
            if (is_decl_noun || is_elative_dip) {
                const bool gen_licensed = c_gen || (st.pp_state > 0) || (st.idafa_state == 1) ||
                                          (st.prev_np_fam == FAM_KASRA) ||
                                          (is_coord_agree && st.head_np_fam == FAM_KASRA) ||
                                          (t.is_ibn && st.prev_np_fam == FAM_KASRA) ||
                                          (pt && pt->digit_after && (pt->num_after_class == 2 || pt->num_after_class == 4)) ||
                                          (st.last_was_title && st.prev_np_fam == FAM_KASRA) ||
                                          (pt && pt->si.particle == PART_AY && pc && diac_has_shadda(pc->diac[pt->n - 1]));

                // Hard Negative Invariant (Solution 3): Block unlicensed dangling KASRA (especially at EOS!)
                const bool oov_sukun = ((c.flags & CF_OOV) && cf == FAM_SUKUN);
                if (!oov_sukun && !gen_licensed && !is_fem_at && !is_dual_pl && !(t.surf_flags & SF_AMS) && cf == FAM_KASRA) {
                    syn -= (is_seg_end ? 1900 : 1550);
                }

                // Direct preposition / adverbial idafa / proclitic prep government
                if (c_gen || is_pp_after_prep) {
                    if (oov_sukun || foreign_sukun_noun) {
                        // Preserve lexical/OOV sukun on foreign loanwords inside PP (في هُونْغْ كُونْغْ)
                    } else if (is_dip && !c_al && !is_dual_pl) {
                        const bool proper_dip = (t.is_diptote_lex == 2) ||
                                                (classify_diptote_stem(ts, t.n, t.si.al_len, c.diac) == 2);
                        const bool afal_super = (t.n == t.si.al_len + 4 && ts[t.si.al_len] == S_ALEF_HAMZA_A);
                        const bool nx_coord_mudaf = afal_super && nx &&
                                                    (nx->is_adj_like ||
                                                     ((ctx.syms[nx->off] == S_WAW || ctx.syms[nx->off] == S_FEH) &&
                                                      nx->n == 5 && ctx.syms[nx->off + 1] == S_ALEF_HAMZA_A));
                        const bool nx_mudaf = !proper_dip &&
                                              (nx_coord_mudaf ||
                                               (nx && nx->si.particle == PART_NONE &&
                                                nx->si.verb_like < 2 && nx->top_verb != 1 &&
                                                !nx->is_adj_like && !nx->is_ibn && !nx->si.has_prep));
                        if (nx_mudaf) {
                            if (cf == FAM_KASRA) syn += 1200;
                            else syn -= 1000;
                        } else {
                            if (cf == FAM_FATHA && !tan) syn += 1250;
                            else syn -= 1100;
                        }
                    } else if (!is_dual_pl) {
                        if (cf == FAM_KASRA) syn += 1300;
                        else syn -= 1800;
                    }
                } else if (st.idafa_state == 1) {
                    if (oov_sukun) {
                        // Preserve sukun on foreign proper compounds
                    } else if (is_dip && !c_al && !is_dual_pl) {
                        const bool proper_dip = (t.is_diptote_lex == 2) ||
                                                (classify_diptote_stem(ts, t.n, t.si.al_len, c.diac) == 2);
                        if (proper_dip || !nx || nx->si.particle != PART_NONE) {
                            if (cf == FAM_FATHA && !tan) syn += 950;
                        } else {
                            if (cf == FAM_KASRA) syn += 950;
                        }
                    } else if (!is_dual_pl) {
                        if (cf == FAM_KASRA) syn += 950;
                        else if (cf == FAM_DAMMA) syn -= 650;
                    }
                } else if (is_coord_agree && st.pp_state == 0) {
                    // Solution 5: Coordinated noun inherits head_np_fam rather than local PP drift
                    if (cf == st.head_np_fam) syn += 850;
                } else if (is_adj_agree) {
                    if (cf == st.prev_np_fam) syn += 750;
                } else if (!is_pp_after_prep && st.pp_state == 2 && cf == FAM_KASRA && !oov_sukun) {
                    syn += 650;
                } else if (is_bare_noun && st.pp_state == 0) {
                    // ClauseFrame Argument Completion (Solution 1: outer governor preserved across PP!)
                    const uint8_t fk = st.kind[st.depth];
                    const uint8_t fs = st.stage[st.depth];
                    const bool time_adv = is_time_adverb_noun(ts, t.n, t.si.al_len);
                    const bool nx_ina = nx && is_ina_clause_word(&ctx.syms[nx->off], nx->n, nx->si.particle, nx->surf_flags);
                    const bool subj_cue = nx_ina || is_title_noun(ts, t.n, t.si.al_len) ||
                                          (nx && (nx->surf_flags & SF_NAME_HEAD));

                    const bool use_val = is_v7_ && valency_.valid() && v7_weights_[V7W_VALENCY] != 0;
                    const int32_t val_w = use_val ? v7_weights_[V7W_VALENCY] : 0;
                    const ValencyInfo val_cur = use_val ? get_valency(t.word_id) : ValencyInfo{};
                    const bool lex_adv = time_adv || (use_val && val_cur.word_syn_type == 3);

                    if (fk == FRM_VERB_ACT && fs == 0 && !lex_adv) {
                        const uint8_t gv = st.gov_vtrans[st.depth];
                        const int8_t gvo = st.gov_vodds[st.depth];
                        if (is_nom_ok) {
                            syn += subj_cue ? 1250 : 650;
                            if (use_val && gv == 1) syn += val_w;
                            else if (use_val && gv == 2 && gvo >= 3 && !subj_cue) syn -= val_w / 2;
                        } else if (is_acc_ok) {
                            if (subj_cue) syn -= 850;
                            if (use_val && gv == 1) syn -= val_w;
                            else if (use_val && gv == 2 && gvo >= 3 && !subj_cue) syn += (val_w * gvo) / 7;
                        }
                    } else if ((fk == FRM_VERB_ACT && fs == 1) || fk == FRM_VERB_HAVE) {
                        const bool after_prep_pron = pt && pt->si.particle == PART_PREP_PRON && is_fem_at && !c_al;
                        const uint8_t gv = st.gov_vtrans[st.depth];
                        const int8_t gvo = st.gov_vodds[st.depth];
                        if (after_prep_pron) {
                            if (is_nom_ok) syn += 700;
                        } else if (use_val && gv == 1) {
                            // Intransitive verb (لازم): suppress spurious ACC object force after subject!
                            if (is_acc_ok && !lex_adv) syn -= val_w / 2;
                            else if (is_nom_ok && !lex_adv) syn += val_w / 2;
                        } else {
                            if (is_acc_ok) {
                                syn += 750;
                                if (use_val && gv == 2 && gvo > 0) syn += (val_w * gvo) / 7;
                            } else if (is_nom_ok && !lex_adv) {
                                syn -= 350;
                                if (use_val && gv == 2 && gvo >= 2) syn -= val_w / 2;
                            }
                        }
                    } else if (fk == FRM_VERB_PASS && fs == 0 && !time_adv &&
                               c.verb_pattern != 2 && !(t.n == 4 && ts[0] == S_ALEF_HAMZA_A && !c_al)) {
                        if (is_nom_ok) syn += 1100;
                        else if (is_acc_ok) syn -= 850;
                    } else if (fk == FRM_INNA) {
                        if (fs == 0) {
                            if (is_acc_ok) syn += 1250;
                            else if (is_nom_ok) syn -= 800;
                        } else if (fs == 1) {
                            if (is_nom_ok) syn += 1050;
                            else if (is_acc_ok && !lex_adv) syn -= 450;
                        }
                    } else if (fk == FRM_KANA) {
                        if (fs == 0) {
                            const bool end_khabar_only = !c_al && (!nx || (nx->si.particle != PART_NONE &&
                                                                           nx->si.particle != PART_IDAFA_NOUN &&
                                                                           !nx->digit_after));
                            if (end_khabar_only) {
                                if (is_acc_ok) syn += 950;
                                else if (is_nom_ok) syn += 150;
                            } else {
                                if (is_nom_ok) syn += 1050;
                                else if (is_acc_ok) syn += 100;
                            }
                        } else if (fs == 1) {
                            if (is_acc_ok) syn += 1100;
                            else if (is_nom_ok && !lex_adv) syn -= 550;
                        }
                    } else if (fk == FRM_NOM && !lex_adv) {
                        const bool after_innama = pt && is_innama_word(&ctx.syms[pt->off], pt->n);
                        if (fs == 0 && (idx == 0 || after_innama) && !is_masdar_prep_adv(ts, t.n)) {
                            if (is_nom_ok) syn += 850;
                            else if (is_acc_ok) syn -= 700;
                        } else if (fs == 1) {
                            if (is_nom_ok) syn += (!c_al ? 950 : 750);
                            else if (is_acc_ok && !c_al && t.is_adj_like) syn -= 500;
                        }
                    }
                    if (use_val && val_cur.word_syn_type == 3 && is_acc_ok) {
                        syn += val_w;
                    }
                    if (use_val && pt && pt->word_id < valency_.count() && pt->si.particle == PART_NONE) {
                        ValencyInfo vpt = get_valency(pt->word_id);
                        if (vpt.next_case_pref != 0) {
                            if (vpt.next_case_pref == FAM_DAMMA && is_nom_ok) syn += (val_w * vpt.next_nom_p) / 24;
                            else if (vpt.next_case_pref == FAM_FATHA && is_acc_ok) syn += (val_w * vpt.next_acc_p) / 24;
                            else if (vpt.next_case_pref == FAM_KASRA && cf == FAM_KASRA) syn += (val_w * vpt.next_gen_p) / 24;
                        }
                    }
                }

                // Prevent spurious Construct (tanween drop) unless a real Mudaf Ilayh can follow
                if (!c_al && !(c.flags & CF_SUFFIX) && !is_dip && !is_fem_at && !is_dual_pl &&
                    !tan && !(t.surf_flags & SF_NAME_HEAD) && !t.digit_after &&
                    st.idafa_state == 0 && t.si.particle == PART_NONE) {
                    const bool can_have_mudaf_ilayh = nx && (nx->si.particle == PART_NONE ||
                                                             nx->si.particle == PART_IDAFA_NOUN ||
                                                             nx->si.particle == PART_NUM_IDAFA ||
                                                             nx->si.particle == PART_DEM ||
                                                             (nx->surf_flags & SF_ALI)) &&
                                                      !(nx->surf_flags & SF_INNA_PRON) &&
                                                      !nx->si.has_prep && nx->top_verb != 1 && nx->si.verb_like < 2;
                    if (!can_have_mudaf_ilayh) {
                        syn -= 650;
                    }
                }
            } else if (st.idafa_state == 1 && !foreign_sukun_noun && !c_part_non_noun && !is_verb_cand && is_invar) {
                // Invariable mudaf ilayh consumes construct
            }

            const bool foreign_sukun_keep = foreign_sukun_noun || ((c.flags & CF_OOV) && cf == FAM_SUKUN);
            if (foreign_sukun_keep) {
                // Keep lexical/OOV sukun foreign words out of construct chains (هُونْغْ كُونْغْ)
            } else {
                // Preserve invar mudaf opening below
            }

            // 4. Advance SynState cleanly
            const bool is_dem_before_al = (t.si.particle == PART_DEM && is_dem_det_word(ts, t.n) &&
                                           nx && nx->si.al_len > 0 && !nx->si.has_prep);
            if (!is_dem_before_al) st.idafa_state = 0;
            if (!is_conj_tok) st.conj_pending = 0;

            if (is_kana_v) {
                if (st.kind[st.depth] == FRM_INNA && st.stage[st.depth] == 1 && st.depth < 3) {
                    ++st.depth;
                }
                st.kind[st.depth] = FRM_KANA;
                st.stage[st.depth] = (c.pos_tag == POS_VERB_PAST_SUBJ || c.pos_tag == POS_VERB_IMPERF_12) ? 1 : 0;
                st.pp_state = 0;
                st.mudaf_head_fam = FAM_NONE;
                st.prev_np_fam = FAM_NONE;
                st.head_np_fam = FAM_NONE;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (is_verb_cand) {
                const bool prev_nom_subj = (st.prev_np_fam == FAM_DAMMA && !st.prev_np_pp && pt && !pt->digit_after) ||
                                           (st.kind[st.depth] == FRM_NOM && st.stage[st.depth] >= 1) ||
                                           (st.kind[st.depth] == FRM_INNA && st.stage[st.depth] == 1) ||
                                           (st.kind[st.depth] == FRM_KANA && st.stage[st.depth] == 1);
                const bool svo_after_nom = prev_nom_subj && starts_like_imperf(ts, t.n);
                const bool embed_for_pred = (st.depth < 3) &&
                    ((st.kind[st.depth] == FRM_INNA && st.stage[st.depth] == 1) ||
                     (st.kind[st.depth] == FRM_KANA && st.stage[st.depth] == 1) ||
                     (st.kind[st.depth] == FRM_NOM && st.stage[st.depth] == 1));
                if (embed_for_pred) ++st.depth;
                if (c.pos_tag == POS_VERB_PASS) {
                    st.kind[st.depth] = FRM_VERB_PASS;
                    st.stage[st.depth] = 0;
                    st.gov_vtrans[st.depth] = 0;
                    st.gov_vodds[st.depth] = 0;
                } else if (c.pos_tag == POS_VERB_PAST_SUBJ || c.pos_tag == POS_VERB_IMPERF_12 ||
                           has_verb_obj_suffix(ts, t.n, c) || svo_after_nom) {
                    st.kind[st.depth] = FRM_VERB_HAVE;
                    st.stage[st.depth] = 1;
                    ValencyInfo vi = is_v7_ ? get_valency(t.word_id) : ValencyInfo{};
                    st.gov_vtrans[st.depth] = vi.v_trans;
                    st.gov_vodds[st.depth] = vi.val_odds;
                } else {
                    st.kind[st.depth] = FRM_VERB_ACT;
                    st.stage[st.depth] = 0;
                    ValencyInfo vi = is_v7_ ? get_valency(t.word_id) : ValencyInfo{};
                    st.gov_vtrans[st.depth] = vi.v_trans;
                    st.gov_vodds[st.depth] = vi.val_odds;
                }
                st.pp_state = 0;
                st.mudaf_head_fam = FAM_NONE;
                st.prev_np_fam = FAM_NONE;
                st.head_np_fam = FAM_NONE;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (t.si.particle == PART_INNA ||
                       ((t.si.particle == PART_AN || t.si.particle == PART_LI_AN || t.si.particle == PART_LAKIN) &&
                        diac_has_shadda(cd)) ||
                       (t.si.particle == PART_IN && diac_has_shadda(cd))) {
                if (st.depth < 3 && st.kind[st.depth] != FRM_REL &&
                    (st.kind[st.depth] == FRM_VERB_ACT || st.kind[st.depth] == FRM_VERB_HAVE)) {
                    ++st.depth;
                }
                st.kind[st.depth] = FRM_INNA;
                st.stage[st.depth] = 0;
                st.pp_state = 0;
                st.mudaf_head_fam = FAM_NONE;
                st.prev_np_fam = FAM_NONE;
                st.head_np_fam = FAM_NONE;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (t.si.particle == PART_REL || t.si.particle == PART_COND) {
                if (st.depth < 3) ++st.depth;
                st.kind[st.depth] = FRM_REL;
                st.stage[st.depth] = 0;
                st.pp_state = 0;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (t.si.particle == PART_GEN) {
                st.pp_state = 1;
                st.prev_np_fam = FAM_NONE;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (t.si.particle == PART_PREP_PRON) {
                st.pp_state = 0;
                st.prev_np_fam = FAM_NONE;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (t.si.particle == PART_HATTA) {
                const bool nx_noun_hatta = nx && nx->si.particle == PART_NONE && nx->top_verb != 1 && nx->si.verb_like < 2;
                if (nx_noun_hatta) {
                    st.pp_state = 1;
                    st.prev_np_fam = FAM_NONE;
                    st.mood_expect = FAM_NONE;
                } else {
                    st.mood_expect = FAM_FATHA;
                }
                st.last_was_title = 0;
            } else if (t.si.particle == PART_NASB ||
                       ((t.si.particle == PART_AN || t.si.particle == PART_LI_AN) && !diac_has_shadda(cd))) {
                st.mood_expect = FAM_FATHA;
                st.last_was_title = 0;
            } else if (t.si.particle == PART_JAZM ||
                       (t.si.particle == PART_IN && !diac_has_shadda(cd))) {
                st.mood_expect = FAM_SUKUN;
                st.last_was_title = 0;
            } else if (is_conj_tok) {
                st.conj_pending = 1;
                st.prev_np_fam = FAM_NONE;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (t.si.particle == PART_DEM && is_dem_before_al) {
                // Propagate pp_state, idafa_state, and ClauseFrame through هذا/هذه/ذلك/تلك to following AL-noun Badal
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (t.si.particle == PART_PRON) {
                if (st.pp_state == 0 && st.kind[st.depth] == FRM_NOM && st.stage[st.depth] == 0) {
                    st.stage[st.depth] = 1;
                }
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (is_innama_word(ts, t.n)) {
                st.kind[st.depth] = FRM_NOM;
                st.stage[st.depth] = 0;
                st.pp_state = 0;
                st.prev_np_fam = FAM_NONE;
                st.head_np_fam = FAM_NONE;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if ((t.si.particle == PART_IDAFA_NOUN && !is_idafa_decl) || t.si.particle == PART_NUM_IDAFA) {
                const bool is_comp_11_19 = (t.si.particle == PART_NUM_IDAFA && (t.surf_flags & SF_ASHAR) &&
                                            pt && (pt->surf_flags & SF_COMP_NUM));
                if (!is_comp_11_19) {
                    st.idafa_state = 1;
                    if (st.pp_state == 1) st.pp_state = 2;
                    if (cf != FAM_NONE && cf != FAM_SUKUN && cf != FAM_KASRA) st.mudaf_head_fam = cf;
                }
                st.last_was_title = 0;
            } else if (t.surf_flags & SF_INNA_PRON) {
                if (st.depth < 3 && st.kind[st.depth] == FRM_VERB_ACT) ++st.depth;
                st.kind[st.depth] = FRM_INNA;
                st.stage[st.depth] = 1;
                st.pp_state = 0;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (!c_part_non_noun || is_idafa_decl) {
                // Nominal progression
                if (c_gen) st.pp_state = 2;
                else if (st.pp_state == 1) st.pp_state = 2;

                if (st.pp_state == 0 && !is_adj_agree && !is_coord_agree) {
                    const uint8_t fk = st.kind[st.depth];
                    const uint8_t fs = st.stage[st.depth];
                    if (fk == FRM_VERB_ACT && fs == 0 && is_nom_ok && !is_time_adverb_noun(ts, t.n, t.si.al_len)) {
                        st.stage[st.depth] = 1;
                    } else if (fk == FRM_VERB_PASS && fs == 0 && is_nom_ok) {
                        st.stage[st.depth] = 1;
                    } else if (fk == FRM_INNA && fs == 0 && is_acc_ok) {
                        st.stage[st.depth] = 1;
                    } else if (fk == FRM_INNA && fs == 1 && is_nom_ok) {
                        st.stage[st.depth] = 2;
                    } else if (fk == FRM_KANA && fs == 0 && is_nom_ok) {
                        st.stage[st.depth] = 1;
                    } else if (fk == FRM_KANA && fs == 1 && is_acc_ok) {
                        st.stage[st.depth] = 2;
                    } else if (fk == FRM_NOM && fs == 0 && is_nom_ok) {
                        st.stage[st.depth] = 1;
                    } else if (fk == FRM_NOM && fs == 1 && is_nom_ok && !c_al) {
                        st.stage[st.depth] = 2;
                    }
                }

                // Open Idafa construct head for next token if non-al, non-suffix, non-tanween, non-proper-dip
                const bool is_proper_dip = (t.is_diptote_lex == 2);
                const bool idafa_has_arg = nx && (nx->si.particle == PART_NONE || nx->si.particle == PART_DEM ||
                                                  nx->si.particle == PART_IDAFA_NOUN || nx->si.particle == PART_NUM_IDAFA);
                const bool can_open_idafa = (!c_al && !(c.flags & CF_SUFFIX) && !tan && !is_proper_dip &&
                                             !(t.surf_flags & (SF_AMS | SF_THEMMA | SF_ASHAR)) &&
                                             !is_innama_word(ts, t.n) && !is_decade_num_word(ts, t.n) &&
                                             !((c.flags & CF_OOV) && cf == FAM_SUKUN) &&
                                             !(cf == FAM_SUKUN && t.si.al_len == 0 && !(c.flags & CF_EXPANDED)) &&
                                             !(c.ext_flags & (EF_DUAL_NOON | EF_MASC_PL_NOON)) && !t.digit_after) ||
                                            (is_idafa_decl && !tan && idafa_has_arg);
                if (can_open_idafa) st.idafa_state = 1;

                const uint8_t eff_track_fam = ((is_invar && (st.pp_state > 0 || st.idafa_state == 1)) ||
                                               ((cf == FAM_NONE || cf == FAM_SUKUN) && st.pp_state > 0))
                                              ? static_cast<uint8_t>(FAM_KASRA) : cf;
                if (st.prev_np_al == 0 && (c_al || is_invar) && st.prev_np_fam != FAM_NONE &&
                    st.prev_np_fam != FAM_KASRA && eff_track_fam == FAM_KASRA) {
                    st.mudaf_head_fam = st.prev_np_fam;
                } else if (!c_al && !is_invar) {
                    st.mudaf_head_fam = FAM_NONE;
                }
                if (st.prev_np_fam == FAM_NONE || !is_adj_agree) {
                    if (!st.conj_pending && !has_attached_conj) {
                        st.head_np_fam = eff_track_fam;
                        st.head_np_al = c_al ? 1 : 0;
                        st.head_np_pp = (st.pp_state > 0) ? 1 : 0;
                    }
                }
                st.prev_np_fam = eff_track_fam;
                st.prev_np_al = c_al ? 1 : 0;
                st.prev_np_tan = tan ? 1 : 0;
                st.prev_np_adj = t.is_adj_like;
                st.prev_np_suf = (c.flags & CF_SUFFIX) ? 1 : 0;
                st.prev_np_pp = (st.pp_state > 0) ? 1 : 0;
                st.last_was_title = is_title_noun(ts, t.n, t.si.al_len) ? 1 : 0;
                st.mood_expect = FAM_NONE;
            } else {
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            }

            // Allow invariable construct heads (أعلى بناء، قاضي المحكمة، منتدى الشباب) to open idafa_state = 1 cleanly
            const bool is_afal_maksura = (t.n == 4 && ts[0] == S_ALEF_HAMZA_A && ts[3] == S_ALEF_MAKSURA &&
                                          !(c.flags & CF_HAS_AL));
            if ((!c_part_non_noun && !is_verb_cand && is_invar && !c_al && !(c.flags & CF_SUFFIX) &&
                 !tan && t.is_diptote_lex != 2 && !t.digit_after) ||
                (is_afal_maksura && nx && nx->si.particle == PART_NONE && !nx->si.has_prep)) {
                st.idafa_state = 1;
            }

            if (is_seg_end && (st.kind[st.depth] == FRM_INNA || st.kind[st.depth] == FRM_KANA) &&
                st.stage[st.depth] >= 2) {
                syn += 250;
            }

            st.last_role = has_v5_24roles_ ? cand_role_v5(t, c) : t.role;
            st.last_end = cd;
            return syn;
        };

        auto run_case_beam = [&](const std::vector<int32_t>* extra_bonus, int32_t syn_scale) {
            const bool use_tr3 = is_v7_ && trans3_ && trans3_size_ == TRANS3_TABLE_SIZE && v7_weights_[V7W_TRANS3] != 0;
            const int32_t w_tr3 = use_tr3 ? v7_weights_[V7W_TRANS3] : 0;
            uint8_t prev_seg_head_fam = FAM_NONE;
            uint8_t prev_seg_head_al = 0;
            uint8_t prev_seg_head_pp = 0;
            size_t seg_s = 0;
            while (seg_s < W) {
                size_t seg_e = seg_s + 1;
                while (seg_e < W && !ctx.toks[seg_e].boundary_before) ++seg_e;

                const Token& seg_t0 = ctx.toks[seg_s];
                const bool carry_comma_coord = (seg_s > 0) && (seg_t0.bkind_mask & BK_COMMA) &&
                                               !(seg_t0.bkind_mask & BK_HARD) &&
                                               prev_seg_head_fam != FAM_NONE && prev_seg_head_fam != FAM_SUKUN &&
                                               ((seg_t0.si.has_conj && !seg_t0.si.has_prep && ctx.syms[seg_t0.off] == S_WAW) ||
                                                seg_t0.si.particle == PART_CONJ);
                if (seg_t0.bkind_mask & BK_HARD) {
                    prev_seg_head_fam = FAM_NONE;
                    prev_seg_head_al = 0;
                    prev_seg_head_pp = 0;
                }

                // Run beam search on segment (>=2 words, or comma-coordinated single word, or Pass-2 irab2 rescore)
                if (seg_e - seg_s >= 2 || carry_comma_coord || (extra_bonus != nullptr && seg_e > seg_s)) {
                    beam_cur.clear();
                    beam_bp.clear();
                    BeamHyp init;
                    if (carry_comma_coord) {
                        init.st.head_np_fam = prev_seg_head_fam;
                        init.st.head_np_al = prev_seg_head_al;
                        init.st.head_np_pp = prev_seg_head_pp;
                        if (seg_t0.si.has_conj) init.st.conj_pending = 1;
                    }
                    beam_cur.push_back(init);

                    for (size_t i = seg_s; i < seg_e; ++i) {
                        const Token& t = ctx.toks[i];
                        if (t.cand_count == 0) continue;
                        const Token* pt = (i > seg_s) ? &ctx.toks[i - 1] : nullptr;
                        const Token* p2 = (i > seg_s + 1) ? &ctx.toks[i - 2] : nullptr;
                        const Token* p3 = (i > seg_s + 2) ? &ctx.toks[i - 3] : nullptr;
                        const Token* nx = (i + 1 < seg_e) ? &ctx.toks[i + 1] : nullptr;
                        const bool is_seg_end = (i + 1 == seg_e);
                        const uint32_t base_k = chosen[i];
                        const Cand& base_c = ctx.cands[t.cand_begin + base_k];
                        const size_t lam_skip = (t.si.al_len >= 2) ? static_cast<size_t>(t.si.al_lam_pos) : static_cast<size_t>(-1);

                        opts.clear();
                        opts.push_back(base_k);
                        const bool base_foreign_sukun = (!(base_c.flags & CF_EXPANDED) &&
                                                         diac_family(base_c.diac[base_c.case_pos]) == FAM_SUKUN &&
                                                         base_c.verb_pattern == 0 && t.si.particle == PART_NONE && t.si.al_len == 0);
                        if (t.cand_count >= 2 && !base_foreign_sukun) {
                            for (uint32_t k = 0; k < t.cand_count && opts.size() < 8; ++k) {
                                if (k == base_k) continue;
                                const Cand& ak = ctx.cands[t.cand_begin + k];
                                if (ak.emis + 4200 < base_c.emis) continue;
                                bool same_core = (ak.case_pos == base_c.case_pos);
                                for (size_t p = 0; same_core && p < base_c.case_pos; ++p) {
                                    if (p != lam_skip && ak.diac[p] != base_c.diac[p]) same_core = false;
                                }
                                const bool part_alt = (t.si.particle != PART_NONE) || ((t.surf_flags & (SF_ALI | SF_MAN)) != 0);
                                const bool fem_past_alt = (t.n >= 5 && ctx.syms[t.off + t.n - 1] == S_TEH &&
                                                           ak.verb_pattern == 1 && base_c.verb_pattern == 1 &&
                                                           ((ak.ext_flags & EF_PAST_TEH_FEM) != (base_c.ext_flags & EF_PAST_TEH_FEM)) &&
                                                           ak.diac[t.n - 1] == DIAC_SUKUN);
                                const bool is_past_fem_sukun_var = (base_c.verb_pattern == 1 && (base_c.ext_flags & EF_PAST_TEH_FEM) &&
                                                                    (ak.ext_flags & EF_PAST_TEH_FEM));
                                const bool is_imperf_noon_var = (base_c.verb_pattern == 1 && t.n >= 5 &&
                                                                 ctx.syms[t.off + t.n - 2] == S_WAW && ctx.syms[t.off + t.n - 1] == S_NOON &&
                                                                 starts_like_imperf(&ctx.syms[t.off], t.n));
                                if ((same_core && !is_past_fem_sukun_var && !is_imperf_noon_var) || part_alt || fem_past_alt) {
                                    opts.push_back(k);
                                }
                            }
                        }

                        beam_nxt.clear();
                        const uint8_t* ts_i = &ctx.syms[t.off];
                        for (const BeamHyp& h : beam_cur) {
                            for (uint32_t ok : opts) {
                                const Cand& cc = ctx.cands[t.cand_begin + ok];
                                const uint8_t ce = cc.diac[cc.case_pos];
                                const uint8_t cr = has_v5_24roles_ ? cand_role_v5(t, cc) : t.role;
                                int32_t trans_v = 0;
                                int32_t bin_v = 0;
                                if (!pt) {
                                    trans_v = static_cast<int32_t>((static_cast<int64_t>(trans_score(0, DIAC_NONE, cr, ce)) * w_tr) / 1000);
                                } else {
                                    const Cand& pc = ctx.cands[pt->cand_begin + h.cand_k];
                                    trans_v = static_cast<int32_t>((static_cast<int64_t>(trans_score(h.st.last_role, h.st.last_end, cr, ce)) * w_tr) / 1000);
                                    bin_v = binary_rules(ctx, p3, p2, *pt, pc, t, cc, nx);
                                }
                                SynState nst = h.st;
                                int32_t syn_v = step_syn(nst, i, t, cc, pt, pt ? &ctx.cands[pt->cand_begin + h.cand_k] : nullptr, nx, is_seg_end);
                                if (syn_scale != 1000) {
                                    syn_v = static_cast<int32_t>((static_cast<int64_t>(syn_v) * syn_scale) / 1000);
                                }
                                int32_t tr3_v = 0;
                                if (use_tr3) {
                                    uint8_t cur_st3 = cand_state3_v7(t, cc, ts_i);
                                    tr3_v = static_cast<int32_t>((static_cast<int64_t>(trans3_score(h.st.prev2_st3, h.st.prev1_st3, cur_st3)) * w_tr3) / 1000);
                                    nst.prev2_st3 = h.st.prev1_st3;
                                    nst.prev1_st3 = cur_st3;
                                }
                                int32_t ex_v = extra_bonus ? (*extra_bonus)[dp_off[i] + ok] : 0;
                                int32_t total = h.score + cc.emis + ex_v + dp_bg[dp_off[i] + ok] + trans_v + tr3_v + bin_v + syn_v;

                                uint32_t bp_id = static_cast<uint32_t>(beam_bp.size());
                                beam_bp.push_back({h.prev_node, ok});
                                BeamHyp nh;
                                nh.st = nst;
                                nh.score = total;
                                nh.prev_node = bp_id;
                                nh.cand_k = ok;
                                beam_nxt.push_back(nh);
                            }
                        }

                        if (beam_nxt.size() > BEAM_W) {
                            std::sort(beam_nxt.begin(), beam_nxt.end(),
                                      [](const BeamHyp& a, const BeamHyp& b) { return a.score > b.score; });
                            beam_cur.clear();
                            for (const BeamHyp& cnd : beam_nxt) {
                                bool dup_state = false;
                                for (const BeamHyp& kept : beam_cur) {
                                    if (kept.st.depth == cnd.st.depth &&
                                        kept.st.kind[kept.st.depth] == cnd.st.kind[cnd.st.depth] &&
                                        kept.st.stage[kept.st.depth] == cnd.st.stage[cnd.st.depth] &&
                                        kept.st.pp_state == cnd.st.pp_state &&
                                        kept.st.idafa_state == cnd.st.idafa_state &&
                                        kept.st.prev_np_fam == cnd.st.prev_np_fam &&
                                        kept.st.head_np_fam == cnd.st.head_np_fam &&
                                        kept.st.last_end == cnd.st.last_end) {
                                        dup_state = true;
                                        break;
                                    }
                                }
                                if (!dup_state) {
                                    beam_cur.push_back(cnd);
                                    if (beam_cur.size() == BEAM_W) break;
                                }
                            }
                        } else {
                            beam_cur.swap(beam_nxt);
                        }
                    }

                    if (!beam_cur.empty()) {
                        size_t best_bi = 0;
                        for (size_t b = 1; b < beam_cur.size(); ++b) {
                            if (beam_cur[b].score > beam_cur[best_bi].score) best_bi = b;
                        }
                        prev_seg_head_fam = beam_cur[best_bi].st.head_np_fam;
                        prev_seg_head_al = beam_cur[best_bi].st.head_np_al;
                        prev_seg_head_pp = beam_cur[best_bi].st.head_np_pp;
                        uint32_t cur_node = beam_cur[best_bi].prev_node;
                        for (size_t i = seg_e; i > seg_s; --i) {
                            if (ctx.toks[i - 1].cand_count == 0) continue;
                            if (cur_node == UINT32_MAX) break;
                            chosen[i - 1] = beam_bp[cur_node].cand_k;
                            cur_node = beam_bp[cur_node].prev_node;
                        }
                    }
                } else if (seg_e == seg_s + 1 && ctx.toks[seg_s].cand_count > 0) {
                    const Token& st1 = ctx.toks[seg_s];
                    const Cand& sc1 = ctx.cands[st1.cand_begin + chosen[seg_s]];
                    uint8_t f1 = eff_case_fam(&ctx.syms[st1.off], sc1);
                    if (sc1.verb_pattern == 0 && (st1.si.particle == PART_NONE || st1.si.particle == PART_IDAFA_NOUN) &&
                        f1 != FAM_NONE && f1 != FAM_SUKUN) {
                        prev_seg_head_fam = f1;
                        prev_seg_head_al = ((sc1.flags & CF_HAS_AL) || st1.si.al_len > 0) ? 1 : 0;
                        prev_seg_head_pp = ((sc1.flags & CF_GEN) || st1.si.has_prep) ? 1 : 0;
                    } else {
                        prev_seg_head_fam = FAM_NONE;
                    }
                }
                seg_s = seg_e;
            }
        };

        // Pass-1 Beam Search
        run_case_beam(nullptr, 1000);

        // Pass-2 Stacked Bidirectional Irab2-GBDT + Case-Lattice Beam Re-Decoding (V7)
        if (dump_out != nullptr || (is_v7_ && irab2_trees_.valid() && v7_weights_[V7W_IRAB2] != 0)) {
            auto clamp_f = [](int32_t v) -> int32_t { return v < -8 ? -8 : (v > 119 ? 119 : v); };
            auto is_bare_fem_p2 = [&](const Token& tk) -> bool {
                const uint8_t* ws = &ctx.syms[tk.off];
                if (tk.n > tk.si.al_len + 1 && ws[tk.n - 1] == S_TEH_MARBUTA) return true;
                if (is_v7_ && valency_.valid() && tk.word_id < valency_.count() && get_valency(tk.word_id).is_gram_fem) return true;
                return false;
            };

            // Precompute Pass-1 per-token decoded properties
            std::vector<uint8_t> p1_end(W, 0), p1_fam(W, 0), p1_tan(W, 0), p1_pos(W, 0), p1_role(W, 0);
            std::vector<uint8_t> p1_al(W, 0), p1_gen(W, 0), p1_suf(W, 0), p1_verb(W, 0), p1_fem(W, 0);
            std::vector<uint8_t> p1_st3(W, 0), p1_multi(W, 0);
            for (size_t i = 0; i < W; ++i) {
                const Token& tk = ctx.toks[i];
                if (tk.cand_count == 0) continue;
                const Cand& c = ctx.cands[tk.cand_begin + chosen[i]];
                const uint8_t* ts = &ctx.syms[tk.off];
                uint8_t ce = c.diac[c.case_pos];
                p1_end[i] = ce;
                p1_fam[i] = eff_case_fam(ts, c);
                p1_tan[i] = diac_is_tanween(ce) ? 1 : 0;
                p1_pos[i] = c.pos_tag;
                p1_role[i] = has_v5_24roles_ ? cand_role_v5(tk, c) : tk.role;
                p1_al[i] = ((c.flags & CF_HAS_AL) || tk.si.al_len > 0) ? 1 : 0;
                p1_gen[i] = ((c.flags & CF_GEN) || tk.si.has_prep) ? 1 : 0;
                p1_suf[i] = (c.flags & CF_SUFFIX) ? 1 : 0;
                p1_verb[i] = (c.verb_pattern == 1) ? 1 : 0;
                p1_fem[i] = is_bare_fem_p2(tk) ? 1 : 0;
                p1_st3[i] = cand_state3_v7(tk, c, ts);
                uint8_t fe = ctx.cands[tk.cand_begin].diac[ctx.cands[tk.cand_begin].case_pos];
                for (uint32_t k = 1; k < tk.cand_count; ++k) {
                    const Cand& ck = ctx.cands[tk.cand_begin + k];
                    if (ck.diac[ck.case_pos] != fe) { p1_multi[i] = 1; break; }
                }
            }

            // Forward O(W) hierarchical ClauseFrame scan over Pass-1 output
            struct P2Frame {
                uint8_t gov = 0;
                uint8_t pp_depth = 0;
                uint8_t fronted_pp = 0;
                uint8_t ism_def = 0;
                uint8_t ism_mod = 0;
                uint8_t has_paren = 0;
                uint8_t head_fam = 0, head_al = 0;
                uint8_t mudaf_fam = 0, mudaf_fem = 0;
                uint8_t prev_fam = 0, prev_al = 0, prev_fem = 0, prev_tan = 0;
                uint8_t gov_vt = 0;
                int8_t gov_vo = 0;
                int gov_idx = -1;
                uint8_t idafa_depth = 0;
            };
            P2Frame stk[8];
            int sp = 1;
            int cl_start = 0;
            std::vector<uint8_t> f_depth(W, 0), f_gov(W, 0), f_outer(W, 0), f_ppfl(W, 0);
            std::vector<uint8_t> f_idafa(W, 0), f_hfam(W, 0), f_mfam(W, 0), f_pfam(W, 0);
            std::vector<uint8_t> f_npm(W, 0), f_gvt(W, 0), f_pic(W, 0);
            std::vector<int8_t> f_dgov(W, -1), f_gvo(W, 0);

            for (size_t i = 0; i < W; ++i) {
                const Token& t = ctx.toks[i];
                if (t.boundary_before) {
                    if (t.bkind_mask & BK_HARD) {
                        sp = 1; stk[0] = P2Frame{}; cl_start = static_cast<int>(i);
                    } else {
                        if (t.bkind_mask & BK_CLOSE) {
                            while (sp > 1 && stk[sp - 1].has_paren) --sp;
                        }
                        if (t.bkind_mask & BK_OPEN) {
                            if (sp < 8) { stk[sp] = P2Frame{}; stk[sp].has_paren = 1; ++sp; }
                        } else if (t.bkind_mask & BK_CLOSE) {
                            stk[sp - 1].pp_depth = 0;
                            stk[sp - 1].prev_fam = 0;
                            stk[sp - 1].idafa_depth = 0;
                            if (stk[sp - 1].gov == 5) stk[sp - 1].ism_mod = 1;
                        } else {
                            uint8_t kp = stk[sp - 1].has_paren;
                            uint8_t phf = stk[sp - 1].head_fam;
                            uint8_t pha = stk[sp - 1].head_al;
                            stk[sp - 1] = P2Frame{};
                            stk[sp - 1].has_paren = kp;
                            if ((t.bkind_mask & BK_COMMA) && t.si.has_conj && !t.si.has_prep && ctx.syms[t.off] == S_WAW) {
                                stk[sp - 1].head_fam = phf;
                                stk[sp - 1].head_al = pha;
                            }
                            cl_start = static_cast<int>(i);
                        }
                    }
                }
                P2Frame& fr = stk[sp - 1];
                f_depth[i] = static_cast<uint8_t>(sp - 1);
                f_gov[i] = fr.gov;
                f_outer[i] = (sp >= 2) ? stk[sp - 2].gov : 0;
                f_dgov[i] = (fr.gov_idx >= 0) ? static_cast<int8_t>(std::min<int>(31, static_cast<int>(i) - fr.gov_idx)) : -1;
                f_ppfl[i] = static_cast<uint8_t>(std::min<uint8_t>(7, fr.pp_depth) | (fr.fronted_pp << 3) | (fr.ism_def << 4) | (fr.ism_mod << 5));
                f_idafa[i] = std::min<uint8_t>(7, fr.idafa_depth);
                f_hfam[i] = fr.head_fam;
                f_mfam[i] = fr.mudaf_fam;
                f_pfam[i] = fr.prev_fam;
                uint8_t npm = 0;
                if (fr.prev_fam != 0 && p1_al[i] == fr.prev_al) npm |= 1;
                if (fr.prev_fam != 0 && p1_fem[i] == fr.prev_fem) npm |= 2;
                if (fr.mudaf_fam != 0 && p1_fem[i] == fr.mudaf_fem) npm |= 4;
                if (fr.head_fam != 0 && p1_al[i] == fr.head_al) npm |= 8;
                if (t.si.has_conj || (i >= 1 && !t.boundary_before && ctx.toks[i - 1].si.particle == PART_CONJ)) npm |= 16;
                f_npm[i] = npm;
                f_gvt[i] = fr.gov_vt;
                f_gvo[i] = fr.gov_vo;
                f_pic[i] = static_cast<uint8_t>(std::min<int>(31, static_cast<int>(i) - cl_start));

                if (t.cand_count == 0) continue;
                const Cand& fin = ctx.cands[t.cand_begin + chosen[i]];
                const uint8_t* ts = &ctx.syms[t.off];
                const bool is_kana = (t.si.particle == PART_KANA) || is_kana_verb_word(ts, t.n);
                const bool is_inna = (t.si.particle == PART_INNA) ||
                    ((t.si.particle == PART_AN || t.si.particle == PART_LI_AN ||
                      t.si.particle == PART_LAKIN || t.si.particle == PART_IN) && diac_has_shadda(p1_end[i])) ||
                    (t.surf_flags & SF_INNA_PRON);

                if (is_kana) {
                    fr.gov = (fin.pos_tag == POS_VERB_PAST_SUBJ || fin.pos_tag == POS_VERB_IMPERF_12) ? 7 : 6;
                    fr.gov_idx = static_cast<int>(i); fr.pp_depth = 0; fr.fronted_pp = 0; fr.ism_def = (fr.gov == 7);
                    fr.ism_mod = 0; fr.prev_fam = 0; fr.head_fam = 0; fr.mudaf_fam = 0; fr.idafa_depth = 0;
                    fr.gov_vt = 0; fr.gov_vo = 0;
                } else if (p1_verb[i]) {
                    if (fin.pos_tag == POS_VERB_PASS) fr.gov = 3;
                    else if (fin.pos_tag == POS_VERB_PAST_SUBJ || fin.pos_tag == POS_VERB_IMPERF_12 || has_verb_obj_suffix(ts, t.n, fin)) fr.gov = 2;
                    else fr.gov = 1;
                    fr.gov_idx = static_cast<int>(i); fr.pp_depth = 0; fr.fronted_pp = 0; fr.ism_def = 0;
                    fr.ism_mod = 0; fr.prev_fam = 0; fr.head_fam = 0; fr.mudaf_fam = 0; fr.idafa_depth = 0;
                    ValencyInfo vi = get_valency(t.word_id);
                    fr.gov_vt = vi.v_trans; fr.gov_vo = vi.val_odds;
                } else if (is_inna) {
                    fr.gov = (t.surf_flags & SF_INNA_PRON) ? 5 : 4;
                    fr.gov_idx = static_cast<int>(i); fr.pp_depth = 0; fr.fronted_pp = 0; fr.ism_def = (fr.gov == 5);
                    fr.ism_mod = 0; fr.prev_fam = 0; fr.head_fam = 0; fr.mudaf_fam = 0; fr.idafa_depth = 0;
                    fr.gov_vt = 0; fr.gov_vo = 0;
                } else if (t.si.particle == PART_REL || t.si.particle == PART_COND) {
                    fr.gov = 0; fr.gov_idx = -1; fr.pp_depth = 0; fr.fronted_pp = 0; fr.idafa_depth = 0;
                } else if (t.si.particle == PART_GEN) {
                    fr.pp_depth = static_cast<uint8_t>(std::min<int>(7, fr.pp_depth + 1));
                    if (fr.gov == 4 || fr.gov == 6) fr.fronted_pp = 1;
                    if (fr.gov == 5) fr.ism_mod = 1;
                    fr.prev_fam = 0; fr.idafa_depth = 0;
                } else if (t.si.particle == PART_PREP_PRON) {
                    if (fr.gov == 4 || fr.gov == 6) fr.fronted_pp = 1;
                    if (fr.gov == 5) fr.ism_mod = 1;
                    fr.prev_fam = 0; fr.idafa_depth = 0;
                } else if (t.si.particle == PART_CONJ) {
                    fr.prev_fam = fr.head_fam; fr.prev_al = fr.head_al; fr.idafa_depth = 0;
                } else if (t.si.particle == PART_DEM && is_dem_det_word(ts, t.n) &&
                           i + 1 < W && !ctx.toks[i + 1].boundary_before && p1_al[i + 1] && !p1_gen[i + 1]) {
                    // Propagate ClauseFrame, pp_depth, idafa_depth across هذا/هذه/ذلك/تلك to following AL-noun
                } else if (!p1_verb[i] && (t.si.particle == PART_NONE || t.si.particle == PART_IDAFA_NOUN || t.si.particle == PART_NUM_IDAFA || is_ali_proper_cand(t, fin))) {
                    if (p1_gen[i]) {
                        fr.pp_depth = 1;
                        if (fr.gov == 4 || fr.gov == 6) fr.fronted_pp = 1;
                        if (fr.gov == 5) fr.ism_mod = 1;
                    } else if (fr.pp_depth > 0 && p1_fam[i] != FAM_KASRA && !p1_al[i]) {
                        fr.pp_depth = 0;
                    }
                    if (fr.pp_depth == 0) {
                        if (fr.gov == 1 && p1_fam[i] == FAM_DAMMA) fr.gov = 2;
                        else if (fr.gov == 4 && (p1_fam[i] == FAM_FATHA || ((fin.ext_flags & EF_FEM_PL_AT) && p1_fam[i] == FAM_KASRA))) {
                            fr.gov = fr.fronted_pp ? 0 : 5; fr.fronted_pp = 0; fr.ism_def = p1_al[i] || p1_suf[i];
                        } else if (fr.gov == 5 && p1_fam[i] == FAM_DAMMA) fr.gov = 0;
                        else if (fr.gov == 6 && p1_fam[i] == FAM_DAMMA) {
                            fr.gov = fr.fronted_pp ? 0 : 7; fr.fronted_pp = 0; fr.ism_def = p1_al[i] || p1_suf[i];
                        } else if (fr.gov == 7 && p1_fam[i] == FAM_FATHA) fr.gov = 0;
                    }
                    if (!fr.prev_al && p1_al[i] && fr.prev_fam != 0 && fr.prev_fam != FAM_KASRA && p1_fam[i] == FAM_KASRA) {
                        fr.mudaf_fam = fr.prev_fam; fr.mudaf_fem = fr.prev_fem;
                    } else if (!p1_al[i]) {
                        fr.mudaf_fam = 0; fr.mudaf_fem = 0;
                    }
                    if (!p1_al[i] && !p1_suf[i] && !p1_tan[i] && t.is_diptote_lex != 2) {
                        fr.idafa_depth = static_cast<uint8_t>(std::min<int>(7, fr.idafa_depth + 1));
                    } else {
                        fr.idafa_depth = 0;
                    }
                    if (p1_fam[i] != 0 && p1_fam[i] != FAM_SUKUN) {
                        if (!(!fr.prev_al && p1_al[i] && p1_fam[i] == FAM_KASRA)) {
                            fr.head_fam = p1_fam[i]; fr.head_al = p1_al[i];
                        }
                        fr.prev_fam = p1_fam[i]; fr.prev_al = p1_al[i]; fr.prev_fem = p1_fem[i]; fr.prev_tan = p1_tan[i];
                    } else {
                        fr.prev_fam = 0;
                    }
                } else {
                    fr.prev_fam = 0; fr.idafa_depth = 0;
                }
            }

            // Backward O(W) right-to-left sentence lookahead over Pass-1 output
            std::vector<uint8_t> r_dverb(W, 31), r_dprep(W, 31), r_dend(W, 0), r_flags(W, 0);
            int next_v = -1, next_p = -1, cl_end = static_cast<int>(W);
            uint8_t r_seen = 0;
            for (int ii = static_cast<int>(W) - 1; ii >= 0; --ii) {
                size_t i = static_cast<size_t>(ii);
                r_dverb[i] = (next_v >= 0) ? static_cast<uint8_t>(std::min<int>(31, next_v - ii)) : 31;
                r_dprep[i] = (next_p >= 0) ? static_cast<uint8_t>(std::min<int>(31, next_p - ii)) : 31;
                r_dend[i] = static_cast<uint8_t>(std::min<int>(31, cl_end - 1 - ii));
                r_flags[i] = r_seen;
                if (p1_verb[i]) { next_v = ii; r_seen |= 4; }
                if (ctx.toks[i].si.particle == PART_GEN || p1_gen[i]) next_p = ii;
                if (ctx.toks[i].si.particle == PART_CONJ || ctx.toks[i].si.has_conj) r_seen |= 8;
                if (!p1_verb[i] && ctx.toks[i].si.particle == PART_NONE && !p1_gen[i]) {
                    if (p1_fam[i] == FAM_DAMMA) r_seen |= 1;
                    else if (p1_fam[i] == FAM_FATHA) r_seen |= 2;
                }
                if (ctx.toks[i].boundary_before) {
                    next_v = -1; next_p = -1; cl_end = ii; r_seen = 0;
                }
            }

            if (dump_out == nullptr) {
                sc_buf.irab2_bonus.assign(dp_off[W], 0);
            }

            int32_t f2[IRAB2_NUM_FEATURES];
            for (size_t i = 0; i < W; ++i) {
                const Token& t = ctx.toks[i];
                if (t.cand_count == 0) continue;
                if (dump_out == nullptr && !p1_multi[i]) continue;
                const Cand& bc = ctx.cands[t.cand_begin + chosen[i]];
                const uint8_t* ts = &ctx.syms[t.off];

                // Compute Pass-1 margin & runner-up ending + case availability bitmask
                int32_t best_em = bc.emis + dp_bg[dp_off[i] + chosen[i]];
                int32_t sec_em = NEG_INF;
                int8_t sec_end = -1;
                uint8_t avail_mask = 0;
                const size_t lskip = (t.si.al_len >= 2) ? static_cast<size_t>(t.si.al_lam_pos) : static_cast<size_t>(-1);
                for (uint32_t k = 0; k < t.cand_count; ++k) {
                    const Cand& ak = ctx.cands[t.cand_begin + k];
                    uint8_t ae = ak.diac[ak.case_pos];
                    uint8_t af = diac_family(ae);
                    if (af == FAM_DAMMA) avail_mask |= 1;
                    else if (af == FAM_FATHA) avail_mask |= 2;
                    else if (af == FAM_KASRA) avail_mask |= 4;
                    else if (af == FAM_SUKUN) avail_mask |= 8;
                    if (diac_is_tanween(ae)) avail_mask |= 16;
                    if (ae == p1_end[i]) continue;
                    bool sc_ok = (ak.case_pos == bc.case_pos);
                    for (size_t pp = 0; sc_ok && pp < bc.case_pos; ++pp) {
                        if (pp != lskip && ak.diac[pp] != bc.diac[pp]) sc_ok = false;
                    }
                    if (!sc_ok) continue;
                    int32_t em = ak.emis + dp_bg[dp_off[i] + k];
                    if (em > sec_em) { sec_em = em; sec_end = static_cast<int8_t>(ae); }
                }
                int32_t margin_q = (sec_em > NEG_INF / 2) ? std::max<int32_t>(-8, std::min<int32_t>(31, (best_em - sec_em) / 150)) : 31;

                // Helper for bounded neighbor index inside same segment
                auto get_nb = [&](int delta) -> int {
                    int idx = static_cast<int>(i) + delta;
                    if (idx < 0 || idx >= static_cast<int>(W)) return -1;
                    if (delta < 0) {
                        for (int step = static_cast<int>(i); step > idx; --step)
                            if (ctx.toks[static_cast<size_t>(step)].boundary_before) return -1;
                    } else {
                        for (int step = static_cast<int>(i) + 1; step <= idx; ++step)
                            if (ctx.toks[static_cast<size_t>(step)].boundary_before) return -1;
                    }
                    return idx;
                };
                int im3 = get_nb(-3), im2 = get_nb(-2), im1 = get_nb(-1);
                int ip1 = get_nb(1), ip2 = get_nb(2), ip3 = get_nb(3);

                // Best case family according to trans3
                int32_t tr3_best_fam = 0;
                if (trans3_ && trans3_size_ == TRANS3_TABLE_SIZE) {
                    uint8_t s2 = (im2 >= 0) ? p1_st3[static_cast<size_t>(im2)] : 0;
                    uint8_t s1 = (im1 >= 0) ? p1_st3[static_cast<size_t>(im1)] : 0;
                    uint8_t sr_i = p1_st3[i] / NUM_TRANS3_FAMS;
                    int32_t bv = -100000;
                    for (uint8_t fm = 1; fm < NUM_TRANS3_FAMS; ++fm) {
                        int32_t sv = trans3_score(s2, s1, static_cast<uint8_t>(sr_i * NUM_TRANS3_FAMS + fm));
                        if (sv > bv) { bv = sv; tr3_best_fam = fm; }
                    }
                    if (bv <= 0) tr3_best_fam = 0;
                }

                // Group A: Pass-1 decoded signals (0..15)
                f2[0] = p1_end[i];
                f2[1] = p1_fam[i];
                f2[2] = p1_tan[i];
                f2[3] = margin_q;
                f2[4] = sec_end;
                f2[5] = (t.lex_top_end != 255) ? t.lex_top_end : -1;
                f2[6] = static_cast<int32_t>(t.cand_count);
                f2[7] = avail_mask;
                f2[8] = sc_buf.p1_irab1_best[i];
                f2[9] = sc_buf.p1_bg_best[i];
                f2[10] = clamp_f(sc_buf.p1_bg_val[i] / 128);
                f2[11] = sc_buf.p1_rbg_best[i];
                f2[12] = clamp_f(sc_buf.p1_rbg_val[i] / 128);
                f2[13] = sc_buf.p1_ng_best[i];
                f2[14] = sc_buf.p1_skip_best[i];
                f2[15] = tr3_best_fam;

                // Group B: Bidirectional Pass-1 decoded sequence i-3..i+3 (16..46)
                f2[16] = (im3 >= 0) ? p1_pos[im3] : -1;
                f2[17] = (im2 >= 0) ? p1_pos[im2] : -1;
                f2[18] = (im1 >= 0) ? p1_pos[im1] : -1;
                f2[19] = p1_pos[i];
                f2[20] = (ip1 >= 0) ? p1_pos[ip1] : -1;
                f2[21] = (ip2 >= 0) ? p1_pos[ip2] : -1;

                f2[22] = (im3 >= 0) ? p1_role[im3] : -1;
                f2[23] = (im2 >= 0) ? p1_role[im2] : -1;
                f2[24] = (im1 >= 0) ? p1_role[im1] : -1;
                f2[25] = p1_role[i];
                f2[26] = (ip1 >= 0) ? p1_role[ip1] : -1;
                f2[27] = (ip2 >= 0) ? p1_role[ip2] : -1;

                f2[28] = (im3 >= 0) ? p1_fam[im3] : -1;
                f2[29] = (im2 >= 0) ? p1_fam[im2] : -1;
                f2[30] = (im1 >= 0) ? p1_fam[im1] : -1;
                f2[31] = (ip1 >= 0) ? p1_fam[ip1] : -1;
                f2[32] = (ip2 >= 0) ? p1_fam[ip2] : -1;
                f2[33] = (ip3 >= 0) ? p1_fam[ip3] : -1;

                f2[34] = (im2 >= 0) ? p1_end[im2] : -1;
                f2[35] = (im1 >= 0) ? p1_end[im1] : -1;
                f2[36] = (ip1 >= 0) ? p1_end[ip1] : -1;
                f2[37] = (ip2 >= 0) ? p1_end[ip2] : -1;

                f2[38] = (im2 >= 0) ? p1_tan[im2] : -1;
                f2[39] = (im1 >= 0) ? p1_tan[im1] : -1;
                f2[40] = (ip1 >= 0) ? p1_tan[ip1] : -1;
                f2[41] = (ip2 >= 0) ? p1_tan[ip2] : -1;

                f2[42] = (im2 >= 0) ? ctx.toks[im2].si.particle : -1;
                f2[43] = (im1 >= 0) ? ctx.toks[im1].si.particle : -1;
                f2[44] = t.si.particle;
                f2[45] = (ip1 >= 0) ? ctx.toks[ip1].si.particle : -1;
                f2[46] = (ip2 >= 0) ? ctx.toks[ip2].si.particle : -1;

                // Group C: Morphology & surface flags (47..63)
                f2[47] = (im2 >= 0) ? p1_al[im2] : -1;
                f2[48] = (im1 >= 0) ? p1_al[im1] : -1;
                f2[49] = p1_al[i];
                f2[50] = (ip1 >= 0) ? p1_al[ip1] : -1;

                f2[51] = (im1 >= 0) ? p1_gen[im1] : -1;
                f2[52] = p1_gen[i];
                f2[53] = (ip1 >= 0) ? p1_gen[ip1] : -1;

                f2[54] = t.si.has_conj;
                f2[55] = (ip1 >= 0) ? ctx.toks[ip1].si.has_conj : -1;

                f2[56] = (im1 >= 0) ? p1_suf[im1] : -1;
                f2[57] = p1_suf[i];
                f2[58] = (ip1 >= 0) ? p1_suf[ip1] : -1;

                uint8_t wfl_i = ((bc.ext_flags & EF_FEM_PL_AT) ? 1 : 0) |
                                ((bc.ext_flags & EF_DIPTOTE) ? 2 : 0) |
                                (t.is_adj_like ? 4 : 0) |
                                (t.is_ibn ? 8 : 0) |
                                ((bc.ext_flags & (EF_DUAL_NOON | EF_MASC_PL_NOON)) ? 16 : 0) |
                                (is_invar_long_vowel_end(ts, t.n, bc.case_pos, bc) ? 32 : 0);
                uint8_t wfl_p = 0;
                if (im1 >= 0) {
                    const Token& pt_n = ctx.toks[im1];
                    wfl_p = (pt_n.is_fem_at ? 1 : 0) | (pt_n.is_adj_like ? 2 : 0) | (p1_fem[im1] ? 4 : 0) |
                            (is_title_noun(&ctx.syms[pt_n.off], pt_n.n, pt_n.si.al_len) ? 8 : 0) |
                            (pt_n.digit_after ? 16 : 0) | (p1_verb[im1] ? 32 : 0);
                }
                uint8_t wfl_nx = 0;
                if (ip1 >= 0) {
                    const Token& nx_n = ctx.toks[ip1];
                    wfl_nx = (nx_n.is_fem_at ? 1 : 0) | (nx_n.is_adj_like ? 2 : 0) | (p1_fem[ip1] ? 4 : 0) |
                             (nx_n.is_ibn ? 8 : 0) | (p1_verb[ip1] ? 16 : 0) |
                             ((p1_al[ip1] && !p1_gen[ip1]) ? 32 : 0);
                }
                f2[59] = wfl_i;
                f2[60] = wfl_p;
                f2[61] = wfl_nx;
                f2[62] = bc.morph_class;
                f2[63] = ts[t.n - 1];

                // Group D: Hierarchical ClauseFrame stack (64..77)
                f2[64] = f_depth[i];
                f2[65] = f_gov[i];
                f2[66] = f_outer[i];
                f2[67] = f_dgov[i];
                f2[68] = f_ppfl[i];
                f2[69] = f_idafa[i];
                f2[70] = f_hfam[i];
                f2[71] = f_mfam[i];
                f2[72] = f_pfam[i];
                f2[73] = f_npm[i];
                f2[74] = f_gvt[i];
                f2[75] = f_gvo[i];
                f2[76] = t.bkind_mask;
                f2[77] = (i + 1 < W) ? ctx.toks[i + 1].bkind_mask : static_cast<uint8_t>(BK_HARD);

                // Group E: Lexical Valency features (78..87)
                ValencyInfo vm2 = (im2 >= 0) ? get_valency(ctx.toks[im2].word_id) : ValencyInfo{};
                ValencyInfo vm1 = (im1 >= 0) ? get_valency(ctx.toks[im1].word_id) : ValencyInfo{};
                ValencyInfo vc0 = get_valency(t.word_id);
                ValencyInfo vp1 = (ip1 >= 0) ? get_valency(ctx.toks[ip1].word_id) : ValencyInfo{};
                f2[78] = vm1.v_trans + 4 * vm1.next_case_pref;
                f2[79] = vm1.val_odds;
                f2[80] = vm1.next_gen_p;
                f2[81] = vm1.next_acc_p;
                f2[82] = vm2.next2_case_pref + 4 * vm2.v_trans;
                f2[83] = vc0.word_syn_type + 4 * vc0.next_case_pref;
                f2[84] = vc0.tan_ratio + 8 * vc0.is_gram_fem;
                f2[85] = vc0.verb_prob;
                f2[86] = vp1.word_syn_type + 4 * vp1.is_gram_fem + 8 * (vp1.verb_prob >= 8 ? 1 : 0);
                f2[87] = vc0.next_gen_p;

                // Group F: Right-to-Left lookahead (88..95)
                f2[88] = r_dverb[i];
                f2[89] = r_dprep[i];
                f2[90] = r_dend[i];
                f2[91] = f_pic[i];
                f2[92] = r_flags[i];
                uint8_t nr_fl = 0;
                if (ip1 >= 0) {
                    const Token& nx_n = ctx.toks[ip1];
                    if ((nx_n.is_adj_like || vp1.word_syn_type == 2) && p1_al[ip1] == p1_al[i] && p1_fem[ip1] == p1_fem[i]) nr_fl |= 1;
                    if (!p1_al[i] && !p1_suf[i] && !p1_verb[ip1] && !p1_gen[ip1] && (p1_al[ip1] || p1_fam[ip1] == FAM_KASRA)) nr_fl |= 2;
                    if (nx_n.si.particle == PART_GEN || p1_gen[ip1] || is_ina_clause_word(&ctx.syms[nx_n.off], nx_n.n, nx_n.si.particle, nx_n.surf_flags)) nr_fl |= 4;
                }
                if (is_time_adverb_noun(ts, t.n, t.si.al_len) || vc0.word_syn_type == 3) nr_fl |= 8;
                f2[93] = nr_fl;
                f2[94] = (im1 >= 0) ? ctx.syms[ctx.toks[im1].off + ctx.toks[im1].n - 1] : -1;
                f2[95] = (ip1 >= 0) ? ctx.syms[ctx.toks[ip1].off + std::min<size_t>(ctx.toks[ip1].si.al_len, ctx.toks[ip1].n - 1)] : -1;

                for (size_t fi = 0; fi < IRAB2_NUM_FEATURES; ++fi) f2[fi] = clamp_f(f2[fi]);

                const bool skip_iltiqa_irab2 =
                    ((t.surf_flags & SF_MAN) != 0) || is_an_prep_word(ts, t.n) ||
                    (ip1 >= 0 && p1_al[ip1] && (bc.ext_flags & (EF_PLURAL_MEEM | EF_PAST_TEH_FEM)));
                if (skip_iltiqa_irab2) continue;

                if (dump_out != nullptr) {
                    if (gold_diacs && i < gold_diacs->size() && bc.case_pos < MAX_WORD_LEN) {
                        uint8_t ge = (*gold_diacs)[i][bc.case_pos];
                        uint8_t end_sym = ts[bc.case_pos];
                        bool weak_end = (end_sym == S_ALEF || end_sym == S_ALEF_MAKSURA || end_sym == S_YEH || end_sym == S_WAW);
                        if (ge != DIAC_NONE || weak_end) {
                            Irab2DumpSample smp;
                            std::memcpy(smp.feat, f2, sizeof(f2));
                            smp.gold_end = ge;
                            smp.p1_end = p1_end[i];
                            smp.multi_end = p1_multi[i];
                            dump_out->push_back(smp);
                        }
                    }
                } else {
                    int32_t sc2[NUM_DIACRITICS];
                    for (auto& v : sc2) v = 0;
                    irab2_trees_.predict_scores(f2, irab2_feat_count_, sc2);
                    const int32_t w_i2 = v7_weights_[V7W_IRAB2];
                    const int32_t p2_i1_delta = v7_weights_[V7W_P2_IRAB1_SCALE] - 1000;
                    for (uint32_t k = 0; k < t.cand_count; ++k) {
                        const Cand& ck = ctx.cands[t.cand_begin + k];
                        uint8_t ce = ck.diac[ck.case_pos];
                        if (ce < irab2_trees_.num_classes()) {
                            int32_t bon = static_cast<int32_t>((static_cast<int64_t>(sc2[ce]) * w_i2) / 1000);
                            if (p2_i1_delta != 0) {
                                bon += static_cast<int32_t>(
                                    (static_cast<int64_t>(sc_buf.p1_irab1_scores[i * NUM_DIACRITICS + ce]) * p2_i1_delta) / 1000);
                            }
                            sc_buf.irab2_bonus[dp_off[i] + k] = bon;
                        }
                    }
                }
            }

            if (dump_out != nullptr) {
                return std::string();
            }

            // Pass-2 Bidirectional Case-Lattice Beam Re-Decode!
            run_case_beam(&sc_buf.irab2_bonus, v7_weights_[V7W_P2_SYN_SCALE]);
        }
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
            if (cur.verb_pattern == 1 || (cur.verb_pattern == 2 && !(cur.ext_flags & EF_DIPTOTE)) ||
                (cur.ext_flags & (EF_DUAL_NOON | EF_MASC_PL_NOON))) return false;
            if ((cur.ext_flags & EF_FEM_PL_AT) && target_fam == FAM_FATHA) return false;
            if (t.surf_flags & (SF_BAAD_QABL | SF_AMS | SF_INNA_PRON)) return false;
            if (is_haythu_word(s, t.n) && target_fam != FAM_DAMMA) return false;
            if (cur_d == DIAC_NONE || cur_d == DIAC_SUKUN) return false;
            if (is_invar_long_vowel_end(s, t.n, cur.case_pos, cur) ||
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
            GOV_INNA_KHABAR = 5,
            GOV_KANA_ISM = 6,
            GOV_KANA_KHABAR = 7
        };
        struct ClauseFrame {
            uint8_t gov = GOV_NONE;
            uint8_t clause_verb_mood = FAM_NONE;
            uint8_t pp_depth = 0;
            bool fronted_pp = false;
            bool ism_def = false;
            bool ism_mod_seen = false;
            bool has_paren_scope = false;
            uint8_t head_np_fam = FAM_NONE;
            bool head_np_al = false;
            bool head_np_fem = false;
            uint8_t mudaf_head_fam = FAM_NONE;
            bool mudaf_head_fem = false;
            uint8_t prev_np_fam = FAM_NONE;
            bool prev_np_al = false;
            bool prev_np_fem = false;
            bool prev_np_suffix = false;
            bool prev_np_tan = false;
            void reset_np() {
                mudaf_head_fam = FAM_NONE;
                mudaf_head_fem = false;
                prev_np_fam = FAM_NONE;
                prev_np_al = false;
                prev_np_fem = false;
                prev_np_suffix = false;
                prev_np_tan = false;
                head_np_fam = FAM_NONE;
                head_np_al = false;
                head_np_fem = false;
            }
        };
        auto is_hunaka_word = [](const uint8_t* s, size_t n) -> bool {
            return (n == 4 && s[0] == S_HEH && s[1] == S_NOON && s[2] == S_ALEF && s[3] == S_KAF) ||
                   (n == 3 && s[0] == S_HEH && s[1] == S_NOON && s[2] == S_ALEF);
        };
        auto is_bare_fem_noun = [](const uint8_t* s, size_t n, size_t st) -> bool {
            if (n <= st + 1) return false;
            const size_t sn = n - st;
            const uint8_t* w = s + st;
            if (w[sn - 1] == S_TEH_MARBUTA) return true;
            if (sn == 3 && (w[0] == S_ALEF_HAMZA_A || w[0] == S_ALEF) && w[1] == S_REH && w[2] == S_DAD) return true; // أرض
            if (sn == 3 && w[0] == S_SHEEN && w[1] == S_MEEM && w[2] == S_SEEN) return true; // شمس
            if (sn == 3 && w[0] == S_NOON && w[1] == S_ALEF && w[2] == S_REH) return true; // نار
            if (sn == 3 && w[0] == S_NOON && w[1] == S_FEH && w[2] == S_SEEN) return true; // نفس
            if (sn == 3 && w[0] == S_AIN && w[1] == S_YEH && w[2] == S_NOON) return true; // عين
            if (sn == 3 && w[0] == S_DAL && w[1] == S_ALEF && w[2] == S_REH) return true; // دار
            if (sn == 3 && w[0] == S_HAH && w[1] == S_REH && w[2] == S_BEH) return true; // حرب
            if (sn == 3 && w[0] == S_REH && w[1] == S_YEH && w[2] == S_HAH) return true; // ريح
            if (sn == 3 && w[0] == S_SEEN && w[1] == S_WAW && w[2] == S_QAF) return true; // سوق
            if (sn == 2 && w[0] == S_YEH && w[1] == S_DAL) return true; // يد
            return false;
        };

        static constexpr int MAX_CLAUSE_DEPTH = 8;
        ClauseFrame clause_stack[MAX_CLAUSE_DEPTH];
        int clause_sp = 1;

        for (size_t i = 0; i < W; ++i) {
            const Token& t = ctx.toks[i];
            if (t.cand_count == 0) continue;
            if (t.boundary_before) {
                if (t.bkind_mask & BK_HARD) {
                    clause_sp = 1;
                    clause_stack[0] = ClauseFrame{};
                } else {
                    if (t.bkind_mask & BK_CLOSE) {
                        while (clause_sp > 1 && clause_stack[clause_sp - 1].has_paren_scope) {
                            --clause_sp;
                        }
                    }
                    if (t.bkind_mask & BK_COMMA) {
                        bool keep_scope = clause_stack[clause_sp - 1].has_paren_scope;
                        uint8_t prev_h_fam = clause_stack[clause_sp - 1].head_np_fam;
                        bool prev_h_al = clause_stack[clause_sp - 1].head_np_al;
                        bool prev_h_fem = clause_stack[clause_sp - 1].head_np_fem;
                        clause_stack[clause_sp - 1] = ClauseFrame{};
                        clause_stack[clause_sp - 1].has_paren_scope = keep_scope;
                        if (t.si.has_conj && !t.si.has_prep && ctx.syms[t.off] == S_WAW) {
                            clause_stack[clause_sp - 1].head_np_fam = prev_h_fam;
                            clause_stack[clause_sp - 1].head_np_al = prev_h_al;
                            clause_stack[clause_sp - 1].head_np_fem = prev_h_fem;
                        }
                    } else if (t.bkind_mask & BK_OPEN) {
                        if (clause_sp < MAX_CLAUSE_DEPTH) {
                            clause_stack[clause_sp] = ClauseFrame{};
                            clause_stack[clause_sp].has_paren_scope = true;
                            ++clause_sp;
                        }
                    } else if (t.bkind_mask & BK_CLOSE) {
                        clause_stack[clause_sp - 1].reset_np();
                        clause_stack[clause_sp - 1].pp_depth = 0;
                        if (clause_stack[clause_sp - 1].gov == GOV_INNA_KHABAR) {
                            clause_stack[clause_sp - 1].ism_mod_seen = true;
                        }
                    } else {
                        // Other boundary (quotes/digits/dash): reset clause state safely while keeping paren scope
                        bool keep_scope = clause_stack[clause_sp - 1].has_paren_scope;
                        clause_stack[clause_sp - 1] = ClauseFrame{};
                        clause_stack[clause_sp - 1].has_paren_scope = keep_scope;
                    }
                }
            }
            ClauseFrame& f = clause_stack[clause_sp - 1];

            {
                const Token* pt = (!t.boundary_before && i >= 1) ? &ctx.toks[i - 1] : nullptr;
                const Cand* pc = pt ? &get_cand(i - 1) : nullptr;
                const Token* nx = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? &ctx.toks[i + 1] : nullptr;
                const uint8_t* ts = &ctx.syms[t.off];

                // Guard مَنْ vs مِنْ before non-verb nouns & idafa heads (مِنْ كُلِّ، مِنْ أَفْضَلِ، مِنَ الْمُحَادَثَاتِ)
                if (pt && (pt->surf_flags & SF_MAN) && pc->diac[pt->n - 2] == DIAC_FATHA &&
                    !(t.surf_flags & SF_THEMMA)) {
                    const Cand& pre_cur = get_cand(i);
                    const uint8_t pre_cf = diac_family(pre_cur.diac[pre_cur.case_pos]);
                    const bool pre_al = (pre_cur.flags & CF_HAS_AL) || t.si.al_len > 0;
                    const bool is_noun_or_idafa = (pre_cur.verb_pattern == 0 && !is_aba_verb_word(ts, t.n) &&
                                                   !(t.si.particle != PART_NONE && t.si.particle != PART_IDAFA_NOUN &&
                                                     t.si.particle != PART_NUM_IDAFA && !is_ali_proper_cand(t, pre_cur)));
                    if (is_noun_or_idafa &&
                        ((t.si.particle == PART_IDAFA_NOUN && !(pre_cur.flags & CF_SUFFIX)) ||
                         t.si.particle == PART_NUM_IDAFA ||
                         pre_cf == FAM_FATHA || pre_cf == FAM_KASRA || (pre_al && pre_cf != FAM_DAMMA))) {
                        const uint8_t want_man_end = pre_al ? DIAC_FATHA : DIAC_SUKUN;
                        int32_t best_sc = NEG_INF;
                        uint32_t best_k = UINT32_MAX;
                        for (uint32_t k = 0; k < pt->cand_count; ++k) {
                            const Cand& ac = ctx.cands[pt->cand_begin + k];
                            if (ac.diac[pt->n - 2] == DIAC_KASRA && ac.diac[pt->n - 1] == want_man_end && ac.emis > best_sc) {
                                best_sc = ac.emis; best_k = k;
                            }
                        }
                        if (best_k != UINT32_MAX) {
                            override_idx[i - 1] = -1;
                            chosen[i - 1] = best_k;
                            pc = &get_cand(i - 1);
                        }
                    }
                }

                const Cand& cur = get_cand(i);
                const uint8_t cf = diac_family(cur.diac[cur.case_pos]);
                const bool c_al = (cur.flags & CF_HAS_AL) || t.si.al_len > 0;
                const bool c_gen = (cur.flags & CF_GEN) || t.si.has_prep;
                const bool c_ali_prop = is_ali_proper_cand(t, cur);
                const bool c_part_non_noun = (t.si.particle != PART_NONE && !c_ali_prop);
                const bool c_noun = (cur.verb_pattern == 0) && !c_part_non_noun && !c_gen;
                const bool c_noun_any = (cur.verb_pattern == 0) && !c_part_non_noun;

                // Law A: Noun immediately before digits is Mudaf (no tanween: فِي سَنَةِ 2013)
                if (t.digit_after && c_noun_any && !c_al && !(cur.flags & CF_SUFFIX) &&
                    diac_is_tanween(cur.diac[cur.case_pos]) && !(cur.flags & CF_OOV)) {
                    enforce_case_fam(i, cf, 0, 2600, true);
                }

                // Law A2: Arabic Counted-Noun (Tamyiz) & Date/Month rules after digits
                if (pt && pt->digit_after && !t.boundary_before) {
                    const uint8_t* ts_num = &ctx.syms[t.off];
                    const bool cur_month = is_month_word(ts_num, t.n);
                    if (cur_month) {
                        if (t.is_diptote_lex == 2 && !(cur.flags & CF_HAS_AL)) {
                            enforce_case_fam(i, FAM_FATHA, 0, 3500, true);
                        }
                    } else if (c_noun_any && !c_al && !(cur.flags & CF_SUFFIX)) {
                        const bool nx_counted = nx && nx->si.particle == PART_NONE && nx->si.verb_like < 2 &&
                                                !nx->si.has_prep && nx->top_verb != 1;
                        const bool scale_mudaf = is_num_scale_word(ts_num, t.n) && nx_counted;
                        if (pt->num_after_class == 3) {
                            enforce_case_fam(i, FAM_FATHA, scale_mudaf ? 0 : 1, 3500, true);
                        } else if (pt->num_after_class == 2 || pt->num_after_class == 4) {
                            if ((cur.ext_flags & EF_DIPTOTE) && !nx_counted) {
                                enforce_case_fam(i, FAM_FATHA, 0, 3500, true);
                            } else {
                                enforce_case_fam(i, FAM_KASRA, scale_mudaf ? 0 : 1, 3500, true);
                            }
                        }
                    }
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

                // const uint8_t* ts moved above
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

                // Law J & Imperfect Mood Agreement (CF_LAM_NASB + clause_verb_mood)
                if ((cur.flags & CF_LAM_NASB) && cur.verb_pattern == 1 && !is_invar_long_vowel_end(ts, t.n, cur.case_pos, cur) &&
                    cf != FAM_FATHA) {
                    enforce_case_fam(i, FAM_FATHA, 0, 3500, true);
                } else if (pt && !t.boundary_before && pt->si.particle == PART_JAZM &&
                    (cur.verb_pattern == 1 || (t.top_verb == 1 && starts_like_imperf(ts, t.n))) &&
                    !c_al && !(cur.flags & CF_SUFFIX) && cur.case_pos == t.n - 1 &&
                    ts[cur.case_pos] != S_WAW && ts[cur.case_pos] != S_YEH && ts[cur.case_pos] != S_ALEF_MAKSURA &&
                    cf == FAM_DAMMA) {
                    const uint8_t target_j = diac_has_shadda(cur.diac[cur.case_pos]) ? FAM_FATHA : FAM_SUKUN;
                    enforce_case_fam(i, target_j, 0, 3500, t.n >= 4);
                } else if (f.clause_verb_mood == FAM_FATHA &&
                           (cur.verb_pattern == 1 || t.si.particle == PART_KANA) &&
                           starts_like_imperf(ts, t.n) && !c_al && !(cur.flags & CF_SUFFIX) &&
                           cur.case_pos == t.n - 1 && !is_invar_long_vowel_end(ts, t.n, cur.case_pos, cur) &&
                           (cf == FAM_DAMMA || cf == FAM_KASRA)) {
                    enforce_case_fam(i, FAM_FATHA, 0, 2600, false);
                } else if (f.clause_verb_mood == FAM_NONE &&
                           (cur.verb_pattern == 1 || t.si.particle == PART_KANA) &&
                           starts_like_imperf(ts, t.n) && !c_al && !(cur.flags & (CF_SUFFIX | CF_LAM_NASB)) &&
                           cur.case_pos == t.n - 1 && ts[cur.case_pos] == S_YEH &&
                           diac_family(cur.diac[cur.case_pos - 1]) == FAM_KASRA && cf == FAM_FATHA) {
                    int32_t best_sc = NEG_INF;
                    uint32_t best_k = UINT32_MAX;
                    for (uint32_t k = 0; k < t.cand_count; ++k) {
                        const Cand& ac = ctx.cands[t.cand_begin + k];
                        if (ac.diac[ac.case_pos] == DIAC_NONE && ac.emis > best_sc) {
                            best_sc = ac.emis; best_k = k;
                        }
                    }
                    if (best_k != UINT32_MAX) { override_idx[i] = -1; chosen[i] = best_k; }
                }

                // Law B: Diptotes governed by preposition, idafa, or proper diptote never taking KASRA
                const bool pt_is_mudaf_gen = pt && (pt->si.particle == PART_NONE || is_ali_proper_cand(*pt, *pc)) &&
                                             (pc->verb_pattern == 0 || pc->verb_pattern == 2) && !(pc->flags & CF_HAS_AL) &&
                                             !diac_is_tanween(pc->diac[pc->case_pos]) &&
                                             diac_family(pc->diac[pc->case_pos]) == FAM_KASRA;
                const bool dip_noun = (cur.verb_pattern != 1 && !c_part_non_noun && !is_invar_long_vowel_end(ts, t.n, cur.case_pos, cur)) &&
                                      (!c_gen || (cur.ext_flags & EF_DIPTOTE)) &&
                                      (cur.verb_pattern == 0 ||
                                       (pt && (pt->si.particle == PART_GEN || pt->si.has_prep || pt_is_mudaf_gen ||
                                               pt->si.particle == PART_IDAFA_NOUN || pt->si.particle == PART_NUM_IDAFA)));
                const bool kunya_fem_proper = pt && !c_al && ts[t.n - 1] == S_TEH_MARBUTA &&
                                              is_kunya_dip_head(&ctx.syms[pt->off], pt->n);
                const bool eff_diptote = (cur.ext_flags & EF_DIPTOTE) || kunya_fem_proper;
                if (eff_diptote && !c_al && dip_noun &&
                    !(pt && pt->digit_after && pt->num_after_class != 5) &&
                    ((c_gen && t.word_id == UINT32_MAX) ||
                     (pt && (pt->si.particle == PART_GEN || pt->si.particle == PART_IDAFA_NOUN ||
                             pt->si.particle == PART_NUM_IDAFA || pt->si.has_prep || pt_is_mudaf_gen)))) {
                    const bool proper_dip = kunya_fem_proper || (t.is_diptote_lex == 2) ||
                                            (classify_diptote_stem(&ctx.syms[t.off], t.n, t.si.al_len, cur.diac) == 2);
                    const bool afal_super = (t.n == t.si.al_len + 4 && ctx.syms[t.off + t.si.al_len] == S_ALEF_HAMZA_A);
                    const Cand* nxc_p = nx ? &get_cand(i + 1) : nullptr;
                    const bool nx_is_mudaf_noun = nx && nxc_p && nx->si.particle == PART_NONE && !nx->is_ibn && !nx->si.has_prep &&
                                                  (nxc_p->verb_pattern != 1 || (afal_super && nx->has_noun_lex));
                    const bool nx_coord_super = afal_super && nx &&
                                                (nx->is_adj_like ||
                                                 ((ctx.syms[nx->off] == S_WAW || ctx.syms[nx->off] == S_FEH) &&
                                                  nx->n == 5 && ctx.syms[nx->off + 1] == S_ALEF_HAMZA_A));
                    const bool nx_mudaf = !proper_dip && (t.digit_after || nx_coord_super ||
                                          (is_num_scale_word(ts, t.n) && nx_is_mudaf_noun) ||
                                          (nx_is_mudaf_noun && (nx->si.al_len > 0 || afal_super || !nx->is_adj_like)));
                    if (nx_mudaf) {
                        enforce_case_fam(i, FAM_KASRA, 0, 4000, true);
                    } else {
                        enforce_case_fam(i, FAM_FATHA, 0, 4000, true);
                    }
                } else if (eff_diptote && !c_al && (c_noun_any || (cur.verb_pattern == 2 && (cur.ext_flags & EF_DIPTOTE))) &&
                           !is_invar_long_vowel_end(ts, t.n, cur.case_pos, cur) &&
                           (t.is_diptote_lex == 2 || kunya_fem_proper) && cf == FAM_KASRA) {
                    enforce_case_fam(i, FAM_FATHA, 0, 4000, true);
                }

                if (is_haythu_word(ts, t.n)) {
                    enforce_case_fam(i, FAM_DAMMA, 0, 4000, true);
                }

                // Law AY: Disambiguate explanatory أَيْ vs declinable quantifier/interrogative أَيُّ/أَيَّ/أَيِّ
                if (t.si.particle == PART_AY && t.n == 2 && (ts[0] == S_ALEF_HAMZA_A || ts[0] == S_ALEF) &&
                    ts[1] == S_YEH && nx && nx->si.particle == PART_NONE && !nx->si.has_prep &&
                    !nx->si.has_conj && nx->si.al_len == 0 && nx->top_verb != 1 && nx->si.verb_like < 2) {
                    const Cand& nxc_ay = get_cand(i + 1);
                    const uint8_t nx_fam_ay = diac_family(nxc_ay.diac[nxc_ay.case_pos]);
                    const bool nx_gen_target = (nxc_ay.verb_pattern == 0) &&
                                               (nx_fam_ay == FAM_KASRA ||
                                                (nxc_ay.ext_flags & (EF_MASC_PL_NOON | EF_DUAL_NOON)));
                    uint8_t want_ay = DIAC_NONE;
                    if (pt && !t.boundary_before) {
                        if ((pt->si.particle == PART_GEN || pt->si.particle == PART_IDAFA_NOUN) &&
                            !diac_is_tanween(pc->diac[pc->case_pos])) {
                            want_ay = DIAC_SHADDA_KASRA;
                        } else if (pt->si.particle == PART_NONE && pt->si.al_len == 0 && pc->verb_pattern == 0 &&
                                   !(pc->flags & CF_SUFFIX) && !diac_is_tanween(pc->diac[pc->case_pos]) &&
                                   diac_family(pc->diac[pc->case_pos]) == FAM_KASRA && nx_gen_target) {
                            want_ay = DIAC_SHADDA_KASRA;
                        }
                    } else if (i == 0 && nx_gen_target) {
                        want_ay = DIAC_SHADDA_DAMMA;
                    }
                    if (want_ay != DIAC_NONE) {
                        int32_t best_sc = NEG_INF;
                        uint32_t best_k = UINT32_MAX;
                        for (uint32_t k = 0; k < t.cand_count; ++k) {
                            const Cand& ac = ctx.cands[t.cand_begin + k];
                            if (ac.diac[1] == want_ay && ac.emis > best_sc) {
                                best_sc = ac.emis; best_k = k;
                            }
                        }
                        if (best_k != UINT32_MAX) {
                            override_idx[i] = -1;
                            chosen[i] = best_k;
                        }
                    }
                }

                // Law GH: غَيْرَ أَنَّ / غَيْرَ أَنَّهُ -> mandatory FATHA unless preceded by preposition
                if (t.si.particle == PART_IDAFA_NOUN && t.n == 3 && ts[0] == S_GHAIN && ts[1] == S_YEH && ts[2] == S_REH &&
                    !(pt && pt->si.particle == PART_GEN && !diac_is_tanween(pc->diac[pc->case_pos])) &&
                    nx && !nx->boundary_before) {
                    const Cand& nxc_gh = get_cand(i + 1);
                    const uint8_t nx0 = ctx.syms[nx->off];
                    const bool is_anna_shadda =
                        (nx->si.particle == PART_AN && (nxc_gh.morph_class == MC_PART_SHADDA || diac_has_shadda(nxc_gh.diac[1]))) ||
                        ((nx->surf_flags & SF_INNA_PRON) && (nx0 == S_ALEF_HAMZA_A || nx0 == S_ALEF));
                    if (is_anna_shadda) {
                        enforce_case_fam(i, FAM_FATHA, 0, 3500, true);
                    }
                }

                // Law AMS: أَمْسِ + day-of-week (أَمْسِ الثُّلَاثَاءَ / الْجُمُعَةَ / الْأَحَدَ) -> accusative Badal, never genitive
                if (pt && (pt->surf_flags & SF_AMS) && c_noun && c_al &&
                    is_day_of_week_word(ts, t.n, t.si.al_len) && cf == FAM_KASRA) {
                    enforce_case_fam(i, FAM_FATHA, 0, 3500, true);
                }

                // Law C: Preposition / shadda-أيّ + non-diptote noun (including declinable locative & idafa nouns) -> mandatory KASRA
                const bool c_idafa_or_loc = is_declinable_prep_noun(ts, t.n) ||
                                            (t.si.particle == PART_IDAFA_NOUN && !(t.surf_flags & SF_FIVE_NOUN) &&
                                             ts[t.n - 1] != S_ALEF_MAKSURA && ts[t.n - 1] != S_ALEF);
                if ((c_noun_any || (cur.verb_pattern == 0 && c_idafa_or_loc)) &&
                    !comp_num_tok && !eff_diptote && !is_haythu_word(ts, t.n) &&
                    !(cur.ext_flags & (EF_DUAL_NOON | EF_MASC_PL_NOON)) &&
                    !(t.surf_flags & (SF_BAAD_QABL | SF_AMS | SF_INNA_PRON)) &&
                    !is_invar_long_vowel_end(ts, t.n, cur.case_pos, cur) &&
                    !(pt && pt->digit_after && pt->num_after_class != 5) &&
                    cf != FAM_NONE && cf != FAM_SUKUN && cf != FAM_KASRA) {
                    const bool prev_man_pron = pt && (pt->surf_flags & SF_MAN) && pc->diac[pt->n - 2] == DIAC_FATHA;
                    const bool prev_prep = pt && !diac_is_tanween(pc->diac[pc->case_pos]) &&
                                                  ((pt->si.particle == PART_GEN && !prev_man_pron) ||
                                                   (pt->si.particle == PART_AY && diac_has_shadda(pc->diac[pt->n - 1])));
                    if ((c_gen && t.word_id == UINT32_MAX) || prev_prep) {
                        enforce_case_fam(i, FAM_KASRA, -1, 4000, true);
                    }
                }

                // Ensure proper name عَلِيّ vs prep+pron عَلَيَّ in contextual Proper Name positions
                if ((t.surf_flags & SF_ALI) && !(t.surf_flags & SF_ALA) && t.n >= 3 &&
                    ((nx && (nx->surf_flags & SF_IBN)) ||
                     (pt && (pt->si.particle == PART_GEN || (pt->surf_flags & SF_IBN) ||
                             is_title_noun(&ctx.syms[pt->off], pt->n, pt->si.al_len))))) {
                    const Cand& cur_ali = get_cand(i);
                    const bool cur_is_prep_alayya = (cur_ali.diac[t.n - 2] == DIAC_FATHA);
                    const bool pt_is_title = pt && is_title_noun(&ctx.syms[pt->off], pt->n, pt->si.al_len);
                    uint8_t want_ali_fam = pt_is_title ? diac_family(pc->diac[pc->case_pos]) : static_cast<uint8_t>(FAM_KASRA);
                    if (cur_is_prep_alayya || (pt_is_title && diac_family(cur_ali.diac[cur_ali.case_pos]) != want_ali_fam)) {
                        int32_t best_sc = NEG_INF;
                        uint32_t best_k = UINT32_MAX;
                        for (uint32_t k = 0; k < t.cand_count; ++k) {
                            const Cand& ac = ctx.cands[t.cand_begin + k];
                            if (ac.diac[t.n - 2] == DIAC_KASRA) {
                                int32_t sc = ac.emis + ((diac_family(ac.diac[ac.case_pos]) == want_ali_fam) ? 3000 : 0);
                                if (sc > best_sc) { best_sc = sc; best_k = k; }
                            }
                        }
                        if (best_k != UINT32_MAX) {
                            override_idx[i] = -1;
                            chosen[i] = best_k;
                        }
                    }
                }

                // Update PP scope before evaluating clause-level government on t
                if (f.pp_depth > 0) {
                    const bool stay_in_pp = c_gen || t.si.particle == PART_GEN || t.si.particle == PART_PREP_PRON ||
                                            t.si.particle == PART_CONJ || t.si.particle == PART_DEM ||
                                            (pt && (pt->si.particle == PART_GEN || pt->si.particle == PART_PREP_PRON ||
                                                    pt->si.has_prep || pt->si.particle == PART_IDAFA_NOUN ||
                                                    pt->si.particle == PART_NUM_IDAFA)) ||
                                            (f.prev_np_fam == FAM_KASRA && cur.verb_pattern == 0 &&
                                             (c_al || !f.prev_np_al) &&
                                             (cf == FAM_KASRA || t.is_adj_like || !f.prev_np_tan));
                    if (!stay_in_pp) f.pp_depth = 0;
                }

                // Law D: Clause Government corrections on bare nouns (not governed by prep/idafa)
                if (pt && (c_noun || c_ali_prop) && f.prev_np_fam == FAM_NONE && !pt->digit_after &&
                    !is_invar_long_vowel_end(ts, t.n, cur.case_pos, cur)) {
                    if ((pt->si.particle == PART_KANA ||
                         (pc->verb_pattern == 1 && is_kana_verb_word(&ctx.syms[pt->off], pt->n))) &&
                        c_al && cf != FAM_DAMMA && !(cur.ext_flags & EF_FEM_PL_AT)) {
                        const bool kana_elative_tamyiz = (pc->ext_flags & EF_PAST_TEH_FEM) &&
                                                         t.n >= 6 && ts[2] == S_ALEF_HAMZA_A &&
                                                         nx && !nx->si.al_len && nx->si.particle == PART_NONE &&
                                                         (nx->lex_top_end == DIAC_FATHATAN || nx->lex_top_end == DIAC_SHADDA_FATHATAN);
                        if (!kana_elative_tamyiz) enforce_case_fam(i, FAM_DAMMA, 0, 3500, true);
                    } else if (f.gov == GOV_VERB_NEED_SUBJ && pc->verb_pattern == 1 &&
                               !is_time_adverb_noun(ts, t.n, t.si.al_len)) {
                        const bool nx_ina_comp = nx && is_ina_clause_word(&ctx.syms[nx->off], nx->n, nx->si.particle, nx->surf_flags);
                        const bool afal_after_pass = (pc->pos_tag == POS_VERB_PASS && !c_al && t.n == 4 && ts[0] == S_ALEF_HAMZA_A);
                        if (!(cur.ext_flags & EF_FEM_PL_AT) && !afal_after_pass &&
                            (pc->pos_tag == POS_VERB_PASS || has_verb_obj_suffix(&ctx.syms[pt->off], pt->n, *pc) ||
                             nx_ina_comp || is_title_noun(ts, t.n, t.si.al_len))) {
                            if (cf == FAM_KASRA || cf == FAM_FATHA) {
                                enforce_case_fam(i, FAM_DAMMA, -1, 3000, true);
                            }
                        } else if ((cur.ext_flags & EF_FEM_PL_AT) && (pc->ext_flags & EF_PAST_TEH_FEM) && c_al && cf == FAM_KASRA) {
                            enforce_case_fam(i, FAM_DAMMA, 0, 3000, true);
                        } else if (!(cur.ext_flags & EF_FEM_PL_AT) &&
                                   (cf == FAM_KASRA ||
                                    ((pc->ext_flags & EF_PAST_TEH_FEM) && cf == FAM_FATHA &&
                                     ts[t.n - 1] == S_TEH_MARBUTA))) {
                            enforce_case_fam(i, FAM_DAMMA, c_al ? 0 : -1, 3000, true);
                        }
                    } else if (pc->verb_pattern == 1 &&
                               (pc->pos_tag == POS_VERB_PAST_SUBJ || pc->pos_tag == POS_VERB_IMPERF_12)) {
                        if (cur.ext_flags & EF_FEM_PL_AT) {
                            if (cf != FAM_KASRA) enforce_case_fam(i, FAM_KASRA, -1, 3000, true);
                        } else if (cf == FAM_DAMMA || cf == FAM_KASRA) {
                            enforce_case_fam(i, FAM_FATHA, -1, 2600, false);
                        }
                    } else if ((pt->surf_flags & SF_INNA_PRON) && cf == FAM_KASRA && !c_al) {
                        enforce_case_fam(i, FAM_DAMMA, -1, 2600, false);
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
                // Stack-Aware Clause Laws (S1-S3): Delayed Ism across PP & Inna/Kana Khabar
                if (f.pp_depth == 0 && !is_invar_long_vowel_end(ts, t.n, cur.case_pos, cur) && !comp_num_tok) {
                    const Cand& s_cur = get_cand(i);
                    const uint8_t s_cf = diac_family(s_cur.diac[s_cur.case_pos]);
                    const bool s_al = (s_cur.flags & CF_HAS_AL) || t.si.al_len > 0;
                    const bool s_gen = (s_cur.flags & CF_GEN) || t.si.has_prep;
                    const bool s_elative = (s_cur.ext_flags & EF_DIPTOTE) && t.n == t.si.al_len + 4 &&
                                           ts[t.si.al_len] == S_ALEF_HAMZA_A;
                    const bool s_ith_adv = (t.n >= 5 && ts[t.n - 2] == S_YEH_HAMZA && ts[t.n - 1] == S_THAL);
                    const bool s_noun_ok = (s_cur.verb_pattern == 0) && !s_gen && !t.si.has_conj &&
                                           !(s_cur.flags & CF_SUFFIX) && !is_time_adverb_noun(ts, t.n, t.si.al_len) &&
                                           !s_ith_adv && !is_hunaka_word(ts, t.n) && !(t.surf_flags & SF_THEMMA) &&
                                           (t.top_verb != 1 || s_elative);
                    const bool s_ghayr_tok = (t.si.particle == PART_IDAFA_NOUN && t.n == 3 &&
                                              ts[0] == S_GHAIN && ts[1] == S_YEH && ts[2] == S_REH);
                    if (f.fronted_pp && (f.gov == GOV_KANA_ISM || f.gov == GOV_INNA_ISM) && s_noun_ok &&
                        t.si.particle == PART_NONE && !t.digit_after && pt &&
                        (pt->si.particle == PART_PREP_PRON || (pt->surf_flags & SF_THEMMA) ||
                         is_hunaka_word(&ctx.syms[pt->off], pt->n)) &&
                        !(s_cur.ext_flags & (EF_MASC_PL_NOON | EF_DUAL_NOON))) {
                        const int8_t tan_m = (s_al || (s_cur.ext_flags & EF_DIPTOTE)) ? 0 : -1;
                        if (f.gov == GOV_KANA_ISM && (s_cf == FAM_FATHA || s_cf == FAM_KASRA)) {
                            enforce_case_fam(i, FAM_DAMMA, tan_m, 3000, true);
                        } else if (f.gov == GOV_INNA_ISM && (s_cf == FAM_DAMMA || (s_cf == FAM_KASRA && !(s_cur.ext_flags & EF_FEM_PL_AT)))) {
                            enforce_case_fam(i, (s_cur.ext_flags & EF_FEM_PL_AT) ? FAM_KASRA : FAM_FATHA, tan_m, 2600, false);
                        }
                    } else if (!f.fronted_pp && f.gov == GOV_INNA_KHABAR && s_noun_ok &&
                               (t.si.particle == PART_NONE || s_ghayr_tok) &&
                               !(pt && pt->si.particle == PART_CONJ)) {
                        const bool pt_inna_pron = pt && (pt->surf_flags & SF_INNA_PRON);
                        const bool close_paren_khabar = (t.bkind_mask & BK_CLOSE) && f.ism_mod_seen &&
                                                        !(pt && (pt->si.particle == PART_GEN || pt->si.has_prep));
                        const bool pt_safe_pred = !pt || pt->si.particle == PART_NONE || pt->si.particle == PART_DEM || pt_inna_pron;
                        if (pt_inna_pron && s_cf == FAM_FATHA) {
                            const int8_t tan_m = (s_al || (s_cur.ext_flags & EF_DIPTOTE) || s_ghayr_tok) ? 0 : -1;
                            enforce_case_fam(i, FAM_DAMMA, tan_m, 3000, true);
                        } else if (f.ism_def && !s_al && s_cf == FAM_FATHA && pt_safe_pred) {
                            const int8_t tan_m = ((s_cur.ext_flags & EF_DIPTOTE) || s_ghayr_tok) ? 0 : -1;
                            enforce_case_fam(i, FAM_DAMMA, tan_m, 2600, false);
                        } else if (close_paren_khabar && (s_cf == FAM_FATHA || s_cf == FAM_KASRA) && !s_al) {
                            const int8_t tan_m = ((s_cur.ext_flags & EF_DIPTOTE) || s_ghayr_tok) ? 0 : -1;
                            enforce_case_fam(i, FAM_DAMMA, tan_m, 2800, true);
                        }
                    } else if (!f.fronted_pp && f.gov == GOV_KANA_KHABAR && f.ism_def && s_noun_ok &&
                               t.si.particle == PART_NONE && !s_al && !(s_cur.ext_flags & EF_FEM_PL_AT) &&
                               pt && !pt->boundary_before && pt->si.particle == PART_NONE &&
                               ((pc->flags & CF_HAS_AL) || pt->si.al_len > 0) &&
                               (s_cur.ext_flags & EF_DIPTOTE) && s_cf == FAM_DAMMA) {
                        enforce_case_fam(i, FAM_FATHA, 0, 2600, false);
                    }
                }
                if (t.boundary_before && (c_noun || t.si.particle == PART_NONE) && !(c_al || c_gen) &&
                    is_masdar_prep_adv(ts, t.n) && nx && nx->si.particle == PART_GEN && cf == FAM_KASRA) {
                    enforce_case_fam(i, FAM_FATHA, 1, 3500, true);
                }

                // Law G1: Proper diptote name at BOS or after active past verb followed by بن -> DAMMA (عُمَرُ بْنُ الخَطَّابِ)
                if ((t.boundary_before || (pt && pt->top_pos == POS_VERB_PAST_3SG)) &&
                    (t.is_diptote_lex == 2 || classify_diptote_stem(&ctx.syms[t.off], t.n, t.si.al_len, cur.diac) == 2) &&
                    c_noun && nx && (nx->surf_flags & SF_IBN)) {
                    enforce_case_fam(i, FAM_DAMMA, 0, 3500, true);
                }

                // Law G2: Dynamic Pass-2 بْن / ابْن agreement using actual chosen case of preceding name, and Title + Proper Name Badal
                if (pt && (t.surf_flags & SF_IBN)) {
                    const bool is_alef_ibn = is_alef_ibn_word(ts, t.n);
                    const bool pt_is_common_mudaf_before_ibn = is_alef_ibn && !pt->si.al_len && pt->si.particle == PART_NONE &&
                                                               !(pt->surf_flags & (SF_NAME_HEAD | SF_IBN | SF_ALI)) &&
                                                               pt->is_diptote_lex != 2 && pc->verb_pattern == 0 &&
                                                               !diac_is_tanween(pc->diac[pc->case_pos]) &&
                                                               !(i >= 2 && !pt->boundary_before && (ctx.toks[i - 2].surf_flags & SF_IBN));
                    uint8_t target_ibn = FAM_NONE;
                    if (pt_is_common_mudaf_before_ibn) {
                        target_ibn = FAM_KASRA; // حَدِيثُ ابْنِ عَبَّاسٍ، مَسْرَحُ ابْنِ أَمْسِيكَ، ابْنَةُ ابْنِ ابْنِهِ
                    } else if (i >= 2 && !pt->boundary_before && (ctx.toks[i - 2].surf_flags & SF_IBN)) {
                        target_ibn = FAM_KASRA;
                    } else if (i >= 2 && !pt->boundary_before && (ctx.toks[i - 2].surf_flags & SF_NAME_HEAD)) {
                        const Cand& p2c = get_cand(i - 2);
                        target_ibn = diac_family(p2c.diac[p2c.case_pos]);
                    } else if (i >= 2 && !pt->boundary_before &&
                               (pt->si.al_len > 0 || is_allah_word(&ctx.syms[pt->off], pt->n)) && !pt->si.has_prep &&
                               ctx.toks[i - 2].si.al_len == 0 && ctx.toks[i - 2].si.particle == PART_NONE &&
                               !ctx.toks[i - 2].digit_after) {
                        const Cand& p2c = get_cand(i - 2);
                        if (p2c.verb_pattern == 0 && !(p2c.flags & CF_SUFFIX) &&
                            !diac_is_tanween(p2c.diac[p2c.case_pos])) {
                            target_ibn = diac_family(p2c.diac[p2c.case_pos]);
                        }
                    } else if ((pt->si.particle == PART_NONE || is_ali_proper_cand(*pt, *pc)) && pc->verb_pattern == 0 &&
                               (!(pc->flags & CF_OOV) || is_ali_proper_cand(*pt, *pc))) {
                        const bool fem_dip_fatha = (pt->n >= 3 && pc->diac[pc->case_pos] == DIAC_FATHA &&
                                                    ctx.syms[pt->off + pt->n - 1] == S_TEH_MARBUTA);
                        if ((pc->ext_flags & EF_DIPTOTE) || fem_dip_fatha) {
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
                    } else if ((pc->flags & CF_OOV) && i >= 2 && !pt->boundary_before) {
                        const Token& p2t = ctx.toks[i - 2];
                        if (p2t.si.particle == PART_GEN || p2t.si.particle == PART_IDAFA_NOUN || p2t.si.has_prep) {
                            target_ibn = FAM_KASRA;
                        }
                    }
                    if ((target_ibn == FAM_DAMMA || target_ibn == FAM_FATHA || target_ibn == FAM_KASRA)) {
                        enforce_case_fam(i, target_ibn, 0, 3500, true);
                    }
                } else if (pt && (c_noun || c_ali_prop) && !c_al && !(t.surf_flags & SF_IBN) &&
                           !is_invar_long_vowel_end(ts, t.n, cur.case_pos, cur) &&
                           pc->verb_pattern == 0 && pt->si.al_len > 0 &&
                           is_title_noun(&ctx.syms[pt->off], pt->n, pt->si.al_len) &&
                           (!nx || (nx->si.particle != PART_GEN && !(nx->si.al_len > 0 && !nx->si.has_prep))) &&
                           !t.digit_after) {
                    const uint8_t pf_title = diac_family(pc->diac[pc->case_pos]);
                    const uint8_t want_b = ((cur.ext_flags & EF_DIPTOTE) && pf_title == FAM_KASRA) ? static_cast<uint8_t>(FAM_FATHA) : pf_title;
                    if ((want_b == FAM_DAMMA || want_b == FAM_FATHA || want_b == FAM_KASRA) && cf != want_b) {
                        enforce_case_fam(i, want_b, (cur.ext_flags & EF_DIPTOTE) ? 0 : -1, 3000, true);
                    }
                }
            }

            // Law H: Multi-word Na't chain + attached-conjunction agreement + Gender-Symmetric Late Na't
            {
                const Cand& cur = get_cand(i);
                const uint8_t cf = diac_family(cur.diac[cur.case_pos]);
                const bool c_al = (cur.flags & CF_HAS_AL) || t.si.al_len > 0;
                const bool c_gen = (cur.flags & CF_GEN) || t.si.has_prep;
                const bool c_ali_prop = is_ali_proper_cand(t, cur);
                const bool c_noun = (cur.verb_pattern == 0) && (t.si.particle == PART_NONE || c_ali_prop) && !c_gen;
                const bool is_proclitic_conj = (t.si.has_conj || ((ctx.syms[t.off] == S_WAW || ctx.syms[t.off] == S_FEH) &&
                                                t.n >= 4 && cur.diac[0] == DIAC_FATHA)) && !t.si.has_prep;
                if (!t.boundary_before && i >= 1 && !ctx.toks[i - 1].digit_after &&
                    !is_invar_long_vowel_end(&ctx.syms[t.off], t.n, cur.case_pos, cur)) {
                    if (c_noun && f.prev_np_fam != FAM_NONE && f.prev_np_fam != FAM_SUKUN && cf != f.prev_np_fam) {
                        const bool match_mudaf_head = (f.mudaf_head_fam != FAM_NONE && c_al && cf == f.mudaf_head_fam);
                        if (!match_mudaf_head && !(cur.ext_flags & EF_FEM_PL_AT)) {
                            const bool c_indef_adj = t.is_adj_like || (t.n > t.si.al_len + 3 && ctx.syms[t.off + t.si.al_len] == S_MEEM &&
                                                                       cur.morph_class == MC_NOUN_SHADDA_CORE);
                            if (c_al && (f.prev_np_al || f.prev_np_suffix)) {
                                enforce_case_fam(i, f.prev_np_fam, 0, 2600, true);
                            } else if (!c_al && !(cur.flags & CF_SUFFIX) && c_indef_adj && f.prev_np_tan) {
                                enforce_case_fam(i, f.prev_np_fam, 1, 2600, true);
                            }
                        } else if (is_proclitic_conj && (cur.ext_flags & EF_FEM_PL_AT) && f.prev_np_fam == FAM_DAMMA &&
                                   (!c_al || f.prev_np_al) && (c_al || f.prev_np_tan || f.prev_np_al) && cf == FAM_KASRA) {
                            // مُحَاضَرَاتٌ وَوَرَشَاتٌ / وَالْمُنَاقَشَاتُ وَوَرَشَاتُ
                            enforce_case_fam(i, FAM_DAMMA, c_al ? 0 : (f.prev_np_tan ? 1 : 0), 2600, true);
                        }
                    } else if (c_noun && c_al && t.is_adj_like && !(cur.ext_flags & (EF_DIPTOTE | EF_FEM_PL_AT)) &&
                               (f.mudaf_head_fam == FAM_DAMMA || f.mudaf_head_fam == FAM_FATHA) &&
                               f.prev_np_fam == FAM_KASRA && cf == FAM_KASRA) {
                        // Gender-symmetric late Na't across Idafa: وَزِيرُ الصِّحَّةِ الْمِصْرِيُّ / أَنْشِطَةُ الْمُؤْتَمَرِ الرَّسْمِيَّةُ
                        const bool a_fem = (ctx.syms[t.off + t.n - 1] == S_TEH_MARBUTA);
                        const Token& pt_adj = ctx.toks[i - 1];
                        const bool ilayh_bare_fem = !f.prev_np_fem &&
                                                    is_bare_fem_noun(&ctx.syms[pt_adj.off], pt_adj.n, pt_adj.si.al_len);
                        if (f.mudaf_head_fem == a_fem && f.prev_np_fem != a_fem && !ilayh_bare_fem) {
                            enforce_case_fam(i, f.mudaf_head_fam, 0, 3000, true);
                        }
                    } else if (f.prev_np_fam == FAM_NONE && f.head_np_fam != FAM_NONE && f.head_np_fam != FAM_SUKUN &&
                               is_proclitic_conj && cur.verb_pattern == 0 && !c_gen &&
                               (c_noun || t.si.particle == PART_IDAFA_NOUN) && cf != f.head_np_fam &&
                               !(cur.ext_flags & (EF_DIPTOTE | EF_FEM_PL_AT))) {
                        const Token* nxc = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? &ctx.toks[i + 1] : nullptr;
                        if (c_al == f.head_np_al || (!c_al && nxc && nxc->si.al_len > 0)) {
                            enforce_case_fam(i, f.head_np_fam, c_al ? 0 : -1, 2600, true);
                        }
                    }
                }
            }

            // Law T: Indefinite Noun Tanween vs Mudaf Non-Tanween Disambiguation
            {
                const Cand& cur = get_cand(i);
                const uint8_t* ts = &ctx.syms[t.off];
                const uint8_t cur_d = cur.diac[cur.case_pos];
                const uint8_t cf = diac_family(cur_d);
                const bool cur_tan = diac_is_tanween(cur_d);
                const bool c_al = (cur.flags & CF_HAS_AL) || t.si.al_len > 0;
                const bool c_ali_prop = is_ali_proper_cand(t, cur);
                const Token* nx = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? &ctx.toks[i + 1] : nullptr;
                if (cur.verb_pattern == 0 && !c_al && !(cur.flags & (CF_SUFFIX | CF_OOV)) &&
                    !(cur.ext_flags & (EF_DIPTOTE | EF_DUAL_NOON | EF_MASC_PL_NOON)) &&
                    !(t.surf_flags & (SF_BAAD_QABL | SF_AMS | SF_INNA_PRON | SF_FIVE_NOUN | SF_COMP_NUM | SF_ASHAR)) &&
                    !is_invar_long_vowel_end(ts, t.n, cur.case_pos, cur) &&
                    !t.digit_after && cur.case_pos == t.n - 1 && (cf == FAM_DAMMA || cf == FAM_KASRA || cf == FAM_FATHA)) {
                    if (t.si.particle == PART_IDAFA_NOUN) {
                        if (cur_tan && nx &&
                            (cf == FAM_KASRA ||
                             (nx->si.particle == PART_DEM && ctx.syms[nx->off] != S_KAF) ||
                             nx->si.al_len > 0)) {
                            enforce_case_fam(i, cf, 0, 3000, true);
                        }
                    } else if (t.si.particle == PART_NONE || c_ali_prop) {
                        if (cur_tan && nx) {
                            bool force_strip = false;
                            const Cand& nxc = get_cand(i + 1);
                            const bool nx_al = (nxc.flags & CF_HAS_AL) || nx->si.al_len > 0;
                            const uint8_t nx_fam = diac_family(nxc.diac[nxc.case_pos]);
                            const bool nx_tan = diac_is_tanween(nxc.diac[nxc.case_pos]);
                            const bool nx_conj_prep = nx->si.has_conj || nx->si.has_prep;
                            if ((nx->surf_flags & SF_IBN) && nx->si.particle == PART_NONE) {
                                force_strip = true;
                            } else if (nx->is_diptote_lex == 2 && nx->si.particle == PART_NONE && !nx_conj_prep) {
                                force_strip = true;
                            } else if (nx_al && !nx_conj_prep && nx_fam == FAM_KASRA && nx->si.particle == PART_NONE) {
                                force_strip = true;
                            } else if ((nxc.flags & CF_SUFFIX) && !nx_al && !nx_conj_prep && nx->si.particle == PART_NONE &&
                                       nxc.verb_pattern == 0 && (nx_fam == FAM_KASRA || cf == FAM_KASRA)) {
                                force_strip = true;
                            } else if (cf == FAM_KASRA && !nx_al && !nx_conj_prep && nx->si.particle == PART_NONE &&
                                       nx->top_verb != 1 && nx->si.verb_like < 2 && !nx->is_adj_like &&
                                       nxc.verb_pattern == 0 &&
                                       (!nx_tan || (is_prep_cause_word(ts, t.n) && !nx->boundary_before)) &&
                                       (nx_fam == FAM_KASRA || (nxc.flags & CF_OOV))) {
                                force_strip = true;
                            }
                            if (force_strip) enforce_case_fam(i, cf, 0, 2600, true);
                        } else if (!cur_tan && nx && (cf == FAM_DAMMA || cf == FAM_KASRA) && !(t.surf_flags & SF_NAME_HEAD)) {
                            const Cand& nxc = get_cand(i + 1);
                            const bool nx_al = (nxc.flags & CF_HAS_AL) || nx->si.al_len > 0;
                            const uint8_t nx_fam = diac_family(nxc.diac[nxc.case_pos]);
                            const bool nx_tan = diac_is_tanween(nxc.diac[nxc.case_pos]);
                            if (!nx->si.has_conj && nx->si.particle == PART_NONE && !nx_al &&
                                nx->is_adj_like && nx_tan && nx_fam == cf) {
                                enforce_case_fam(i, cf, 1, 2600, true);
                            }
                        }
                    }
                }
            }

            // Refresh fin after potential Pass-2 switch
            const Cand& fin = get_cand(i);
            const bool c_al = (fin.flags & CF_HAS_AL) || t.si.al_len > 0;
            uint8_t fin_fam = eff_case_fam(&ctx.syms[t.off], fin);
            const bool fin_gen = (fin.flags & CF_GEN) || t.si.has_prep;
            if (fin_fam == FAM_NONE) {
                if (fin_gen) {
                    fin_fam = FAM_KASRA;
                } else if (i >= 1 && !t.boundary_before) {
                    const Token& pt = ctx.toks[i - 1];
                    const Cand& pc = get_cand(i - 1);
                    if (pt.si.particle == PART_GEN || pt.si.particle == PART_IDAFA_NOUN || pt.si.particle == PART_NUM_IDAFA ||
                        ((c_al || is_invar_long_vowel_end(&ctx.syms[t.off], t.n, fin.case_pos, fin)) &&
                         pc.verb_pattern == 0 && pt.si.particle == PART_NONE && !(pc.flags & CF_HAS_AL) && pt.si.al_len == 0)) {
                        // Implicit genitive on مقصور/منقوص/واو-مد after prep or idafa head (دَارِ الأُوبِرَا، مَهْرَجَانِ مُوسْكُو)
                        fin_fam = FAM_KASRA;
                    } else if (f.gov == GOV_INNA_ISM) {
                        fin_fam = FAM_FATHA;
                    } else if (f.gov == GOV_VERB_NEED_SUBJ || f.gov == GOV_VERB_PASS ||
                               f.gov == GOV_KANA_ISM || f.gov == GOV_INNA_KHABAR) {
                        fin_fam = FAM_DAMMA;
                    }
                } else if (f.gov == GOV_INNA_ISM) {
                    fin_fam = FAM_FATHA;
                } else if (f.gov == GOV_VERB_NEED_SUBJ || f.gov == GOV_VERB_PASS ||
                           f.gov == GOV_KANA_ISM || f.gov == GOV_INNA_KHABAR) {
                    fin_fam = FAM_DAMMA;
                }
            }

            // Update clause stack frame state
            const bool tok_is_fem = (ctx.syms[t.off + t.n - 1] == S_TEH_MARBUTA);
            const bool fin_is_kanna_ma = (t.n >= 4 && ctx.syms[t.off + t.n - 2] == S_MEEM && ctx.syms[t.off + t.n - 1] == S_ALEF);
            if (t.si.particle == PART_KANA || (fin.verb_pattern == 1 && is_kana_verb_word(&ctx.syms[t.off], t.n))) {
                if (fin.pos_tag == POS_VERB_PAST_SUBJ || fin.pos_tag == POS_VERB_IMPERF_12) {
                    f.gov = GOV_KANA_KHABAR;
                    f.ism_def = true;
                } else {
                    f.gov = GOV_KANA_ISM;
                    f.ism_def = false;
                }
                f.clause_verb_mood = FAM_NONE;
                f.pp_depth = 0;
                f.fronted_pp = false;
                f.ism_mod_seen = false;
                f.reset_np();
            } else if (fin.verb_pattern == 1) {
                if (fin.pos_tag == POS_VERB_PASS) f.gov = GOV_VERB_PASS;
                else if (fin.pos_tag == POS_VERB_PAST_SUBJ || fin.pos_tag == POS_VERB_IMPERF_12) f.gov = GOV_VERB_HAVE_SUBJ;
                else f.gov = GOV_VERB_NEED_SUBJ;
                f.clause_verb_mood = FAM_NONE;
                f.pp_depth = 0;
                f.fronted_pp = false;
                f.ism_def = false;
                f.ism_mod_seen = false;
                f.reset_np();
            } else if ((t.si.particle == PART_INNA && !fin_is_kanna_ma) ||
                       ((t.si.particle == PART_AN || t.si.particle == PART_LI_AN || t.si.particle == PART_LAKIN) &&
                        fin.morph_class == MC_PART_SHADDA && !fin_is_kanna_ma)) {
                if (t.surf_flags & SF_INNA_PRON) {
                    f.gov = GOV_INNA_KHABAR;
                    f.ism_def = true;
                } else {
                    f.gov = GOV_INNA_ISM;
                    f.ism_def = false;
                }
                f.clause_verb_mood = FAM_NONE;
                f.pp_depth = 0;
                f.fronted_pp = false;
                f.ism_mod_seen = false;
                f.reset_np();
            } else if (t.si.particle == PART_NASB || t.si.particle == PART_HATTA ||
                       ((t.si.particle == PART_AN || t.si.particle == PART_LI_AN) && fin.morph_class != MC_PART_SHADDA)) {
                f.clause_verb_mood = FAM_FATHA;
            } else if (t.si.particle == PART_JAZM ||
                       (t.si.particle == PART_IN && fin.morph_class != MC_PART_SHADDA)) {
                f.clause_verb_mood = FAM_SUKUN;
            } else if (fin.verb_pattern == 0 &&
                       (t.si.particle == PART_NONE || t.si.particle == PART_IDAFA_NOUN || is_ali_proper_cand(t, fin))) {
                const bool invar_end = is_invar_long_vowel_end(&ctx.syms[t.off], t.n, fin.case_pos, fin);
                if (fin_gen) {
                    f.pp_depth = 1;
                    if (f.gov == GOV_KANA_ISM || f.gov == GOV_INNA_ISM) f.fronted_pp = true;
                    if (f.gov == GOV_INNA_KHABAR) f.ism_mod_seen = true;
                } else if (f.pp_depth == 0) {
                    if (f.gov == GOV_VERB_NEED_SUBJ && fin_fam == FAM_DAMMA) f.gov = GOV_VERB_HAVE_SUBJ;
                    else if (f.gov == GOV_INNA_ISM &&
                             (fin_fam == FAM_FATHA || ((fin.ext_flags & EF_FEM_PL_AT) && fin_fam == FAM_KASRA) || invar_end)) {
                        if (f.fronted_pp) { f.gov = GOV_NONE; f.fronted_pp = false; }
                        else {
                            f.gov = GOV_INNA_KHABAR;
                            f.ism_def = c_al || (fin.flags & CF_SUFFIX);
                            f.ism_mod_seen = false;
                        }
                    } else if (f.gov == GOV_INNA_KHABAR) {
                        if (fin_fam == FAM_DAMMA) {
                            f.gov = GOV_NONE;
                        } else if (fin_fam == FAM_KASRA) {
                            f.ism_mod_seen = true;
                        }
                    } else if (f.gov == GOV_KANA_ISM && fin_fam == FAM_DAMMA) {
                        if (f.fronted_pp) { f.gov = GOV_NONE; f.fronted_pp = false; }
                        else {
                            f.gov = GOV_KANA_KHABAR;
                            f.ism_def = c_al || (fin.flags & CF_SUFFIX) || (t.is_diptote_lex == 2);
                        }
                    } else if (f.gov == GOV_KANA_KHABAR && fin_fam == FAM_FATHA) {
                        f.gov = GOV_NONE;
                    }
                }

                if (fin_fam != FAM_NONE && fin_fam != FAM_SUKUN) {
                    const bool is_invar_mudaf_ilayh = invar_end;
                    if (!f.prev_np_al && (c_al || is_invar_mudaf_ilayh)) {
                        if (f.prev_np_fam != FAM_NONE && f.prev_np_fam != FAM_KASRA && fin_fam == FAM_KASRA) {
                            f.mudaf_head_fam = f.prev_np_fam;
                            f.mudaf_head_fem = f.prev_np_fem;
                        }
                        if (i >= 1 && (ctx.toks[i - 1].si.particle == PART_GEN || ctx.toks[i - 1].si.has_prep)) {
                            f.head_np_fam = fin_fam;
                            f.head_np_al = c_al;
                            f.head_np_fem = tok_is_fem;
                        }
                    } else {
                        if (!c_al) { f.mudaf_head_fam = FAM_NONE; f.mudaf_head_fem = false; }
                        f.head_np_fam = fin_fam;
                        f.head_np_al = c_al;
                        f.head_np_fem = tok_is_fem;
                    }
                    f.prev_np_fam = fin_fam;
                    f.prev_np_al = c_al;
                    f.prev_np_fem = tok_is_fem;
                    f.prev_np_suffix = (fin.flags & CF_SUFFIX) != 0;
                    f.prev_np_tan = diac_is_tanween(fin.diac[fin.case_pos]);
                } else {
                    if (fin_gen && i >= 1 && (ctx.toks[i - 1].si.particle == PART_GEN || ctx.toks[i - 1].si.has_prep)) {
                        f.head_np_fam = FAM_KASRA;
                        f.head_np_al = c_al;
                        f.head_np_fem = tok_is_fem;
                    }
                    f.prev_np_fam = FAM_NONE;
                    f.prev_np_al = false;
                    f.prev_np_fem = false;
                    f.prev_np_suffix = false;
                    f.prev_np_tan = false;
                }
            } else if (t.si.particle == PART_CONJ && i + 1 < W && f.head_np_fam != FAM_NONE) {
                f.prev_np_fam = f.head_np_fam;
                f.prev_np_al = f.head_np_al;
                f.prev_np_fem = f.head_np_fem;
                f.prev_np_suffix = false;
                f.prev_np_tan = false;
            } else if (t.si.particle != PART_NONE) {
                if (t.si.particle != PART_LA) f.clause_verb_mood = FAM_NONE;
                if (t.si.particle == PART_GEN) {
                    f.pp_depth = 1;
                    if (f.gov == GOV_KANA_ISM || f.gov == GOV_INNA_ISM) f.fronted_pp = true;
                    if (f.gov == GOV_INNA_KHABAR) f.ism_mod_seen = true;
                } else if (t.si.particle == PART_PREP_PRON || (t.surf_flags & SF_THEMMA) ||
                           is_hunaka_word(&ctx.syms[t.off], t.n)) {
                    if (f.gov == GOV_KANA_ISM || f.gov == GOV_INNA_ISM) f.fronted_pp = true;
                    if (f.gov == GOV_INNA_KHABAR) f.ism_mod_seen = true;
                } else if (t.si.particle == PART_DEM && f.pp_depth == 0) {
                    if (f.gov == GOV_INNA_ISM) {
                        if (f.fronted_pp) { f.gov = GOV_NONE; f.fronted_pp = false; }
                        else { f.gov = GOV_INNA_KHABAR; f.ism_def = true; f.ism_mod_seen = false; }
                    } else if (f.gov == GOV_KANA_ISM) {
                        if (f.fronted_pp) { f.gov = GOV_NONE; f.fronted_pp = false; }
                        else { f.gov = GOV_KANA_KHABAR; f.ism_def = true; }
                    }
                }
                f.prev_np_fam = FAM_NONE;
                f.prev_np_al = false;
                f.prev_np_fem = false;
                f.prev_np_suffix = false;
                f.prev_np_tan = false;
                if (t.si.particle != PART_CONJ && t.si.particle != PART_PREP_PRON && t.si.particle != PART_DEM) {
                    f.head_np_fam = FAM_NONE;
                    f.head_np_al = false;
                    f.head_np_fem = false;
                    f.mudaf_head_fam = FAM_NONE;
                    f.mudaf_head_fem = false;
                }
                if (t.si.particle == PART_REL || t.si.particle == PART_COND) {
                    f.gov = GOV_NONE;
                    f.pp_depth = 0;
                    f.fronted_pp = false;
                }
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

void Diacritizer::dump_irab2_sentence(const char* gold_utf8, size_t len, std::vector<Irab2DumpSample>& out) const {
    if (!is_loaded() || !gold_utf8 || len == 0) return;
    std::string plain;
    plain.reserve(len);
    std::vector<std::array<uint8_t, MAX_WORD_LEN>> gold_diacs;
    size_t pos = 0;
    while (pos < len) {
        size_t save = pos;
        uint32_t cp = utf8_next(gold_utf8, len, pos);
        if (is_arabic_letter(cp)) {
            std::array<uint8_t, MAX_WORD_LEN> gd{};
            gd.fill(DIAC_NONE);
            size_t cnt = 0;
            auto consume_diacs = [&](size_t l_idx) {
                bool has_sh = false;
                uint8_t base = DIAC_NONE;
                while (pos < len) {
                    size_t ps = pos;
                    uint32_t dcp = utf8_next(gold_utf8, len, pos);
                    if (is_tatweel(dcp)) continue;
                    if (!is_arabic_diacritic(dcp)) { pos = ps; break; }
                    if (dcp == 0x0651u) has_sh = true;
                    else if (dcp == 0x064Eu) base = DIAC_FATHA;
                    else if (dcp == 0x064Fu) base = DIAC_DAMMA;
                    else if (dcp == 0x0650u) base = DIAC_KASRA;
                    else if (dcp == 0x0652u) base = DIAC_SUKUN;
                    else if (dcp == 0x064Bu) base = DIAC_FATHATAN;
                    else if (dcp == 0x064Cu) base = DIAC_DAMMATAN;
                    else if (dcp == 0x064Du) base = DIAC_KASRATAN;
                }
                if (l_idx < MAX_WORD_LEN) {
                    uint8_t did = base;
                    if (has_sh) {
                        if (base == DIAC_FATHA) did = DIAC_SHADDA_FATHA;
                        else if (base == DIAC_DAMMA) did = DIAC_SHADDA_DAMMA;
                        else if (base == DIAC_KASRA) did = DIAC_SHADDA_KASRA;
                        else if (base == DIAC_FATHATAN) did = DIAC_SHADDA_FATHATAN;
                        else if (base == DIAC_DAMMATAN) did = DIAC_SHADDA_DAMMATAN;
                        else if (base == DIAC_KASRATAN) did = DIAC_SHADDA_KASRATAN;
                        else did = DIAC_SHADDA;
                    }
                    gd[l_idx] = did;
                }
            };
            utf8_append(plain, cp);
            consume_diacs(0);
            cnt = 1;
            while (pos < len) {
                size_t ns = pos;
                uint32_t ncp = utf8_next(gold_utf8, len, pos);
                if (is_tatweel(ncp) || is_arabic_diacritic(ncp)) continue;
                if (is_arabic_letter(ncp)) {
                    utf8_append(plain, ncp);
                    consume_diacs(cnt);
                    ++cnt;
                } else {
                    pos = ns;
                    break;
                }
            }
            if (cnt <= MAX_WORD_LEN) gold_diacs.push_back(gd);
        } else {
            if (!is_arabic_diacritic(cp)) plain.append(gold_utf8 + save, pos - save);
        }
    }
    if (gold_diacs.empty()) return;
    diacritize_internal(plain.data(), plain.size(), &out, &gold_diacs);
}

}
