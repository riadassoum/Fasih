#include "tashkeel/diacritizer.hpp"
#include "tashkeel/tiny_inflate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

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

constexpr size_t MAX_STEM_CANDS = 12;
constexpr size_t MAX_CANDS = 24;
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

inline bool is_allah_any(const uint8_t* s, size_t n) {
    if (n == 4 && is_allah_word(s, 4)) return true;
    if (n == 5 && (s[0] == S_BEH || s[0] == S_WAW || s[0] == S_FEH || s[0] == S_TEH) && is_allah_word(s + 1, 4)) return true;
    if (n == 3 && s[0] == S_LAM && s[1] == S_LAM && s[2] == S_HEH) return true;
    if (n == 4 && (s[0] == S_WAW || s[0] == S_FEH) && s[1] == S_LAM && s[2] == S_LAM && s[3] == S_HEH) return true;
    return false;
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

inline uint32_t compute_extra_surf_flags(const uint8_t* s, size_t n, const SurfaceInfo& si, uint16_t sf) {
    uint32_t xf = XF_NONE;
    if (starts_like_imperf(s, n)) xf |= XF_STARTS_IMPERF;
    if (is_kana_verb_word(s, n)) xf |= XF_IS_KANA_VERB;
    if (is_title_noun(s, n, si.al_len)) xf |= XF_IS_TITLE;
    if (is_time_adverb_noun(s, n, si.al_len)) xf |= XF_IS_TIME_ADV;
    if (is_innama_word(s, n)) xf |= XF_IS_INNAMA;
    if (is_decade_num_word(s, n)) xf |= XF_IS_DECADE;
    if (is_day_of_week_word(s, n, si.al_len)) xf |= XF_IS_DAY_WEEK;
    if (is_dem_det_word(s, n)) xf |= XF_IS_DEM_DET;
    if (is_an_prep_word(s, n)) xf |= XF_IS_AN_PREP;
    if (is_bare_baad(s, n)) xf |= XF_IS_BARE_BAAD;
    if (is_aba_verb_word(s, n)) xf |= XF_IS_ABA_VERB;
    if (is_masdar_prep_adv(s, n)) xf |= XF_IS_MASDAR_ADV;
    if (is_ina_clause_word(s, n, si.particle, sf)) xf |= XF_IS_INA_CLAUSE;
    if (si.al_len >= 2 && si.al_len < n && is_sun_letter(s[si.al_len])) xf |= XF_AL_SUN;
    if (n >= 4 && s[n - 2] == S_WAW && s[n - 1] == S_ALEF) xf |= XF_ENDS_WAW_ALEF;
    if (n >= 5 && s[n - 2] == S_WAW && s[n - 1] == S_NOON) xf |= XF_ENDS_WAW_NOON;
    if (is_allah_word(s, n)) xf |= XF_IS_ALLAH;
    if (is_haythu_word(s, n)) xf |= XF_IS_HAYTHU;
    if ((n == 4 && s[0] == S_HEH && s[1] == S_NOON && s[2] == S_ALEF && s[3] == S_KAF) ||
        (n == 3 && s[0] == S_HEH && s[1] == S_NOON && s[2] == S_ALEF)) xf |= XF_IS_HUNAKA;
    if (is_kull_word(s, n)) xf |= XF_IS_KULL;
    if (is_ila_yeh_word(s, n)) xf |= XF_IS_ILA_YEH;
    if (is_umar_word(s, n)) xf |= XF_IS_UMAR;
    if (n == 3 && s[0] == S_LAM && s[1] == S_QAF && s[2] == S_DAL) xf |= XF_IS_LAQAD;
    if ((sf & (SF_AMS | SF_THEMMA | SF_ASHAR)) || (xf & (XF_IS_INNAMA | XF_IS_DECADE))) xf |= XF_NON_IDAFA_MABNI;
    return xf;
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
    for (uint32_t i = 0; i < NUM_V8_WEIGHTS; ++i) v8_weights_[i] = DEFAULT_V8_WEIGHTS[i];
    for (uint32_t i = 0; i < NUM_V9_WEIGHTS; ++i) v9_weights_[i] = DEFAULT_V9_WEIGHTS[i];
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
    owned_buf_.reset();
    owned_size_ = 0;
    is_v3_ = false;
    is_v5_ = false;
    is_v7_ = false;
    is_v8_ = false;
    is_v9_ = false;
    has_v5_24roles_ = false;
    v9_flags_ = 0;
    vocab_count_ = 0;
    base_vocab_count_ = 0;
    fst_nodes_count_ = 0;
    word_meta_ = nullptr;
    word_meta_v3_ = nullptr;
    word_meta_v8_ = nullptr;
    candidates_ = nullptr;
    candidates_v3_ = nullptr;
    candidates_v5_ = nullptr;
    core_pool_ = nullptr;
    valency_ = ValencyTableV7();
    extra_core_ = ExtraCoreTableV8();
    trans3_ = nullptr;
    trans3_size_ = 0;
    trans_matrix_ = nullptr;
    trans_matrix_size_ = 0;
    trans_scaled_16_.clear();
    trans_scaled_32_.clear();
    has_trans3_lut_ = false;
    oov_feat_count_ = 16;
    irab_feat_count_ = 40;
    irab2_feat_count_ = IRAB2_NUM_FEATURES;
    core2_feat_count_ = CORE2_NUM_FEATURES;
    fst_ = BitmappedFST();
    fst_ext_ = BitmappedFST();
    neural_irab_.clear();
    bigrams_ = BigramTable();
    ngrams_ = NgramTableV6();
    core_bg_ = CoreBgTableV7();
    oov_trees_ = FlatTreeEnsemble();
    irab_trees_ = FlatTreeEnsemble();
    irab2_trees_ = FlatTreeEnsemble();
    core2_trees_ = FlatTreeEnsemble();
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
        const size_t decomp_sz = ch->uncompressed_size;
        std::unique_ptr<uint8_t[]> decomp(new uint8_t[decomp_sz]);
        size_t written = 0;
        bool ok = tiny_inflate_raw(
            base + sizeof(CompressedFileHeader),
            fsize - sizeof(CompressedFileHeader),
            decomp.get(), decomp_sz, &written);
        if (!ok || written != decomp_sz ||
            crc32_ieee(decomp.get(), decomp_sz) != ch->crc32) {
            unload();
            if (err_msg) *err_msg = "Failed to decompress or verify model checksum";
            return false;
        }
        unload();
        owned_buf_ = std::move(decomp);
        owned_size_ = decomp_sz;
        base = owned_buf_.get();
        fsize = owned_size_;
    }
    const auto* hdr2 = reinterpret_cast<const FileHeader*>(base);
    if (hdr2->magic != TASH_MAGIC ||
        (hdr2->version != TASH_VERSION_2 && hdr2->version != TASH_VERSION_3 &&
         hdr2->version != TASH_VERSION_4 && hdr2->version != TASH_VERSION_5 &&
         hdr2->version != TASH_VERSION_6 && hdr2->version != TASH_VERSION_7 &&
         hdr2->version != TASH_VERSION_8 &&
         hdr2->version != TASH_VERSION_9)) {
        unload();
        if (err_msg) *err_msg = "Bad magic or unsupported model version";
        return false;
    }
    auto in_range = [fsize](uint64_t off, uint64_t bytes) { return off <= fsize && bytes <= fsize - off; };

    if (hdr2->version == TASH_VERSION_5 || hdr2->version == TASH_VERSION_6 ||
        hdr2->version == TASH_VERSION_7 || hdr2->version == TASH_VERSION_8 ||
        hdr2->version == TASH_VERSION_9) {
        const size_t min_v56 = (hdr2->version == TASH_VERSION_9) ? sizeof(FileHeaderV9)
                             : ((hdr2->version == TASH_VERSION_8) ? sizeof(FileHeaderV8)
                             : ((hdr2->version == TASH_VERSION_7) ? sizeof(FileHeaderV7)
                             : ((hdr2->version == TASH_VERSION_6) ? sizeof(FileHeaderV6) : sizeof(FileHeaderV5))));
        if (fsize < min_v56) {
            unload();
            if (err_msg) *err_msg = "Truncated header";
            return false;
        }
        const auto* hdr5 = reinterpret_cast<const FileHeaderV5*>(base);
        const auto* hdr6 = (hdr2->version >= TASH_VERSION_6) ? reinterpret_cast<const FileHeaderV6*>(base) : nullptr;
        const auto* hdr7 = (hdr2->version >= TASH_VERSION_7) ? reinterpret_cast<const FileHeaderV7*>(base) : nullptr;
        const auto* hdr8 = (hdr2->version >= TASH_VERSION_8) ? reinterpret_cast<const FileHeaderV8*>(base) : nullptr;
        const auto* hdr9 = (hdr2->version >= TASH_VERSION_9) ? reinterpret_cast<const FileHeaderV9*>(base) : nullptr;
        const bool compact_meta = (hdr9 && (hdr9->v9_flags & V9F_COMPACT_META) != 0);
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
            !in_range(hdr5->word_meta_offset, uint64_t(hdr5->word_meta_count) * (compact_meta ? sizeof(WordMetaV8) : sizeof(WordMetaV3))) ||
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
            (hdr7 && hdr7->v7_weights_count > 0 && !in_range(hdr7->v7_weights_offset, uint64_t(hdr7->v7_weights_count) * sizeof(int32_t))) ||
            (hdr8 && hdr8->extra_core_size > 0 && !in_range(hdr8->extra_core_offset, hdr8->extra_core_size)) ||
            (hdr8 && hdr8->core2_trees_size > 0 && !in_range(hdr8->core2_trees_offset, hdr8->core2_trees_size)) ||
            (hdr8 && hdr8->v8_weights_count > 0 && !in_range(hdr8->v8_weights_offset, uint64_t(hdr8->v8_weights_count) * sizeof(int32_t))) ||
            (hdr9 && hdr9->neural_irab_size > 0 && !in_range(hdr9->neural_irab_offset, hdr9->neural_irab_size))) {
            unload();
            if (err_msg) *err_msg = "Model sections exceed file bounds";
            return false;
        }
        is_v5_ = true;
        is_v7_ = (hdr7 != nullptr);
        is_v8_ = (hdr8 != nullptr);
        is_v9_ = (hdr9 != nullptr);
        is_v3_ = false;
        vocab_count_ = hdr5->word_meta_count;
        if (hdr9 && (hdr9->v9_flags & V9F_DUAL_FST) != 0) {
            if (hdr5->fst_blob_size < 8u) {
                unload();
                if (err_msg) *err_msg = "Invalid v9 dual FST header";
                return false;
            }
            const uint8_t* fp = base + hdr5->fst_blob_offset;
            uint32_t p_sz = 0, bv = 0;
            std::memcpy(&p_sz, fp, 4);
            std::memcpy(&bv, fp + 4, 4);
            const size_t p_off = 8u;
            const size_t ext_off = (p_off + p_sz + 7u) & ~size_t(7u);
            if (p_off + p_sz > hdr5->fst_blob_size || ext_off > hdr5->fst_blob_size ||
                !fst_.init_v5(fp + p_off, p_sz)) {
                unload();
                if (err_msg) *err_msg = "Invalid v9 primary FST blob";
                return false;
            }
            if (ext_off < hdr5->fst_blob_size && !fst_ext_.init_v5(fp + ext_off, static_cast<uint32_t>(hdr5->fst_blob_size - ext_off))) {
                unload();
                if (err_msg) *err_msg = "Invalid v9 overlay FST blob";
                return false;
            }
            base_vocab_count_ = bv;
            fst_nodes_count_ = fst_.node_count() + fst_ext_.node_count();
        } else {
            if (!fst_.init_v5(base + hdr5->fst_blob_offset, hdr5->fst_blob_size)) {
                unload();
                if (err_msg) *err_msg = "Invalid v5 FST blob";
                return false;
            }
            base_vocab_count_ = vocab_count_;
            fst_nodes_count_ = fst_.node_count();
        }
        if (compact_meta) {
            word_meta_v8_ = reinterpret_cast<const WordMetaV8*>(base + hdr5->word_meta_offset);
            word_meta_v3_ = nullptr;
        } else {
            word_meta_v8_ = nullptr;
            word_meta_v3_ = reinterpret_cast<const WordMetaV3*>(base + hdr5->word_meta_offset);
        }
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
        for (uint32_t i = 0; i < NUM_V8_WEIGHTS; ++i) v8_weights_[i] = DEFAULT_V8_WEIGHTS[i];
        if (hdr8) {
            if (hdr8->v8_flags & V8F_OOV_V8_38F) {
                oov_feat_count_ = OOV_V8_FEATURES;
            }
            if (hdr8->extra_core_size > 0) {
                if (!extra_core_.init(base + hdr8->extra_core_offset, hdr8->extra_core_size)) {
                    unload();
                    if (err_msg) *err_msg = "Invalid V8 extra_core blob";
                    return false;
                }
            }
            if (hdr8->core2_trees_size > 0) {
                uint8_t c2_mode = (hdr8->v8_flags & V8F_CORE2_V5_4B) ? 2 : 0;
                core2_trees_.init(base + hdr8->core2_trees_offset, hdr8->core2_trees_size, c2_mode);
                core2_feat_count_ = static_cast<uint8_t>(hdr8->core2_feat_count ? hdr8->core2_feat_count : CORE2_NUM_FEATURES);
            }
            if (hdr8->v8_weights_count > 0) {
                const auto* w8 = reinterpret_cast<const int32_t*>(base + hdr8->v8_weights_offset);
                for (uint32_t i = 0; i < hdr8->v8_weights_count && i < NUM_V8_WEIGHTS; ++i) v8_weights_[i] = w8[i];
            }
        }
        for (uint32_t i = 0; i < NUM_V9_WEIGHTS; ++i) v9_weights_[i] = DEFAULT_V9_WEIGHTS[i];
        if (hdr9) {
            v9_flags_ = hdr9->v9_flags;
            for (uint32_t i = 0; i < NUM_V9_WEIGHTS; ++i) v9_weights_[i] = hdr9->v9_weights[i];
            if (hdr9->neural_irab_size > 0) {
                if (!neural_irab_.init(base + hdr9->neural_irab_offset, hdr9->neural_irab_size)) {
                    unload();
                    if (err_msg) *err_msg = "Invalid V9 neural_irab blob";
                    return false;
                }
            }
        }
        rebuild_trans_scaled();
        rebuild_trans3_scaled();
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
        rebuild_trans_scaled();
        rebuild_trans3_scaled();
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
    rebuild_trans_scaled();
    rebuild_trans3_scaled();
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

void Diacritizer::rebuild_trans_scaled() {
    trans_scaled_16_.clear();
    trans_scaled_32_.clear();
    if (!trans_matrix_) return;
    const size_t need = has_v5_24roles_
        ? (static_cast<size_t>(NUM_TRANS_ROLES_V5) * NUM_DIACRITICS * NUM_TRANS_ROLES_V5 * NUM_DIACRITICS)
        : (16u * NUM_DIACRITICS * 16u * NUM_DIACRITICS);
    if (trans_matrix_size_ < need) return;
    const int64_t w_tr = weights_[W_TRANS];
    bool fits_i16 = true;
    trans_scaled_16_.resize(need);
    for (size_t i = 0; i < need; ++i) {
        const int32_t v = static_cast<int32_t>((static_cast<int64_t>(trans_matrix_[i]) * w_tr) / 1000);
        if (v < -32768 || v > 32767) {
            fits_i16 = false;
            break;
        }
        trans_scaled_16_[i] = static_cast<int16_t>(v);
    }
    if (!fits_i16) {
        trans_scaled_16_.clear();
        trans_scaled_32_.resize(need);
        for (size_t i = 0; i < need; ++i) {
            trans_scaled_32_[i] = static_cast<int32_t>((static_cast<int64_t>(trans_matrix_[i]) * w_tr) / 1000);
        }
    }
}

void Diacritizer::rebuild_trans3_scaled() {
    if (!trans3_ || trans3_size_ != TRANS3_TABLE_SIZE) {
        has_trans3_lut_ = false;
        return;
    }
    const int64_t w3 = v7_weights_[V7W_TRANS3];
    for (int v = -128; v <= 127; ++v) {
        trans3_lut_[static_cast<uint8_t>(static_cast<int8_t>(v))] =
            static_cast<int32_t>((static_cast<int64_t>(v * 16) * w3) / 1000);
    }
    has_trans3_lut_ = true;
}

uint8_t Diacritizer::cand_state3_v7_val(const Token& tok, const Cand& c, const uint8_t* ts, const ValencyInfo& vi, uint8_t fam) const {
    const uint8_t cd = c.diac[c.case_pos];
    const bool c_al = (c.flags & CF_HAS_AL) || tok.si.al_len > 0;
    const bool c_gen = (c.flags & CF_GEN) || tok.si.has_prep;
    const bool is_kana = (tok.si.particle == PART_KANA) || ((tok.xflags & XF_IS_KANA_VERB) != 0);
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

uint8_t Diacritizer::cand_state3_v7(const Token& tok, const Cand& c, const uint8_t* ts) const {
    return cand_state3_v7_val(tok, c, ts, get_valency(tok.word_id), eff_case_fam(ts, c));
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
    uint32_t ec_s = 0, ec_e = 0;
    const bool has_ec = (is_v8_ && extra_core_.valid() && extra_core_.get_range(wid, ec_s, ec_e));
    if (c_cnt + (has_ec ? (ec_e - ec_s) : 0u) < 2) return false;
    uint8_t lam_skip = (si.al_len >= 2) ? si.al_lam_pos : 255;
    uint16_t first_h = 0;
    bool have_first = false;
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
        if (!have_first) { first_h = h; have_first = true; }
        else if (h != first_h) return true;
    }
    if (has_ec) {
        for (uint32_t ei = ec_s; ei < ec_e; ++ei) {
            WordCandidate wc = extra_core_.read_cand(ei);
            for (size_t p = 0; p + 1 < len; ++p) diacs[p] = extra_core_.read_core_diac(wc.core_offset, p);
            diacs[len - 1] = wc.ending_id;
            Cand tmp{};
            std::memcpy(tmp.diac, diacs, len);
            tmp.case_pos = static_cast<uint8_t>(len - 1);
            tmp.flags = wc.flags;
            finalize_cand(tmp, syms, len, si, 0);
            uint16_t h = compute_core_hash(syms, tmp.diac, tmp.case_pos, lam_skip);
            if (!have_first) { first_h = h; have_first = true; }
            else if (h != first_h) return true;
        }
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
    uint32_t ec_s = 0, ec_e = 0;
    if (is_v8_ && extra_core_.get_range(wid, ec_s, ec_e)) {
        for (uint32_t ei = ec_s; ei < ec_e; ++ei) {
            WordCandidate wc = extra_core_.read_cand(ei);
            for (size_t p = 0; p + 1 < len; ++p) diacs[p] = extra_core_.read_core_diac(wc.core_offset, p);
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

void Diacritizer::extract_word_existing_cores(uint32_t wid, const uint8_t* syms, size_t len,
                                              std::vector<uint16_t>& out_hashes, int16_t& out_top_lp) const {
    out_hashes.clear();
    out_top_lp = -30000;
    if (wid >= vocab_count_ || len == 0 || len > MAX_WORD_LEN) return;
    uint32_t c_start = meta_cand_start(wid);
    uint16_t c_cnt = meta_cand_count(wid);
    if (c_cnt == 0) return;
    SurfaceInfo si = analyze_surface(syms, len);
    uint8_t lam_skip = (si.al_len >= 2) ? si.al_lam_pos : 255;
    uint8_t diacs[MAX_WORD_LEN];
    for (uint16_t k = 0; k < c_cnt; ++k) {
        WordCandidate wc = read_cand(c_start + k);
        if (wc.log_prob > out_top_lp) out_top_lp = wc.log_prob;
        for (size_t p = 0; p + 1 < len; ++p) diacs[p] = read_core_diac(wc.core_offset, p);
        diacs[len - 1] = wc.ending_id;
        Cand tmp{};
        std::memcpy(tmp.diac, diacs, len);
        tmp.case_pos = static_cast<uint8_t>(len - 1);
        tmp.flags = wc.flags;
        finalize_cand(tmp, syms, len, si, 0);
        uint16_t h = compute_core_hash(syms, tmp.diac, tmp.case_pos, lam_skip);
        if (std::find(out_hashes.begin(), out_hashes.end(), h) == out_hashes.end()) out_hashes.push_back(h);
    }
}

uint32_t Diacritizer::lookup_relaxed(const uint8_t* syms, size_t n, int& repl_pos, uint8_t& repl_sym) const {
    repl_pos = -1;
    repl_sym = 0;
    if (n == 0 || n > MAX_WORD_LEN) return UINT32_MAX;
    uint32_t wid = fst_lookup(syms, n);
    if (wid != UINT32_MAX && wid < vocab_count_) return wid;
    uint8_t buf[MAX_WORD_LEN];
    std::memcpy(buf, syms, n);
    auto attempt = [&](size_t pos, uint8_t sym, bool record_repl) -> uint32_t {
        uint8_t old = buf[pos];
        buf[pos] = sym;
        uint32_t r = fst_lookup(buf, n);
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
    if (fst_lookup(s, stem_len) != UINT32_MAX) return true;
    uint8_t buf[MAX_WORD_LEN];
    std::memcpy(buf, s, stem_len);
    if (buf[stem_len - 1] == S_TEH) {
        buf[stem_len - 1] = S_TEH_MARBUTA;
        if (fst_lookup(buf, stem_len) != UINT32_MAX) return true;
        buf[stem_len - 1] = S_TEH;
    } else if (buf[stem_len - 1] == S_ALEF) {
        buf[stem_len - 1] = S_ALEF_MAKSURA;
        if (fst_lookup(buf, stem_len) != UINT32_MAX) return true;
        buf[stem_len - 1] = S_ALEF;
    }
    if (stem_len >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM || s[0] == S_KAF)) {
        if (fst_lookup(s + 1, stem_len - 1) != UINT32_MAX) return true;
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
        } else if (si.particle == PART_NONE && si.al_len == 0 && n == 3 &&
                   ((s[2] == S_HEH && (d[2] == DIAC_DAMMA || d[2] == DIAC_KASRA)) ||
                    (s[2] == S_KAF && (d[2] == DIAC_FATHA || d[2] == DIAC_KASRA))) &&
                   !diac_is_tanween(d[1]) && d[1] != DIAC_SUKUN && d[1] != DIAC_NONE &&
                   ((diac_has_shadda(d[1]) &&
                     ((s[0] == S_KAF && s[1] == S_LAM) || (s[0] == S_REH && s[1] == S_BEH) ||
                      (s[0] == S_ALEF_HAMZA_A && s[1] == S_MEEM) || (s[0] == S_HAH && (s[1] == S_QAF || s[1] == S_DAL || s[1] == S_JEEM)) ||
                      (s[0] == S_AIN && s[1] == S_MEEM) || (s[0] == S_SEEN && (s[1] == S_NOON || s[1] == S_REH)) ||
                      (s[0] == S_SHEEN && s[1] == S_REH) || (s[0] == S_BEH && s[1] == S_REH) ||
                      (s[0] == S_FEH && s[1] == S_NOON) || (s[0] == S_ZAH && s[1] == S_LAM) ||
                      (s[0] == S_NOON && s[1] == S_SAD) || (s[0] == S_DAD && s[1] == S_DAL) ||
                      (s[0] == S_KHAH && s[1] == S_TAH) || (s[0] == S_JEEM && s[1] == S_DAL) ||
                      (s[0] == S_SAD && s[1] == S_FEH) || (s[0] == S_WAW && s[1] == S_DAL))) ||
                    (!diac_has_shadda(d[1]) &&
                     ((s[0] == S_YEH && s[1] == S_DAL) || (s[0] == S_DAL && s[1] == S_MEEM) ||
                      (s[0] == S_FEH && s[1] == S_MEEM) ||
                      (s[0] == S_ALEF_HAMZA_A && (s[1] == S_BEH || s[1] == S_KHAH)))))) {
            c.flags |= CF_SUFFIX;
            c.case_pos = 1;
        } else if (si.particle == PART_NONE && si.al_len == 0 && n >= 4 && !diac_is_tanween(d[n - 1]) && !is_allah_any(s, n)) {
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
                if ((!diac_is_tanween(stem_end_d) || sl >= 2) && has_stem_in_fst(s, stem_len)) {
                    c.flags |= CF_SUFFIX;
                    c.case_pos = static_cast<uint8_t>(stem_len - 1);
                    if (diac_is_tanween(c.diac[c.case_pos])) {
                        c.diac[c.case_pos] = strip_tanween(c.diac[c.case_pos]);
                    }
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
    const uint16_t cnt = std::min<uint16_t>(meta_cand_count(stem_id), MAX_STEM_CANDS);
    uint32_t ec_s = 0, ec_e = 0;
    const bool has_ec = (is_v8_ && extra_core_.valid() && extra_core_.get_range(stem_id, ec_s, ec_e));
    const uint32_t total_iter = static_cast<uint32_t>(cnt) + (has_ec ? (ec_e - ec_s) : 0u);
    const uint32_t tok_start_cands = tok.cand_count;
    for (uint32_t ci = 0; ci < total_iter; ++ci) {
        const bool from_ec = (ci >= cnt);
        const WordCandidate wc = from_ec ? extra_core_.read_cand(ec_s + (ci - cnt)) : read_cand(c_start + ci);
        Cand c;
        std::memset(c.diac, 0, sizeof(c.diac));
        for (size_t j = 0; j < pre_len; ++j) c.diac[j] = pre_diac[j];
        for (size_t k = 0; k + 1 < stem_len; ++k) {
            c.diac[pre_len + k] = from_ec ? extra_core_.read_core_diac(wc.core_offset, k) : read_core_diac(wc.core_offset, k);
        }
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

        if (from_ec) {
            bool dup = false;
            for (uint32_t j = tok_start_cands; j < tok.cand_count; ++j) {
                if (std::memcmp(ctx.cands[tok.cand_begin + j].diac, c.diac, n) == 0) { dup = true; break; }
            }
            if (dup || tok.cand_count >= MAX_CANDS) continue;
        }

        c.emis = static_cast<int32_t>((static_cast<int64_t>(wc.log_prob) * weights_[W_LEX]) / 1000) - penalty -
                 (from_ec ? v8_weights_[V8W_EXTRA_CORE_PEN] : 0);
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
                        wid = fst_lookup(stem_buf, stem_len);
                        if (wid == UINT32_MAX || wid >= vocab_count_) {
                            wid = UINT32_MAX;
                            stem_buf[stem_len - 1] = S_TEH;
                        }
                    } else if (sl > 0 && stem_buf[stem_len - 1] == S_ALEF) {
                        stem_buf[stem_len - 1] = S_ALEF_MAKSURA;
                        wid = fst_lookup(stem_buf, stem_len);
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
                    if (mid == S_LAM && al == 0) {
                        // Past verb cannot take harf jarr لِـ: emit emphatic لَـ only when stem is unambiguously a past verb (not a diptote noun!)
                        const uint32_t cs0 = meta_cand_start(wid);
                        const WordCandidate wc0 = read_cand(cs0);
                        const uint8_t stem_rfl = meta_role_flags(wid);
                        bool stem_has_noun_end = false;
                        uint16_t scnt = std::min<uint16_t>(meta_cand_count(wid), MAX_STEM_CANDS);
                        for (uint16_t ci = 0; ci < scnt; ++ci) {
                            uint8_t eid = read_cand(cs0 + ci).ending_id;
                            if (diac_is_tanween(eid) || diac_family(eid) == FAM_KASRA || diac_family(eid) == FAM_DAMMA) {
                                stem_has_noun_end = true; break;
                            }
                        }
                        const bool stem_past_verb = (!is_imperf_prefix(s[pl]) && !stem_has_noun_end &&
                                                     (stem_rfl & MORPH_VERB_LIKE) != 0 &&
                                                     (wc0.ending_id == DIAC_FATHA || wc0.ending_id == DIAC_SHADDA_FATHA) &&
                                                     stem_len >= 3 && s[pl + stem_len - 1] != S_TEH_MARBUTA);
                        if (stem_past_verb) {
                            uint8_t pd_la[5];
                            std::memcpy(pd_la, pd, pl);
                            pd_la[pl - 1] = DIAC_FATHA;
                            emit_stem(ctx, tok, wid, pl, pd_la, false, suf_idx, pen, 0, rp, rs);
                        } else {
                            emit_stem(ctx, tok, wid, pl, pd, false, suf_idx, pen, flags, rp, rs);
                        }
                    } else {
                        emit_stem(ctx, tok, wid, pl, pd, al != 0, suf_idx, pen, flags, rp, rs);
                    }
                }
            }
        }
    }

    // Farasa-style fallback for OOV plural/dual/past-verb inflections (strictly OOV-only)
    if (tok.cand_count == start_count && n >= 5) {
        size_t pl = tok.si.al_len;
        const bool has_al_p = (pl >= 2);
        const bool has_single_pre = (pl == 0 && n >= 6 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_KAF || s[0] == S_LAM));
        size_t pre_off = has_al_p ? pl : (has_single_pre ? 1 : 0);
        auto fill_prefix = [&](Cand& c, uint8_t& cflags) {
            if (has_al_p) {
                for (size_t j = 0; j < pl; ++j) c.diac[j] = tok.si.pre_diac[j];
                if (!is_sun_letter(s[pl])) c.diac[tok.si.al_lam_pos] = DIAC_SUKUN;
                cflags |= CF_HAS_AL;
                if (tok.si.has_prep) cflags |= CF_GEN;
            } else if (has_single_pre) {
                c.diac[0] = proclitic_diac(s[0]);
                if (s[0] == S_BEH || s[0] == S_KAF || s[0] == S_LAM) cflags |= CF_GEN;
            }
        };
        auto lookup_noun_stem = [&](const uint8_t* ptr, size_t blen, bool try_tm) -> uint32_t {
            if (blen < 2 || blen + 1 >= MAX_WORD_LEN) return UINT32_MAX;
            std::memcpy(stem_buf, ptr, blen);
            uint32_t w = fst_lookup(stem_buf, blen);
            if (w != UINT32_MAX && w < vocab_count_) return w;
            if (try_tm) {
                if (stem_buf[blen - 1] == S_TEH) {
                    stem_buf[blen - 1] = S_TEH_MARBUTA;
                    w = fst_lookup(stem_buf, blen);
                    if (w != UINT32_MAX && w < vocab_count_) return w;
                    stem_buf[blen - 1] = S_TEH;
                }
                stem_buf[blen] = S_TEH_MARBUTA;
                w = fst_lookup(stem_buf, blen + 1);
                if (w != UINT32_MAX && w < vocab_count_) return w;
            }
            return UINT32_MAX;
        };

        size_t body_len = n - pre_off;
        // 1. Sound feminine plural: [proclitic][al] + stem + ات
        if (body_len >= 5 && s[n - 2] == S_ALEF && s[n - 1] == S_TEH) {
            size_t base_len = body_len - 2;
            uint32_t wid = lookup_noun_stem(s + pre_off, base_len, true);
            if (wid != UINT32_MAX && meta_word_len(wid) >= base_len) {
                const WordCandidate wc = read_cand(meta_cand_start(wid));
                size_t wlen = meta_word_len(wid);
                static const uint8_t END_AL[2] = {DIAC_DAMMA, DIAC_KASRA};
                static const uint8_t END_INDEF[4] = {DIAC_DAMMA, DIAC_KASRA, DIAC_DAMMATAN, DIAC_KASRATAN};
                const uint8_t* ends = has_al_p ? END_AL : END_INDEF;
                const size_t n_ends = has_al_p ? 2 : 4;
                for (size_t ei = 0; ei < n_ends; ++ei) {
                    Cand c;
                    std::memset(c.diac, 0, sizeof(c.diac));
                    uint8_t cf = 0;
                    fill_prefix(c, cf);
                    for (size_t k = 0; k + 1 < base_len && k + 1 < wlen; ++k) {
                        c.diac[pre_off + k] = read_core_diac(wc.core_offset, k);
                    }
                    if (has_al_p && is_sun_letter(s[pl])) c.diac[pl] = add_shadda(c.diac[pl]);
                    uint8_t prev_d = (base_len < wlen) ? read_core_diac(wc.core_offset, base_len - 1) : wc.ending_id;
                    c.diac[pre_off + base_len - 1] = diac_has_shadda(prev_d) ? DIAC_SHADDA_FATHA : DIAC_FATHA;
                    c.diac[n - 2] = DIAC_NONE;
                    c.diac[n - 1] = ends[ei];
                    c.flags = cf;
                    c.repl_pos = 255;
                    c.repl_sym = 0;
                    c.emis = static_cast<int32_t>((static_cast<int64_t>(wc.log_prob) * weights_[W_LEX]) / 1000) - weights_[W_SUFFIX_PEN];
                    finalize_cand(c, s, n, tok.si, 1);
                    ctx.cands.push_back(c);
                    ++tok.cand_count;
                }
            }
        }
        // 2. Sound masculine plural (ـون / ـين) and Dual (ـان / ـين)
        if (tok.cand_count == start_count && body_len >= 5 && s[n - 1] == S_NOON &&
            (s[n - 2] == S_WAW || s[n - 2] == S_ALEF || s[n - 2] == S_YEH)) {
            size_t base_len = body_len - 2;
            const bool try_tm = (s[n - 2] != S_WAW);
            uint32_t wid = lookup_noun_stem(s + pre_off, base_len, try_tm);
            if (wid != UINT32_MAX && meta_word_len(wid) == base_len) {
                const WordCandidate wc = read_cand(meta_cand_start(wid));
                const bool stem_sh = diac_has_shadda(wc.ending_id);
                auto emit_pl_dual = [&](uint8_t pre_v, uint8_t mid_v, uint8_t end_v, int32_t extra_pen) {
                    Cand c;
                    std::memset(c.diac, 0, sizeof(c.diac));
                    uint8_t cf = 0;
                    fill_prefix(c, cf);
                    for (size_t k = 0; k + 1 < base_len; ++k) c.diac[pre_off + k] = read_core_diac(wc.core_offset, k);
                    if (has_al_p && is_sun_letter(s[pl])) c.diac[pl] = add_shadda(c.diac[pl]);
                    c.diac[pre_off + base_len - 1] = stem_sh ? add_shadda(pre_v) : pre_v;
                    c.diac[n - 2] = mid_v;
                    c.diac[n - 1] = end_v;
                    c.flags = cf;
                    c.repl_pos = 255;
                    c.repl_sym = 0;
                    c.emis = static_cast<int32_t>((static_cast<int64_t>(wc.log_prob) * weights_[W_LEX]) / 1000) - weights_[W_SUFFIX_PEN] - extra_pen;
                    finalize_cand(c, s, n, tok.si, 1);
                    ctx.cands.push_back(c);
                    ++tok.cand_count;
                };
                if (s[n - 2] == S_WAW) {
                    emit_pl_dual(DIAC_DAMMA, DIAC_NONE, DIAC_FATHA, 0);
                } else if (s[n - 2] == S_ALEF) {
                    emit_pl_dual(DIAC_FATHA, DIAC_NONE, DIAC_KASRA, 0);
                } else {
                    emit_pl_dual(DIAC_KASRA, DIAC_NONE, DIAC_FATHA, 0);
                    emit_pl_dual(DIAC_FATHA, DIAC_SUKUN, DIAC_KASRA, 250);
                }
            }
        }
        // 3. Past verb + subject suffix (ـتُ/ـتَ/ـتِ/ـتْ, ـتُمْ, ـنَا, ـوا) when bare verb stem is in FST
        if (tok.cand_count == start_count && !has_al_p) {
            size_t vpre = (n >= 5 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_LAM)) ? 1 : 0;
            struct VSuf { uint8_t len; uint8_t s0; uint8_t s1; uint8_t pre_d; uint8_t d0; uint8_t d1; };
            static const VSuf VSUFS[] = {
                {2, S_TEH, S_MEEM, DIAC_SUKUN, DIAC_DAMMA, DIAC_SUKUN},
                {2, S_NOON, S_ALEF, DIAC_SUKUN, DIAC_FATHA, DIAC_NONE},
                {2, S_WAW, S_ALEF, DIAC_DAMMA, DIAC_NONE, DIAC_NONE},
                {1, S_TEH, 0, DIAC_SUKUN, DIAC_DAMMA, 0},
                {1, S_TEH, 0, DIAC_FATHA, DIAC_SUKUN, 0},
                {1, S_TEH, 0, DIAC_SUKUN, DIAC_FATHA, 0},
            };
            for (const auto& vs : VSUFS) {
                if (n < vpre + 3 + vs.len) continue;
                if (s[n - vs.len] != vs.s0) continue;
                if (vs.len == 2 && s[n - 1] != vs.s1) continue;
                size_t slen = n - vpre - vs.len;
                uint32_t wid = fst_lookup(s + vpre, slen);
                if (wid == UINT32_MAX || wid >= vocab_count_ || meta_word_len(wid) != slen) continue;
                const WordCandidate wc = read_cand(meta_cand_start(wid));
                if ((meta_role_flags(wid) & MORPH_VERB_LIKE) == 0) continue;
                if (wc.ending_id != DIAC_FATHA && wc.ending_id != DIAC_SHADDA_FATHA) continue;
                if (is_imperf_prefix(s[vpre]) && slen >= 4) continue;
                Cand c;
                std::memset(c.diac, 0, sizeof(c.diac));
                if (vpre == 1) c.diac[0] = DIAC_FATHA; // وَ / فَ / لَ before past verb
                for (size_t k = 0; k + 1 < slen; ++k) c.diac[vpre + k] = read_core_diac(wc.core_offset, k);
                c.diac[vpre + slen - 1] = vs.pre_d;
                c.diac[n - vs.len] = vs.d0;
                if (vs.len == 2) c.diac[n - 1] = vs.d1;
                c.flags = 0;
                c.repl_pos = 255;
                c.repl_sym = 0;
                c.emis = static_cast<int32_t>((static_cast<int64_t>(wc.log_prob) * weights_[W_LEX]) / 1000) - weights_[W_SUFFIX_PEN];
                finalize_cand(c, s, n, tok.si, 0);
                ctx.cands.push_back(c);
                ++tok.cand_count;
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

    int32_t feats[OOV_V8_FEATURES];
    const int32_t stem_len = static_cast<int32_t>(n) - si.al_len;
    const int32_t is_sun_al = (si.al_len >= 2 && si.al_len < n && is_sun_letter(s[si.al_len])) ? 1 : 0;
    const int32_t starts_meem = (si.al_len < n && s[si.al_len] == S_MEEM) ? 1 : 0;
    const int32_t starts_imp = si.verb_like;
    const int32_t ends_tm = (s[n - 1] == S_TEH_MARBUTA) ? 1 : 0;
    const int32_t ends_at = (n >= 4 && s[n - 2] == S_ALEF && s[n - 1] == S_TEH) ? 1 : 0;
    const int32_t ends_wa = (n >= 4 && s[n - 2] == S_WAW && s[n - 1] == S_ALEF) ? 1 : 0;

    // Precompute OOV-V8 morphological pattern features (matching train_oov_v8.py 100%)
    const int32_t st0 = (si.al_len < n) ? static_cast<int32_t>(s[si.al_len]) : -1;
    const int32_t st1 = (static_cast<size_t>(si.al_len) + 1 < n) ? static_cast<int32_t>(s[si.al_len + 1]) : -1;
    const int32_t st2 = (static_cast<size_t>(si.al_len) + 2 < n) ? static_cast<int32_t>(s[si.al_len + 2]) : -1;
    const int32_t penult = (n >= 2) ? static_cast<int32_t>(s[n - 2]) : -1;
    const int32_t antepen = (n >= 3) ? static_cast<int32_t>(s[n - 3]) : -1;
    int32_t sp_pat = 0;
    if (st0 == S_MEEM && stem_len >= 4) {
        if (stem_len >= 6 && st1 == S_SEEN && st2 == S_TEH) sp_pat = 3;
        else if (stem_len >= 5 && st1 == S_TEH) sp_pat = 2;
        else if (stem_len >= 5 && st1 == S_NOON) sp_pat = 4;
        else sp_pat = 1;
    } else if ((st0 == S_ALEF || st0 == S_ALEF_WASLA) && stem_len >= 5) {
        if (stem_len >= 6 && st1 == S_SEEN && st2 == S_TEH) sp_pat = 5;
        else if (st1 == S_NOON) sp_pat = 7;
        else if (st2 == S_TEH) sp_pat = 8;
    } else if (st0 == S_TEH && stem_len >= 5) sp_pat = 6;
    else if ((st0 == S_ALEF_HAMZA_A || st0 == S_ALEF_HAMZA_B) && stem_len >= 4) sp_pat = 9;
    else if ((st0 == S_YEH || st0 == S_TEH || st0 == S_NOON) && stem_len >= 4) sp_pat = 10;

    int32_t ss_pat = 0;
    if (n >= 3) {
        uint8_t l1 = s[n - 1], l2 = s[n - 2];
        if (l1 == S_TEH_MARBUTA) ss_pat = (n >= 4 && l2 == S_YEH) ? 7 : 1;
        else if (n >= 5 && l2 == S_ALEF && l1 == S_TEH) ss_pat = 2;
        else if (n >= 5 && l2 == S_WAW && l1 == S_NOON) ss_pat = 3;
        else if (n >= 5 && l2 == S_YEH && l1 == S_NOON) ss_pat = 4;
        else if (n >= 5 && l2 == S_ALEF && l1 == S_NOON) ss_pat = 5;
        else if (n >= 4 && l1 == S_YEH) ss_pat = 6;
        else if (n >= 5 && ((l2 == S_HEH && (l1 == S_ALEF || l1 == S_MEEM || l1 == S_NOON)) ||
                            (l2 == S_KAF && (l1 == S_MEEM || l1 == S_NOON)) ||
                            (l2 == S_NOON && l1 == S_ALEF))) ss_pat = 8;
        else if (n >= 4 && (l1 == S_HEH || l1 == S_KAF)) ss_pat = 9;
        else if (n >= 4 && ((l2 == S_WAW && l1 == S_ALEF) || (l2 == S_TEH && l1 == S_MEEM) || l1 == S_TEH)) ss_pat = 10;
        else if (n >= 4 && l1 == S_ALEF) ss_pat = 11;
    }
    int32_t loan = 0;
    if (stem_len >= 4) {
        int lv = 0; bool has_gh = false;
        for (size_t k = si.al_len; k < n; ++k) {
            if (s[k] == S_ALEF || s[k] == S_WAW || s[k] == S_YEH) ++lv;
            if (s[k] == S_GHAIN) has_gh = true;
        }
        if (stem_len >= 5 && lv >= 2 && (s[n - 1] == S_WAW || s[n - 1] == S_ALEF || s[n - 1] == S_YEH)) loan = 2;
        else if (stem_len >= 6 && (lv >= 3 || (has_gh && lv >= 2))) loan = 1;
    }
    int32_t pcode = 0;
    if (si.al_len == 0 && n >= 4) {
        if (s[0] == S_WAW) pcode = 1;
        else if (s[0] == S_FEH) pcode = 2;
        else if (s[0] == S_BEH) pcode = 3;
        else if (s[0] == S_LAM) pcode = 4;
        else if (s[0] == S_KAF) pcode = 5;
        else if (s[0] == S_SEEN) pcode = 6;
    }
    auto is_lv_sym = [](uint8_t c) -> bool { return c == S_ALEF || c == S_WAW || c == S_YEH || c == S_ALEF_MAKSURA; };
    auto char_weak_code = [](uint8_t c) -> int32_t {
        if (c == S_ALEF_HAMZA_A) return 1;
        if (c == S_ALEF_HAMZA_B) return 2;
        if (c == S_ALEF_MADDA) return 3;
        if (c == S_WAW_HAMZA) return 4;
        if (c == S_YEH_HAMZA) return 5;
        if (c == S_HAMZA) return 6;
        if (c == S_ALEF || c == S_ALEF_WASLA) return 7;
        if (c == S_WAW) return 8;
        if (c == S_YEH || c == S_ALEF_MAKSURA) return 9;
        if (c == S_TEH_MARBUTA) return 10;
        return 0;
    };

    const bool oov_fast_v5 = oov_trees_.valid() && oov_trees_.has_fast_v5();
    const int32_t w_oov = weights_[W_OOV_TREE];
    alignas(64) int8_t f8[128];
    if (oov_fast_v5) {
        std::memset(f8, 0xFF, 128);
        f8[9] = static_cast<int8_t>(std::min<size_t>(47, n));
        f8[10] = static_cast<int8_t>(si.al_len);
        f8[14] = static_cast<int8_t>(std::min<int32_t>(47, stem_len));
        f8[15] = static_cast<int8_t>(s[n - 1]);
        if (oov_feat_count_ >= OOV_NUM_FEATURES) {
            f8[16] = static_cast<int8_t>(is_sun_al);
            f8[17] = static_cast<int8_t>(starts_meem);
            f8[18] = static_cast<int8_t>(starts_imp);
            f8[19] = static_cast<int8_t>(ends_tm);
            f8[20] = static_cast<int8_t>(ends_at);
            f8[21] = static_cast<int8_t>(ends_wa);
        }
        if (oov_feat_count_ >= OOV_V8_FEATURES) {
            f8[25] = static_cast<int8_t>(st0);
            f8[26] = static_cast<int8_t>(st1);
            f8[27] = static_cast<int8_t>(st2);
            f8[28] = static_cast<int8_t>(penult);
            f8[29] = static_cast<int8_t>(antepen);
            f8[30] = static_cast<int8_t>(sp_pat);
            f8[31] = static_cast<int8_t>(ss_pat);
            f8[34] = static_cast<int8_t>(loan);
            f8[35] = static_cast<int8_t>(pcode);
        }
    }

    for (size_t i = 0; i < n; ++i) {
        const uint16_t mi = mask[i];
        if (i != cpos && (mi & (mi - 1u)) == 0u) {
            base.diac[i] = static_cast<uint8_t>(portable_ctz32(mi));
            continue;
        }
        const uint16_t eval_mask = (i == cpos && n >= 4 && s[n - 1] == S_WAW && cpos == n - 1)
            ? static_cast<uint16_t>(mi | BIT(DIAC_NONE)) : mi;
        int32_t sc[NUM_DIACRITICS];
        if (oov_fast_v5) {
            f8[0] = i >= 3 ? static_cast<int8_t>(s[i - 3]) : -1;
            f8[1] = i >= 2 ? static_cast<int8_t>(s[i - 2]) : -1;
            f8[2] = i >= 1 ? static_cast<int8_t>(s[i - 1]) : -1;
            f8[3] = static_cast<int8_t>(s[i]);
            f8[4] = i + 1 < n ? static_cast<int8_t>(s[i + 1]) : -1;
            f8[5] = i + 2 < n ? static_cast<int8_t>(s[i + 2]) : -1;
            f8[6] = i + 3 < n ? static_cast<int8_t>(s[i + 3]) : -1;
            f8[7] = static_cast<int8_t>(std::min<size_t>(47, i));
            f8[8] = static_cast<int8_t>(std::min<size_t>(47, n - 1 - i));
            f8[11] = static_cast<int8_t>(std::max<int32_t>(-8, std::min<int32_t>(47, static_cast<int32_t>(i) - si.al_len)));
            f8[12] = i >= 1 ? static_cast<int8_t>(base.diac[i - 1]) : -1;
            f8[13] = i >= 2 ? static_cast<int8_t>(base.diac[i - 2]) : -1;
            if (oov_feat_count_ >= OOV_V8_FEATURES) {
                f8[22] = i >= 4 ? static_cast<int8_t>(s[i - 4]) : -1;
                f8[23] = i + 4 < n ? static_cast<int8_t>(s[i + 4]) : -1;
                f8[24] = i >= 3 ? static_cast<int8_t>(base.diac[i - 3]) : -1;
                f8[32] = static_cast<int8_t>(((i >= 1 && is_lv_sym(s[i - 1])) ? 1 : 0) |
                                             ((i + 1 < n && is_lv_sym(s[i + 1])) ? 2 : 0) |
                                             (is_lv_sym(s[i]) ? 4 : 0));
                f8[33] = static_cast<int8_t>(std::max<int32_t>(-8, std::min<int32_t>(47, static_cast<int32_t>(cpos) - static_cast<int32_t>(i))));
                f8[36] = (i >= 1) ? static_cast<int8_t>(diac_family(base.diac[i - 1]) | (diac_has_shadda(base.diac[i - 1]) ? 8 : 0)) : -1;
                f8[37] = static_cast<int8_t>(char_weak_code(s[i]));
            }
            oov_trees_.predict_scores_masked_i8(f8, eval_mask, sc);
            uint32_t sm = eval_mask & ((1u << NUM_DIACRITICS) - 1u);
            while (sm) {
                const uint32_t d = portable_ctz32(sm);
                sm &= (sm - 1u);
                sc[d] = static_cast<int32_t>((static_cast<int64_t>(sc[d]) * w_oov) / 1000);
            }
        } else if (oov_trees_.valid()) {
            for (auto& v : sc) v = 0;
            feats[0] = i >= 3 ? s[i - 3] : -1;
            feats[1] = i >= 2 ? s[i - 2] : -1;
            feats[2] = i >= 1 ? s[i - 1] : -1;
            feats[3] = s[i];
            feats[4] = i + 1 < n ? s[i + 1] : -1;
            feats[5] = i + 2 < n ? s[i + 2] : -1;
            feats[6] = i + 3 < n ? s[i + 3] : -1;
            feats[7] = static_cast<int32_t>(std::min<size_t>(47, i));
            feats[8] = static_cast<int32_t>(std::min<size_t>(47, n - 1 - i));
            feats[9] = static_cast<int32_t>(std::min<size_t>(47, n));
            feats[10] = si.al_len;
            feats[11] = std::max<int32_t>(-8, std::min<int32_t>(47, static_cast<int32_t>(i) - si.al_len));
            feats[12] = i >= 1 ? base.diac[i - 1] : -1;
            feats[13] = i >= 2 ? base.diac[i - 2] : -1;
            feats[14] = std::min<int32_t>(47, stem_len);
            feats[15] = s[n - 1];
            if (oov_feat_count_ >= OOV_NUM_FEATURES) {
                feats[16] = is_sun_al;
                feats[17] = starts_meem;
                feats[18] = starts_imp;
                feats[19] = ends_tm;
                feats[20] = ends_at;
                feats[21] = ends_wa;
            }
            if (oov_feat_count_ >= OOV_V8_FEATURES) {
                feats[22] = i >= 4 ? s[i - 4] : -1;
                feats[23] = i + 4 < n ? s[i + 4] : -1;
                feats[24] = i >= 3 ? base.diac[i - 3] : -1;
                feats[25] = st0;
                feats[26] = st1;
                feats[27] = st2;
                feats[28] = penult;
                feats[29] = antepen;
                feats[30] = sp_pat;
                feats[31] = ss_pat;
                feats[32] = ((i >= 1 && is_lv_sym(s[i - 1])) ? 1 : 0) |
                            ((i + 1 < n && is_lv_sym(s[i + 1])) ? 2 : 0) |
                            (is_lv_sym(s[i]) ? 4 : 0);
                feats[33] = std::max<int32_t>(-8, std::min<int32_t>(47, static_cast<int32_t>(cpos) - static_cast<int32_t>(i)));
                feats[34] = loan;
                feats[35] = pcode;
                feats[36] = (i >= 1) ? static_cast<int32_t>(diac_family(base.diac[i - 1]) | (diac_has_shadda(base.diac[i - 1]) ? 8 : 0)) : -1;
                feats[37] = char_weak_code(s[i]);
            }
            oov_trees_.predict_scores_masked(feats, oov_feat_count_, eval_mask, sc);
            for (auto& v : sc) v = static_cast<int32_t>((static_cast<int64_t>(v) * w_oov) / 1000);
        } else {
            for (auto& v : sc) v = 0;
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
            fst_lookup(s + 1, n - 1) == UINT32_MAX) {
            sc[DIAC_KASRA] += 3 * w_c;
        }
        if (i == 1 && n >= 6 && si.al_len == 0 && si.verb_like == 0 &&
            s[0] == S_WAW && s[1] == S_YEH && base.diac[0] == DIAC_KASRA) {
            sc[DIAC_NONE] += 3 * w_c;
        }
        if (i == cpos) {
            uint32_t sm = eval_mask & ((1u << NUM_DIACRITICS) - 1u);
            while (sm) {
                const uint32_t d = portable_ctz32(sm);
                sm &= (sm - 1u);
                case_scores[d] = sc[d];
            }
            base.diac[i] = DIAC_NONE;
            continue;
        }
        uint8_t best = DIAC_NONE;
        int32_t best_s = NEG_INF;
        uint32_t bm = mi & ((1u << NUM_DIACRITICS) - 1u);
        while (bm) {
            const uint32_t d = portable_ctz32(bm);
            bm &= (bm - 1u);
            if (sc[d] > best_s) { best_s = sc[d]; best = static_cast<uint8_t>(d); }
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
        c.emis = (oov_feat_count_ >= OOV_V8_FEATURES) ? ((ranked[k].first * 2) / 3) : ranked[k].first;
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
        if (n == 3 && (src.flags & CF_SUFFIX) && (tok.surf_flags & SF_INNA_PRON)) continue;

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
            } else if ((src.flags & CF_SUFFIX) && (src.flags & CF_GEN) &&
                       !(src.ext_flags & EF_DIPTOTE) && diac_family(nd) == FAM_KASRA) {
                exp_pen = exp_pen / 3;
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
                !tc.verb_pattern &&
                (tc.diac[n - 1] != DIAC_NONE || ((tc.flags & CF_SUFFIX) && tc.diac[tc.case_pos] != DIAC_NONE)) &&
                !(s[0] == S_BEH && n == 4 && is_masdar_prep_adv(s, n)) &&
                fst_lookup(s + 1, n - 1) != UINT32_MAX) {
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

void Diacritizer::apply_unary_rules(Ctx& ctx, size_t idx) const {
    const Token& tok = ctx.toks[idx];
    const uint32_t cnt = tok.cand_count;
    if (cnt == 0) return;

    const int32_t B = 1500;
    const int32_t w_unary = weights_[W_UNARY_RULE];
    const uint8_t* s = &ctx.syms[tok.off];
    const size_t n = tok.n;
    const uint16_t sf = tok.surf_flags;
    const uint32_t xf = tok.xflags;
    const Token* pt = (idx >= 1 && !tok.boundary_before) ? &ctx.toks[idx - 1] : nullptr;
    const Token* nx = (idx + 1 < ctx.toks.size() && !ctx.toks[idx + 1].boundary_before) ? &ctx.toks[idx + 1] : nullptr;
    const bool nx_wasl = nx && ((nx->surf_flags & SF_WASL) != 0);
    const bool comp_num_pair = (nx && (sf & SF_COMP_NUM) && (nx->surf_flags & SF_ASHAR)) ||
                               (pt && (sf & SF_ASHAR) && (pt->surf_flags & SF_COMP_NUM));
    const bool nx_mudaf_ilayh = nx && nx->si.particle == PART_NONE && nx->si.verb_like < 2 &&
                                nx->top_verb != 1 && !nx->is_adj_like && !nx->is_ibn && !nx->si.has_prep;
    const bool pt_gen_gov = pt && (pt->si.particle == PART_GEN || pt->si.particle == PART_IDAFA_NOUN ||
                                   pt->si.particle == PART_NUM_IDAFA || pt->si.has_prep);
    const bool tok_has_prep_al = (tok.si.has_prep && tok.si.al_len > 0);
    const bool is_kull = (xf & XF_IS_KULL) != 0;
    const bool is_ila_yeh = (xf & XF_IS_ILA_YEH) != 0;
    const bool is_aba = (xf & XF_IS_ABA_VERB) != 0;
    const bool aba_non_verb_kunya = is_aba && pt && (pt->si.particle == PART_GEN || (pt->surf_flags & (SF_IBN | SF_NAME_HEAD)));
    const size_t st_aba = (n == 4) ? 1 : 0;
    const bool is_umar = (xf & XF_IS_UMAR) != 0;
    const size_t st_um = (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_BEH || s[0] == S_LAM)) ? 1 : 0;
    const bool umar_age_ctx = is_umar && ((nx && is_age_ctx_word(&ctx.syms[nx->off], nx->n)) || (nx && nx->digit_after));
    const bool umar_name_ctx = is_umar && !umar_age_ctx && ((nx && (nx->surf_flags & SF_IBN)) || (pt && (pt->xflags & XF_IS_AN_PREP)));
    const bool nx_al = nx && nx->si.al_len > 0 && !nx->si.has_prep && !nx->si.has_conj;
    const bool is_man_w = (sf & SF_MAN) != 0;
    const bool part_iltiqa_ok = (tok.si.particle != PART_NONE &&
                                 (tok.si.particle == PART_GEN || tok.si.particle == PART_JAZM || tok.si.particle == PART_QAD ||
                                  tok.si.particle == PART_CONJ || tok.si.particle == PART_COND || tok.si.particle == PART_QUEST ||
                                  tok.si.particle == PART_IN) &&
                                 s[n - 1] != S_YEH && s[n - 1] != S_ALEF && s[n - 1] != S_ALEF_MAKSURA);
    const bool ams_cond = (sf & SF_AMS) && !(pt && pt->si.particle == PART_JAZM);
    const bool five_noun_cond = (sf & SF_FIVE_NOUN) && nx && nx->si.particle == PART_NONE;
    const bool num_idafa_cond = (tok.si.particle == PART_NUM_IDAFA && nx &&
                                 (nx->si.particle == PART_NONE || nx->si.particle == PART_NUM_IDAFA) &&
                                 nx->si.verb_like < 2 && !(nx->surf_flags & SF_ASHAR));
    const bool baad_qabl_ctx = (sf & SF_BAAD_QABL) &&
                               ((pt && pt->si.particle == PART_AMMA) || !nx ||
                                (nx && ctx.syms[nx->off] == S_FEH && nx->n >= 2 && nx->si.particle != PART_NONE));
    const bool comp_num_left = nx && (sf & SF_COMP_NUM) && (nx->surf_flags & SF_ASHAR);
    const bool comp_num_right = pt && (sf & SF_ASHAR) && (pt->surf_flags & SF_COMP_NUM);
    const bool nx_an_lian = nx && (nx->si.particle == PART_AN || nx->si.particle == PART_LI_AN);

    // Tanween-at-boundary rule preconditions (hoisted once per token!)
    const bool tan_rule_tok = !tok.top_verb && !(sf & (SF_BAAD_QABL | SF_AMS)) &&
                              tok.si.particle == PART_NONE && tok.si.verb_like < 2 &&
                              s[n - 1] != S_ALEF_MAKSURA && s[n - 1] != S_ALEF &&
                              !(pt && (pt->si.particle == PART_LA || pt->si.particle == PART_YA));
    bool tan_end_or_part = false;
    bool tan_has_tan = false;
    if (tan_rule_tok && !tok.digit_after) {
        const bool before_ibn = nx && ((nx->surf_flags & SF_IBN) != 0);
        if (!before_ibn) {
            tan_end_or_part = (idx + 1 == ctx.toks.size()) || ctx.toks[idx + 1].boundary_before ||
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
            if (tan_end_or_part) {
                for (uint32_t k = 0; k < cnt; ++k) {
                    const Cand& oc = ctx.cands[tok.cand_begin + k];
                    if (diac_is_tanween(oc.diac[oc.case_pos])) { tan_has_tan = true; break; }
                }
            }
        }
    }

    // Imperfect mood preconditions (hoisted once per token!)
    const bool starts_imp = (xf & XF_STARTS_IMPERF) != 0;
    size_t st_imp = (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH)) ? 1 : 0;
    if (n - st_imp >= 4 && s[st_imp] == S_SEEN) ++st_imp;
    const bool imp_waw_noon = starts_imp && !(xf & XF_IS_KANA_VERB) && (n - st_imp) >= 6 && (xf & XF_ENDS_WAW_NOON);
    bool imp_governed = false;
    if (starts_imp && !imp_waw_noon) {
        const Token* p2u = (pt && !pt->boundary_before && idx >= 2 && !ctx.toks[idx - 1].boundary_before)
                           ? &ctx.toks[idx - 2] : nullptr;
        auto is_nasb_or_jazm_tok = [](const Token* t) -> bool {
            if (!t) return false;
            uint8_t p = t->si.particle;
            return p == PART_NASB || p == PART_JAZM || p == PART_AN || p == PART_LI_AN ||
                   p == PART_HATTA || p == PART_IN || p == PART_LAMA || p == PART_COND;
        };
        imp_governed = is_nasb_or_jazm_tok(pt) ||
                       (pt && pt->si.particle == PART_LA && (is_nasb_or_jazm_tok(p2u) || s[0] == S_TEH));
    }

    const bool masdar_bos_cond = tok.boundary_before && tok.si.particle == PART_NONE &&
                                 !tok.si.has_prep && (xf & XF_IS_MASDAR_ADV) && nx && nx->si.particle == PART_GEN;

    const bool is_idh_word = pt && pt->si.particle == PART_COND && pt->n <= 3 &&
                             (ctx.syms[pt->off] == S_ALEF_HAMZA_B || ctx.syms[pt->off] == S_ALEF) &&
                             ctx.syms[pt->off + pt->n - 1] == 15;
    const bool clause_head_cond = pt && (pt->si.particle == PART_AMMA || is_idh_word);
    const bool comma_coord_waw = tok.boundary_before && (tok.bkind_mask & BK_COMMA) && !(tok.bkind_mask & BK_HARD) &&
                                 tok.si.has_conj && s[0] == S_WAW && !tok.si.has_prep;
    const bool bos_clause = ((tok.boundary_before && !comma_coord_waw) || clause_head_cond) && tok.si.particle == PART_NONE;
    const bool bos_nx_subj = nx && (nx->is_ibn || nx->si.particle == PART_AN || nx->si.al_len > 0 || (nx->surf_flags & SF_NAME_HEAD));
    const bool bos_nx_noun = nx && (nx->si.al_len > 0 || nx->has_noun_lex);
    const bool feh_al_jawab = !bos_clause && tok.si.has_conj && s[0] == S_FEH && tok.si.al_len >= 3 && !tok.si.has_prep &&
                              pt && pt->si.particle == PART_NONE && diac_family(pt->lex_top_end) != FAM_KASRA;
    const bool idafa_no_tan_cond = (tok.si.particle == PART_IDAFA_NOUN && !tok.digit_after && !(nx && nx->si.particle == PART_GEN));
    const bool lam_nasb_imperf_tail = (n >= 2 && starts_like_imperf(s + 1, n - 1));

    Cand* cptr = &ctx.cands[tok.cand_begin];
    for (uint32_t k = 0; k < cnt; ++k) {
        Cand& c = cptr[k];
        c.lex_emis = static_cast<int16_t>(std::max<int32_t>(-32000, std::min<int32_t>(32000, c.emis)));
        c.morph_sc = 0;
        c.core_bg_sc = 0;

        int32_t r = 0;
        const uint8_t d = c.diac[c.case_pos];
        const uint8_t fam = diac_family(d);
        const bool is_tan = diac_is_tanween(d);

        if ((c.flags & CF_HAS_AL) && is_tan) r -= 4 * B;
        if (idafa_no_tan_cond && is_tan) r -= 2 * B;
        if (c.ext_flags & EF_DIPTOTE) {
            if (is_tan) r -= 3 * B + B / 2;
            if (!(c.flags & CF_HAS_AL) && tok.is_diptote_lex == 2 && fam == FAM_KASRA) r -= 4 * B;
        }
        if ((c.ext_flags & EF_FEM_PL_AT) && c.case_pos == n - 1 && fam == FAM_FATHA) r -= 4 * B;
        if ((c.ext_flags & EF_DUAL_NOON) && (fam == FAM_DAMMA || fam == FAM_FATHA)) r -= 3 * B;
        if ((c.ext_flags & EF_MASC_PL_NOON) && (fam == FAM_KASRA || fam == FAM_DAMMA)) r -= 3 * B;
        if (is_defective_yeh_noun(s, n, c.case_pos, c.diac, c.verb_pattern)) {
            if (fam == FAM_KASRA || fam == FAM_DAMMA || is_tan) r -= 4 * B;
        }

        if ((c.flags & CF_GEN) && c.verb_pattern == 1 && !(c.ext_flags & EF_DIPTOTE)) r -= 4 * B;

        const bool terminal_alef_locked = !(c.flags & CF_SUFFIX) && static_cast<size_t>(c.case_pos) + 1 < n && s[c.case_pos + 1] == S_ALEF;
        if (terminal_alef_locked && s[c.case_pos] == S_YEH && fam == FAM_KASRA) r -= 4 * B;

        if (!terminal_alef_locked && !comp_num_pair &&
            !(sf & SF_INNA_PRON) && ((c.flags & CF_GEN) || tok_has_prep_al) &&
            !is_invar_long_vowel_end(s, n, c.case_pos, c)) {
            if (c.ext_flags & EF_DIPTOTE) {
                if (!(c.flags & CF_HAS_AL)) {
                    const bool proper_dip = (tok.is_diptote_lex == 2) ||
                                            (classify_diptote_stem(s, n, tok.si.al_len, c.diac) == 2);
                    if (proper_dip || !nx_mudaf_ilayh) {
                        if (fam == FAM_FATHA && !is_tan) r += 2 * B;
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
        } else if ((c.ext_flags & EF_DIPTOTE) && !(c.flags & CF_HAS_AL) && pt_gen_gov &&
                   !is_invar_long_vowel_end(s, n, c.case_pos, c)) {
            const bool proper_dip = (tok.is_diptote_lex == 2) ||
                                    (classify_diptote_stem(s, n, tok.si.al_len, c.diac) == 2);
            if (proper_dip || !nx_mudaf_ilayh) {
                if (fam == FAM_FATHA && !is_tan) r += 2 * B;
                else if (fam == FAM_KASRA || is_tan) r -= 2 * B;
            } else {
                if (fam == FAM_KASRA && !is_tan) r += 2 * B;
                else if (fam == FAM_FATHA || is_tan) r -= B;
            }
        }
        if (sf & SF_INNA_PRON) {
            if (d == DIAC_SHADDA_FATHA) r += 3 * B;
            else r -= 4 * B;
            if (static_cast<size_t>(c.case_pos) + 1 < n && s[c.case_pos + 1] == S_HEH) {
                if (c.diac[c.case_pos + 1] == DIAC_DAMMA) r += 2 * B;
                else if (c.diac[c.case_pos + 1] == DIAC_KASRA) r -= 4 * B;
            }
        }
        if (is_kull && fam != FAM_SUKUN) {
            if (diac_has_shadda(d)) r += 3 * B;
            else r -= 4 * B;
        }
        if (is_ila_yeh) {
            if (d == DIAC_SHADDA_FATHA) r += 4 * B;
            else r -= 4 * B;
        }
        if (is_aba) {
            if (aba_non_verb_kunya) {
                if (c.diac[st_aba + 1] == DIAC_KASRA) r += 3 * B;
                else if (c.diac[st_aba + 1] == DIAC_FATHA) r -= 3 * B;
            } else {
                if (c.diac[st_aba + 1] == DIAC_FATHA) r += 2 * B;
                else if (c.diac[st_aba + 1] == DIAC_KASRA) r -= 3 * B;
            }
        }
        if ((c.flags & CF_SUFFIX) && c.case_pos >= 3 && s[0] == S_ALEF_HAMZA_A && c.diac[0] == DIAC_FATHA &&
            c.diac[1] == DIAC_SUKUN && c.diac[2] == DIAC_FATHA) {
            if (d == DIAC_FATHA) r += 2 * B;
            else if (d == DIAC_KASRA) r -= 4 * B;
        }
        if (is_umar && c.diac[st_um] == DIAC_DAMMA) {
            if (umar_age_ctx) {
                if (c.diac[st_um + 1] == DIAC_SUKUN) r += 3 * B;
                else if (c.diac[st_um + 1] == DIAC_FATHA) r -= 2 * B;
            } else if (umar_name_ctx) {
                if (c.diac[st_um + 1] == DIAC_FATHA) r += 2 * B + B / 2;
                else if (c.diac[st_um + 1] == DIAC_SUKUN) r -= 2 * B;
            }
        }
        if (c.flags & CF_LAM_NASB) {
            if (c.verb_pattern == 1 && lam_nasb_imperf_tail) {
                if (fam == FAM_FATHA && !is_tan) r += B + B / 2;
                else if (fam != FAM_NONE) r -= B;
            } else if (c.verb_pattern == 1) {
                r -= 3 * B;
            }
        }
        if (c.flags & CF_FUT) {
            if (d == DIAC_DAMMA) r += B / 2;
            if (is_tan || fam == FAM_KASRA) r -= B;
        }

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
            if (c.case_pos == n - 1 && !nx_wasl) {
                if (d == DIAC_SUKUN) r += B;
                else if (d == DIAC_KASRA) r -= 5 * B;
            }
        } else if (part_iltiqa_ok) {
            if (!nx_wasl && (d == DIAC_KASRA || (c.flags & CF_ILTIQA))) r -= 5 * B;
        } else if (c.flags & CF_ILTIQA) {
            if (!nx_wasl) r -= 5 * B;
        }

        if (ams_cond && !(c.flags & CF_HAS_AL)) {
            if (d == DIAC_KASRA) r += 2 * B;
            else r -= 3 * B;
        }
        if (five_noun_cond && is_tan) r -= 4 * B;
        if (num_idafa_cond && is_tan) r -= 2 * B + B / 2;

        if (baad_qabl_ctx && c.diac[n - 3] == DIAC_FATHA && c.diac[n - 2] == DIAC_SUKUN) {
            if (d == DIAC_DAMMA) r += 2 * B;
            else if (d == DIAC_FATHA) r -= B;
        }
        if (comp_num_left) {
            if (d == DIAC_FATHA) r += 2 * B;
            else r -= 2 * B;
        }
        if (comp_num_right) {
            if (d == DIAC_FATHA) r += 2 * B;
            else r -= 2 * B;
        }
        if (nx_an_lian && (c.ext_flags & EF_PASSIVE)) {
            r += B / 2;
        }

        if (tan_rule_tok && !(c.flags & (CF_HAS_AL | CF_SUFFIX | CF_OOV)) && c.verb_pattern == 0 &&
            !(c.ext_flags & (EF_DIPTOTE | EF_PAST_TEH_FEM | EF_PLURAL_MEEM | EF_DUAL_NOON | EF_MASC_PL_NOON)) &&
            fam != FAM_NONE && fam != FAM_SUKUN) {
            if (tok.digit_after) {
                if (is_tan) r -= 2 * B;
            } else if (tan_has_tan) {
                if (is_tan) r += B;
                else r -= B;
            }
        }

        if ((c.flags & CF_OOV) && tok.si.verb_like == 2) {
            if (fam == FAM_KASRA || is_tan) r -= B;
        }

        if (starts_imp) {
            if (c.verb_pattern == 1 && imp_waw_noon && !(c.flags & CF_HAS_AL)) {
                if (d == DIAC_FATHA) r += B;
                else r -= 3 * B;
            } else if ((c.verb_pattern == 1 || tok.si.particle == PART_KANA) &&
                       c.case_pos == n - 1 && !(c.flags & (CF_SUFFIX | CF_LAM_NASB | CF_HAS_AL))) {
                const uint8_t end_s = s[c.case_pos];
                if (end_s != S_WAW && end_s != S_YEH && end_s != S_ALEF_MAKSURA) {
                    if (!imp_governed) {
                        if (fam == FAM_DAMMA && !is_tan) r += B;
                        else if (fam == FAM_FATHA) r -= B / 2;
                    }
                } else if (end_s == S_YEH && diac_family(c.diac[c.case_pos - 1]) == FAM_KASRA) {
                    if (!imp_governed) {
                        if (d == DIAC_NONE) r += B + B / 2;
                        else if (fam == FAM_FATHA) r -= 2 * B;
                    }
                }
            }
        }

        if (masdar_bos_cond && !(c.flags & (CF_HAS_AL | CF_GEN))) {
            if (fam == FAM_FATHA && is_tan) r += 3 * B;
            else if (fam == FAM_KASRA) r -= 3 * B;
        }

        if (bos_clause) {
            const bool bos_past_before_subj = (c.verb_pattern == 2 && fam == FAM_FATHA && !is_tan && bos_nx_subj);
            const bool looks_bos_noun = (c.verb_pattern == 0) ||
                (c.verb_pattern == 2 && !bos_past_before_subj && bos_nx_noun);
            if (!(c.flags & CF_GEN) && !tok_has_prep_al &&
                looks_bos_noun && tok.top_verb != 1 && tok.si.verb_like < 2 && !(c.ext_flags & EF_PAST_TEH_FEM)) {
                if (fam == FAM_DAMMA) r += (c.ext_flags & EF_DIPTOTE) ? (B + B / 2) : (B / 2);
                else if (fam == FAM_KASRA) r -= B + B / 2;
                else if ((c.ext_flags & EF_DIPTOTE) && tok.boundary_before && fam == FAM_FATHA) r -= B;
            } else if ((c.flags & CF_HAS_AL) && !tok.si.has_prep && !(c.flags & CF_GEN)) {
                if (fam == FAM_DAMMA) r += B / 2;
                else if (fam == FAM_KASRA) r -= B;
            }
            if ((tok.si.verb_like == 2 || c.verb_pattern == 1) && fam == FAM_DAMMA) r += B / 3;
        } else if (feh_al_jawab && !(c.flags & CF_GEN)) {
            if (fam == FAM_DAMMA) r += B / 2;
            else if (fam == FAM_KASRA) r -= B / 2;
        }

        if (r != 0) {
            c.emis += static_cast<int32_t>((static_cast<int64_t>(r) * w_unary) / 1000);
        }
    }
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

void Diacritizer::fill_pair_bin(const Ctx& ctx, const CandMeta* cmeta, uint32_t p_dp_off, uint32_t c_dp_off,
                                const Token* p3, const Token* p2, const Token& pt, const Token& ct, const Token* nx,
                                int32_t* out_bin) const {
    const uint32_t pcnt = pt.cand_count;
    const uint32_t ccnt = ct.cand_count;
    if (ct.boundary_before || pcnt == 0 || ccnt == 0) {
        std::memset(out_bin, 0, static_cast<size_t>(pcnt) * ccnt * sizeof(int32_t));
        return;
    }
    const int32_t B = 1500;
    const int32_t w_rule = weights_[W_RULE];
    const uint8_t* ps = &ctx.syms[pt.off];
    const uint8_t* cs = &ctx.syms[ct.off];
    const uint16_t psf = pt.surf_flags;
    const uint16_t csf = ct.surf_flags;
    const uint32_t pxf = pt.xflags;
    const uint32_t cxf = ct.xflags;

    // Hoist all per-token invariants ONCE for all pcnt * ccnt pairs
    const bool prev_is_man = (psf & SF_MAN) != 0;
    const bool pt_lunar = (pt.si.al_len >= 2 && !(pxf & XF_AL_SUN));
    const bool ct_lunar = (ct.si.al_len >= 2 && !(cxf & XF_AL_SUN));
    const bool ct_sf_ali = (csf & SF_ALI) != 0;
    const bool ct_sf_am = (csf & SF_AM) != 0;
    const bool ct_sf_themma = (csf & SF_THEMMA) != 0;
    const bool ct_sf_ibn = (csf & SF_IBN) != 0;
    const bool pt_is_title = (pxf & XF_IS_TITLE) != 0;
    const bool ct_is_title = (cxf & XF_IS_TITLE) != 0;
    const bool ct_is_time_adv = (cxf & XF_IS_TIME_ADV) != 0;
    const bool pt_is_innama = (pxf & XF_IS_INNAMA) != 0;
    const bool pt_is_decade = (pxf & XF_IS_DECADE) != 0;
    const bool p_non_idafa_mabni = (pxf & XF_NON_IDAFA_MABNI) != 0;
    const bool ct_is_day_week = (cxf & XF_IS_DAY_WEEK) != 0;
    const bool pt_is_dem_det = (pxf & XF_IS_DEM_DET) != 0;
    const bool pt_an_ct_baad = ((pxf & XF_IS_AN_PREP) != 0) && ((cxf & XF_IS_BARE_BAAD) != 0);
    const bool ct_is_aba = (cxf & XF_IS_ABA_VERB) != 0;
    const bool ct_starts_imperf = (cxf & XF_STARTS_IMPERF) != 0;
    const bool ct_ends_waw_alef = (cxf & XF_ENDS_WAW_ALEF) != 0;
    const bool pt_is_laqad = (pxf & XF_IS_LAQAD) != 0;

    const bool nx_is_al = nx && nx->si.al_len > 0 && !nx->si.has_prep;
    const bool nx_is_fem_noun = nx && nx->n >= 3 && (ctx.syms[nx->off + nx->n - 1] == S_TEH_MARBUTA || nx->is_fem_at);
    const bool nx_pass_fathatan = nx && nx->si.particle == PART_NONE && !nx->si.has_prep && nx->si.verb_like < 2 &&
                                  (nx->lex_top_end == DIAC_FATHATAN || nx->lex_top_end == DIAC_SHADDA_FATHATAN);
    const bool nx_mudaf_gen = (ct.is_diptote_lex != 2) && nx && nx->si.particle == PART_NONE &&
                              nx->si.verb_like < 2 && nx->top_verb != 1 &&
                              !nx->is_adj_like && !nx->is_ibn && !nx->si.has_prep;
    const bool nx_dip_non_mudaf = (ct.is_diptote_lex == 2 || !nx || nx->si.particle != PART_NONE || nx->is_adj_like);
    const bool nx_mudaf_tm = nx && nx->si.particle == PART_NONE && !nx->si.has_prep &&
                             (nx->si.al_len > 0 || (nx->surf_flags & SF_NAME_HEAD));
    const bool voc_mudaf = nx && !nx->si.has_prep && !nx->si.has_conj &&
                           (nx->si.al_len > 0 || ((nx->xflags & XF_IS_ALLAH) != 0));
    const bool next_ibn_or_rad = ct_sf_ali && nx && ((nx->surf_flags & SF_IBN) ||
                                 (nx->n >= 3 && ctx.syms[nx->off] == S_REH && ctx.syms[nx->off + 1] == S_DAD));
    const bool next_am_ctx = ct_sf_am && nx && (nx->si.al_len > 0 ||
                             (nx->n >= 3 && ctx.syms[nx->off] == S_WAW && ctx.syms[nx->off + 1] == S_LAM));
    const bool p2_quant_idafa = p2 && ((p2->surf_flags & SF_QUANT_IDAFA) != 0);
    const bool p2_comp_num = p2 && ((p2->surf_flags & SF_COMP_NUM) != 0);
    const bool p2_comp_or_conj = !p2 || (p2->surf_flags & SF_INNA_PRON) ||
                                 p2->si.particle == PART_CONJ || p2->si.particle == PART_LI_AN ||
                                 p2->si.particle == PART_INNA || p2->si.particle == PART_AN ||
                                 p2->si.particle == PART_COND;
    const bool p2_ay_no_gen = !p2 || (p2->si.particle != PART_GEN && p2->si.particle != PART_IDAFA_NOUN &&
                              !p2->si.has_prep && !(p2->si.particle == PART_NONE && p2->si.al_len == 0 && p2->top_verb != 1));
    const bool p2_inna_short = p2 && (p2->si.particle == PART_INNA || p2->si.particle == PART_AN ||
                               p2->si.particle == PART_LI_AN || p2->si.particle == PART_LAKIN) &&
                               (psf & SF_SHORT_PREP_PRON);
    const bool p2_kana_short = p2 && p2->si.particle == PART_KANA && (psf & SF_SHORT_PREP_PRON);
    const bool likely_khabar_plus_adj = p2 && (p2->si.particle == PART_PRON || p2->si.particle == PART_KANA || p2->si.particle == PART_INNA) && ct.is_adj_like;
    const bool p2_not_la_ya = !(p2 && (p2->si.particle == PART_LA || p2->si.particle == PART_YA));
    const bool p2_gen_or_prep = p2 && (p2->si.particle == PART_GEN || p2->si.has_prep);
    const bool p2_gen_idafa_hatta = p2 && (p2->si.particle == PART_GEN || p2->si.particle == PART_IDAFA_NOUN || p2->si.particle == PART_HATTA);
    const bool p2_inna_an_lian = p2 && (p2->si.particle == PART_INNA || p2->si.particle == PART_AN || p2->si.particle == PART_LI_AN);
    const bool anta_taliq_pair = (pt.n == 3 && ps[0] == S_ALEF_HAMZA_A && ps[1] == S_NOON && ps[2] == S_TEH &&
                                  ct.n == 4 && ((cs[0] == S_TAH && cs[1] == S_ALEF && cs[2] == S_LAM && cs[3] == S_QAF) ||
                                                (cs[0] == S_HAH && cs[1] == S_ALEF && cs[2] == S_YEH_HAMZA && cs[3] == S_DAD)));
    const bool p_last_not_alef_maks = (ps[pt.n - 1] != S_ALEF_MAKSURA && ps[pt.n - 1] != S_ALEF);
    const bool c_wf_conj_4 = (ct.n >= 4 && (cs[0] == S_WAW || cs[0] == S_FEH) && !ct.si.has_prep);
    const bool ct_is_meem_al = (ct.n > ct.si.al_len + 3 && cs[ct.si.al_len] == S_MEEM);
    const bool ct_ends_tm = (cs[ct.n - 1] == S_TEH_MARBUTA);
    const bool ct_ends_teh_4 = (ct.n >= 4 && cs[ct.n - 1] == S_TEH);
    const bool ct_ends_teh_5 = (ct.n >= 5 && cs[ct.n - 1] == S_TEH);
    const bool ct_starts_waw_3 = (ct.n >= 3 && cs[0] == S_WAW);
    const bool nx_has_al = nx && nx->si.al_len > 0;

    // Precompute PART_DEM p2 mode
    uint8_t dem_p2_mode = 0; // 1=gen/idafa/mudaf, 2=inna/an, 3=verb, 4=kana
    if (p2) {
        if (pt_is_dem_det) {
            const bool p2_mudaf = (p2->si.particle == PART_NONE && p2->si.al_len == 0 &&
                                   p2->top_verb != 1 && p2->si.verb_like < 2 &&
                                   !diac_is_tanween(p2->lex_top_end) &&
                                   !(p2->surf_flags & (SF_AMS | SF_THEMMA | SF_ASHAR)));
            if (p2->si.particle == PART_GEN || p2->si.particle == PART_IDAFA_NOUN ||
                p2->si.particle == PART_NUM_IDAFA || p2->si.has_prep || p2_mudaf) dem_p2_mode = 1;
            else if (p2->si.particle == PART_INNA || p2->si.particle == PART_AN ||
                     p2->si.particle == PART_LI_AN || p2->si.particle == PART_LAKIN) dem_p2_mode = 2;
            else if (p2->top_verb == 1) dem_p2_mode = 3;
        }
        if (p2->si.particle == PART_KANA) dem_p2_mode |= 8u;
    }

    // Precompute PART_CONJ expected family
    uint8_t conj_p2_fam = FAM_NONE;
    if (p2 && p2->si.particle == PART_NONE && p2->si.verb_like < 2 && !p2->top_verb) {
        if (p3 && (p3->si.particle == PART_GEN || p3->si.particle == PART_IDAFA_NOUN ||
                   p3->si.particle == PART_NUM_IDAFA || p3->si.has_prep)) {
            conj_p2_fam = FAM_KASRA;
        } else if (p2->lex_top_end != 255) {
            uint8_t p2f = diac_family(p2->lex_top_end);
            if (p2f != FAM_NONE && p2f != FAM_SUKUN) conj_p2_fam = p2f;
        }
    }

    // Precompute SF_IBN p2/p3 mode
    uint8_t ibn_mode = 0; // 1=head_fam, 2=gen, 3=inna, 0=p_noun fallback
    uint8_t ibn_head_fam = FAM_NONE;
    if (ct_sf_ibn) {
        if (p2 && (p2->surf_flags & SF_NAME_HEAD)) {
            ibn_mode = 1;
            ibn_head_fam = diac_family(p2->lex_top_end);
            const uint8_t* p2s = &ctx.syms[p2->off];
            size_t p2st = (p2->n >= 4 && (p2s[0] == S_WAW || p2s[0] == S_FEH || p2s[0] == S_BEH || p2s[0] == S_LAM)) ? 1 : 0;
            if (p2->n - p2st == 3 && p2s[p2st] == S_ALEF_HAMZA_A && p2s[p2st + 1] == S_BEH) {
                if (p2s[p2st + 2] == S_WAW) ibn_head_fam = FAM_DAMMA;
                else if (p2s[p2st + 2] == S_YEH) ibn_head_fam = FAM_KASRA;
                else if (p2s[p2st + 2] == S_ALEF) ibn_head_fam = FAM_FATHA;
            } else if (p3 && (p3->si.particle == PART_GEN || p3->si.particle == PART_IDAFA_NOUN ||
                              p3->si.has_prep) && !(p3->surf_flags & SF_MAN)) {
                ibn_head_fam = FAM_KASRA;
            } else if (p3 && (p3->si.particle == PART_INNA || p3->si.particle == PART_AN ||
                              p3->si.particle == PART_LI_AN)) {
                ibn_head_fam = FAM_FATHA;
            }
        } else if (p2 && (p2->si.particle == PART_GEN || p2->si.particle == PART_IDAFA_NOUN || p2->si.has_prep) &&
                   !(p2->surf_flags & SF_MAN)) {
            ibn_mode = 2;
        } else if (p2 && (p2->si.particle == PART_INNA || p2->si.particle == PART_AN || p2->si.particle == PART_LI_AN)) {
            ibn_mode = 3;
        }
    }

    struct BinP {
        uint8_t pd, pf, pp, pos_tag, flags, ext_flags, vp;
        bool ptan, p_al, p_suk, p_man_pron, p_noun, p_verb, p_has_vobj, p_past_fem_kas;
        bool p_sh, p_lama_sh;
        int32_t anta_r;
    };
    BinP pinfo[32];
    const uint32_t p_lim = std::min<uint32_t>(pcnt, 32u);
    for (uint32_t p = 0; p < p_lim; ++p) {
        const Cand& pc = ctx.cands[pt.cand_begin + p];
        const CandMeta& pm = cmeta[p_dp_off + p];
        BinP& bp = pinfo[p];
        bp.pd = pm.ce;
        bp.pf = pm.cf;
        bp.ptan = (pm.bits & CMB_TAN) != 0;
        bp.pp = (pm.bits & CMB_ALI_PROP) ? static_cast<uint8_t>(PART_NONE) : pt.si.particle;
        bp.p_al = (pm.bits & CMB_HAS_AL) != 0;
        bp.p_suk = (pc.ext_flags & EF_LAM_SUKUN) != 0;
        bp.p_man_pron = prev_is_man && pc.diac[pt.n - 2] == DIAC_FATHA;
        bp.pos_tag = pc.pos_tag;
        bp.flags = pc.flags;
        bp.ext_flags = pc.ext_flags;
        bp.vp = pc.verb_pattern;
        const bool p_known_nonverb = (pt.word_id != UINT32_MAX && pc.verb_pattern == 0 && pt.top_verb == 0);
        bp.p_noun = (pc.verb_pattern == 0) && (pt.si.verb_like < 2 || p_known_nonverb) && !(pc.flags & CF_FUT) &&
                    bp.pp != PART_IDAFA_NOUN && bp.pp != PART_NUM_IDAFA;
        bp.p_verb = (pc.verb_pattern == 1) || (pt.si.verb_like == 2 && !p_known_nonverb && !bp.p_al);
        bp.p_has_vobj = (pm.bits & CMB_HAS_VOBJ) != 0;
        bp.p_past_fem_kas = ((pc.ext_flags & EF_PAST_TEH_FEM) != 0) && pc.diac[pt.n - 1] == DIAC_KASRA;
        bp.p_sh = diac_has_shadda(bp.pd);
        bp.p_lama_sh = (bp.pp == PART_LAMA && pt.n >= 2) ? diac_has_shadda(pc.diac[pt.n - 2]) : false;
        bp.anta_r = 0;
        if (anta_taliq_pair) {
            if (pc.diac[2] == DIAC_KASRA) bp.anta_r = 3 * B;
            else if (pc.diac[2] == DIAC_FATHA) bp.anta_r = -2 * B;
        }
    }

    for (uint32_t c = 0; c < ccnt; ++c) {
        const Cand& cc = ctx.cands[ct.cand_begin + c];
        const CandMeta& cm = cmeta[c_dp_off + c];
        const uint8_t cd = cm.ce;
        const uint8_t cf = cm.cf;
        const bool ctan = (cm.bits & CMB_TAN) != 0;
        const uint8_t cp = (cm.bits & CMB_ALI_PROP) ? static_cast<uint8_t>(PART_NONE) : ct.si.particle;
        const bool c_al = (cm.bits & CMB_HAS_AL) != 0;
        const uint8_t vmd = cc.verb_mood_diac;
        const uint8_t vmf = diac_family(vmd);
        const bool vmtan = diac_is_tanween(vmd);
        const bool c_is_idafa_head = (cp == PART_IDAFA_NOUN || cp == PART_NUM_IDAFA);
        const bool c_part = cp != PART_NONE && !c_is_idafa_head;
        const bool c_verb = (cc.verb_pattern == 1) || (cc.verb_pattern == 2 && ct.top_verb == 1) ||
                            (ct.si.verb_like == 2 && (ct.word_id == UINT32_MAX || ct.top_verb == 1) &&
                             !c_al && !ctan && cf != FAM_KASRA && vmf != FAM_KASRA && !vmtan);
        const bool c_imperf = ((cc.verb_pattern == 1) && ct_starts_imperf) ||
                              (ct.si.verb_like == 2 && !c_al && !c_part && !ctan && !vmtan &&
                               cf != FAM_KASRA && vmf != FAM_KASRA && !ct_ends_tm);
        const bool c_noun = !c_part && !c_verb && cc.verb_pattern != 2 && !(cc.flags & (CF_GEN | CF_LAM_NASB | CF_FUT));
        const bool c_own_gen = (cm.bits & CMB_HAS_GEN) != 0;
        const bool c_diptote = (cc.ext_flags & EF_DIPTOTE) != 0;
        const bool c_fem_at = (cc.ext_flags & EF_FEM_PL_AT) != 0;
        const bool c_suk = (cc.ext_flags & EF_LAM_SUKUN) != 0;
        const bool c_inv_end = (cm.bits & CMB_INVAR_LV) != 0;
        const bool c_terminal_alef_locked = !(cc.flags & CF_SUFFIX) && cc.case_pos + 1 < ct.n && cs[cc.case_pos + 1] == S_ALEF;
        const bool c_meem_participle = ct_is_meem_al && cc.morph_class == MC_NOUN_SHADDA_CORE;

        // Terms that depend ONLY on (ct, cc, p2, pt_tok, nx) — computed ONCE per c!
        int32_t r_c_only = 0;
        if (cc.verb_pattern == 1 && nx) {
            if ((cc.ext_flags & EF_PASSIVE) && nx_pass_fathatan) r_c_only -= B + B / 2;
            if (ct_ends_teh_4 && (cc.ext_flags & EF_PAST_TEH_FEM)) {
                if (nx_is_fem_noun) r_c_only += 2 * B + B / 2;
                else if (nx_is_al) r_c_only += B;
            } else if (ct_ends_teh_5 && cc.pos_tag == POS_VERB_PAST_SUBJ && nx_is_fem_noun) {
                r_c_only -= 2 * B;
            }
        }
        if (p2_inna_short && c_noun && !c_al && cf == FAM_FATHA) r_c_only += B + B / 2;
        if (p2_kana_short && c_noun && cf == FAM_DAMMA) r_c_only += B + B / 2;

        bool c_is_ala = false, c_is_ali_yya = false, c_is_ali_prop = false;
        if (ct_sf_ali) {
            c_is_ala = (csf & SF_ALA) && cc.diac[ct.n - 1] == DIAC_NONE;
            c_is_ali_yya = !c_is_ala && ct.n >= 3 && cc.diac[ct.n - 2] == DIAC_FATHA && diac_has_shadda(cd);
            c_is_ali_prop = !c_is_ala && ct.n >= 3 && cc.diac[ct.n - 2] == DIAC_KASRA;
            if (next_ibn_or_rad && ctan) r_c_only -= 3 * B;
        }
        const bool c_is_um = ct_sf_am && diac_has_shadda(cd) && cc.diac[0] == DIAC_DAMMA;
        const bool c_tail_ok = !c_part && !c_own_gen && !(cc.flags & CF_LAM_NASB);

        for (uint32_t p = 0; p < p_lim; ++p) {
            const BinP& bp = pinfo[p];
            int32_t r = r_c_only + bp.anta_r;

            // 1. Register & Lunar AL sukun consistency
            if (c_al) {
                if (bp.p_al && pt_lunar && ct_lunar) {
                    r += (bp.p_suk == c_suk) ? (B / 2) : (-B / 2);
                }
                if (prev_is_man && !bp.p_man_pron && ct_lunar) {
                    if (c_suk && bp.pd == DIAC_FATHA) r += B;
                    else if (!c_suk && bp.pd == DIAC_SUKUN) r += B / 2;
                }
                if (bp.p_past_fem_kas && c_suk) r += B / 2;
            }

            // 2. على vs عليّ and أم vs أمّ
            if (ct_sf_ali) {
                const bool proper_ctx = next_ibn_or_rad || bp.pp == PART_GEN || (psf & SF_IBN) ||
                                        (bp.vp == 1 && !c_is_ala) || pt_is_title;
                if (proper_ctx) {
                    if (c_is_ali_prop) {
                        r += 2 * B + B / 2;
                        if (bp.pf != FAM_NONE && bp.pf != FAM_SUKUN && pt_is_title && cf == bp.pf) r += B;
                    } else if (c_is_ali_yya || c_is_ala) {
                        r -= 3 * B;
                    }
                }
            }
            if (c_is_um && (bp.pp == PART_KANA || next_am_ctx)) {
                r += B + B / 2;
            }

            // 3. من (مَنْ vs مِنْ) and من ثمَّ
            if (prev_is_man) {
                if (ct_sf_themma) {
                    r += (!bp.p_man_pron && cc.diac[0] == DIAC_FATHA) ? (4 * B) : (-4 * B);
                }
                if (bp.p_man_pron && !ct_sf_themma) {
                    if (c_verb || c_imperf || cp == PART_JAZM || cp == PART_KANA || cp == PART_QAD || cp == PART_LA) r += 2 * B;
                    else if (c_noun) {
                        if (cp == PART_IDAFA_NOUN || cp == PART_NUM_IDAFA || cf == FAM_FATHA || c_al) r -= 4 * B;
                        else if (cf == FAM_KASRA || p2_quant_idafa) {
                            r -= p2_quant_idafa ? (4 * B) : (2 * B);
                        }
                    }
                } else {
                    if (c_verb || c_imperf) r -= 2 * B;
                    else if (c_noun) {
                        if (cc.ext_flags & (EF_MASC_PL_NOON | EF_DUAL_NOON)) {
                            r += B;
                        } else if (c_inv_end) {
                            if (cf == FAM_NONE) r += B;
                        } else if (c_diptote && !c_al) {
                            if (nx_mudaf_gen) {
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

            // 5. Particle government
            if (!c_own_gen && !bp.p_man_pron && bp.pp != PART_NONE) {
                switch (bp.pp) {
                    case PART_GEN:
                        if (c_verb && !ct_starts_waw_3) r -= 3 * B;
                        if (pt_an_ct_baad) {
                            if (cc.diac[0] == DIAC_DAMMA && cf == FAM_KASRA) r += 4 * B;
                            else r -= 4 * B;
                        } else if (c_noun && !c_terminal_alef_locked) {
                            if (cc.ext_flags & (EF_MASC_PL_NOON | EF_DUAL_NOON)) {
                                r += B;
                            } else if (c_diptote && !c_al && !nx_mudaf_gen) {
                                if (cf == FAM_FATHA && !ctan) r += 2 * B;
                                else if (cf == FAM_KASRA || ctan) r -= B + B / 2;
                            } else if (c_diptote && !c_al && nx_mudaf_gen) {
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
                            } else if (c_diptote && !c_al && nx_dip_non_mudaf) {
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
                        if (bp.pp == PART_NUM_IDAFA && (psf & SF_ASHAR) && p2_comp_num) {
                            if (c_noun && !c_part && !c_al && !(cc.flags & CF_SUFFIX) && !c_terminal_alef_locked) {
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
                                } else if (c_diptote && !c_al && nx_dip_non_mudaf) {
                                    if (cf == FAM_FATHA && !ctan) r += B + B / 2;
                                    else r -= B;
                                } else {
                                    if (cf == FAM_KASRA) r += B + B / 2;
                                    else if (cf != FAM_NONE) r -= B;
                                }
                            }
                            if (bp.ptan) r -= 2 * B;
                        }
                        break;

                    case PART_AY: {
                        const bool ay_sh = bp.p_sh;
                        const bool next_is_indef_gen_target = c_noun && !c_al && !c_part && !ct.si.has_prep && cf == FAM_KASRA;
                        if (c_verb || cp == PART_GEN || cp == PART_CONJ || cp == PART_LA || cp == PART_AN ||
                            cp == PART_INNA || cp == PART_PREP_PRON || c_al ||
                            (c_noun && cf == FAM_DAMMA && (!nx || nx->si.particle == PART_NONE))) {
                            if (!ay_sh && bp.pd == DIAC_SUKUN) r += 2 * B;
                            else if (!ay_sh) r -= B;
                        }
                        if (next_is_indef_gen_target) {
                            if (ay_sh) r += 2 * B;
                            else r -= B + B / 2;
                        } else if (ay_sh && c_noun && !c_al && !c_part && (cf == FAM_DAMMA || cf == FAM_FATHA)) {
                            r -= B;
                        }
                        if (ay_sh && bp.pf == FAM_KASRA && p2_ay_no_gen) {
                            r -= 3 * B;
                        }
                        break;
                    }

                    case PART_LAMA:
                        if (c_verb && !c_imperf) {
                            if (bp.p_lama_sh) r += p2_comp_or_conj ? (2 * B) : (B + B / 2);
                            else if (p2_comp_or_conj) r -= 2 * B;
                        } else if (c_imperf || cp == PART_JAZM) {
                            if (bp.p_lama_sh) r += B;
                        } else if (cp == PART_AN || cp == PART_GEN || cp == PART_PREP_PRON) {
                            if (!bp.p_lama_sh) r += B + B / 2;
                        }
                        break;

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
                        if (c_noun && !c_part && !(csf & SF_BAAD_QABL)) {
                            if (cf == FAM_DAMMA) r += B + B / 2;
                            else if (cf == FAM_KASRA) r -= 2 * B;
                        }
                        break;

                    case PART_AN:
                    case PART_LI_AN: {
                        const bool an_sh = bp.p_sh;
                        const bool next_verbish = c_imperf || c_verb || cp == PART_LA || cp == PART_JAZM ||
                                                  cp == PART_KANA || cp == PART_QAD || ct_ends_waw_alef;
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
                        const bool in_sh = bp.p_sh;
                        const bool next_verbish = c_imperf || c_verb || cp == PART_JAZM || cp == PART_NASB ||
                                                  cp == PART_COND || cp == PART_QAD || ct_ends_waw_alef;
                        if (bp.pp == PART_LAKIN && (next_verbish || cp == PART_GEN || c_own_gen)) {
                            r += (!in_sh) ? (2 * B) : (-2 * B);
                        } else if (c_noun || cp == PART_DEM || cp == PART_PRON || cp == PART_REL ||
                                   cp == PART_PREP_PRON || (bp.pp == PART_INNA && cp == PART_GEN)) {
                            if (in_sh) r += (c_noun && !c_al && ctan && cf == FAM_FATHA) ? (3 * B) : (2 * B);
                            else if (bp.pp == PART_LAKIN) r -= 2 * B;
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
                        const bool in_sh = bp.p_sh;
                        const bool next_verbish = c_imperf || c_verb || cp == PART_KANA || cp == PART_LA ||
                                                  cp == PART_JAZM || cp == PART_QAD || ct_is_aba ||
                                                  ct_ends_waw_alef ||
                                                  (cc.verb_pattern == 2 && cf == FAM_FATHA && !ctan && !c_al);
                        if (next_verbish) {
                            if (!in_sh) {
                                r += 2 * B;
                                if ((c_imperf || c_verb) && vmf == FAM_SUKUN) r += B;
                            } else {
                                r -= 2 * B + B / 2;
                            }
                        } else if (c_noun || cp == PART_DEM || cp == PART_PRON || cp == PART_PREP_PRON || cp == PART_GEN ||
                                   (csf & SF_INNA_PRON)) {
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

                    case PART_QAD:
                        if (c_imperf || c_verb) {
                            if (!pt_is_laqad && c_imperf && !(cs[0] == S_TEH && ct.n >= 4 && cc.diac[1] == DIAC_FATHA) &&
                                vmf == FAM_DAMMA && !vmtan) {
                                r += B / 2;
                            }
                            if (vmf == FAM_KASRA || vmtan) r -= B;
                        }
                        break;

                    case PART_YA:
                        if (c_noun && !c_al) {
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
                        if (pt_is_dem_det && c_al && c_noun && p2) {
                            const uint8_t dm = dem_p2_mode & 7u;
                            if (dm == 1) {
                                if (cf == FAM_KASRA) r += B + B / 2;
                                else if (cf != FAM_NONE) r -= B;
                            } else if (dm == 2) {
                                if (cf == FAM_FATHA || (c_fem_at && cf == FAM_KASRA)) r += B + B / 2;
                            } else if (dm == 3) {
                                if (cf == FAM_FATHA || cf == FAM_DAMMA) r += B / 2;
                                else if (cf == FAM_KASRA && !c_fem_at) r -= B;
                            }
                        } else if (!c_al && c_noun && (dem_p2_mode & 8u) && ctan) {
                            if (cf == FAM_FATHA) r += B + B / 2;
                        }
                        break;

                    case PART_CONJ:
                        if (c_noun && conj_p2_fam != FAM_NONE && cf == conj_p2_fam) r += B;
                        break;

                    default:
                        break;
                }
            }

            // 6. Content-word pairs (Verb/Noun -> Noun)
            if (c_tail_ok && (bp.pp == PART_NONE || bp.pp == PART_IDAFA_NOUN || bp.pp == PART_NUM_IDAFA)) {
                if (pt_is_innama && c_noun) {
                    if (cf == FAM_DAMMA) r += B + B / 2;
                    else if (cf == FAM_KASRA) r -= 2 * B;
                }
                if ((psf & SF_AMS) && c_noun && ct_is_day_week) {
                    if (cf == FAM_FATHA) r += 2 * B;
                    else if (cf == FAM_KASRA) r -= 2 * B;
                }
                if (pt_is_decade && c_noun && !c_al && !(cc.flags & CF_SUFFIX) &&
                    !(cc.ext_flags & (EF_FEM_PL_AT | EF_MASC_PL_NOON | EF_DUAL_NOON)) && !c_diptote && !c_terminal_alef_locked) {
                    if (cf == FAM_FATHA && (nx_mudaf_tm || ctan)) r += B + B / 2;
                    else if (cf == FAM_KASRA && ctan) r -= B;
                }

                if (bp.p_verb && c_noun) {
                    if (bp.pos_tag == POS_VERB_PAST_SUBJ || bp.pos_tag == POS_VERB_IMPERF_12) {
                        if (c_fem_at) {
                            if (cf == FAM_KASRA) r += B + B / 2;
                            else if (cf == FAM_FATHA) r -= 3 * B;
                            else if (cf == FAM_DAMMA) r -= B;
                        } else {
                            if (cf == FAM_FATHA) r += B + B / 2;
                            else if (cf == FAM_DAMMA) r -= B;
                            else if (cf == FAM_KASRA) r -= 3 * B;
                        }
                    } else if (bp.p_has_vobj) {
                        if (!ct_is_time_adv) {
                            if (cf == FAM_DAMMA) r += 2 * B + B / 2;
                            else if (cf == FAM_FATHA) r -= 2 * B;
                        }
                        if (cf == FAM_KASRA) r -= 4 * B;
                    } else if (bp.pos_tag == POS_VERB_PASS) {
                        if (!ct_is_time_adv) {
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
                        if ((c_al || ct_is_title) && !ct_is_time_adv) {
                            if (cf == FAM_DAMMA) r += B;
                            else if (cf == FAM_KASRA) r -= B;
                        } else if ((bp.ext_flags & EF_PAST_TEH_FEM) && c_al && (ct_ends_tm || bp.pd == DIAC_KASRA)) {
                            if (cf == FAM_DAMMA) r += B;
                            else if (cf == FAM_KASRA) r -= B;
                        }
                    }
                }
                if ((psf & SF_INNA_PRON) && c_noun && !c_own_gen) {
                    if (cf == FAM_DAMMA) r += B + B / 2;
                    else if (cf == FAM_KASRA) r -= 2 * B;
                }
                if (bp.p_noun && c_noun && !c_al && pt_is_title && !ct_sf_ibn &&
                    bp.pf != FAM_NONE && bp.pf != FAM_SUKUN && cf != FAM_NONE && cf != FAM_SUKUN) {
                    r += (cf == bp.pf) ? (2 * B) : (-(B + B / 2));
                }

                if (ct_sf_ibn) {
                    if (ctan) r -= 3 * B;
                    if (bp.ptan) r -= 3 * B;
                    if (ibn_mode == 1) {
                        if (ibn_head_fam != FAM_NONE && ibn_head_fam != FAM_SUKUN) {
                            r += (cf == ibn_head_fam) ? (2 * B) : (-(B + B / 2));
                        }
                    } else if (ibn_mode == 2) {
                        r += (cf == FAM_KASRA) ? (B + B / 2) : (-(B + B / 2));
                    } else if (ibn_mode == 3) {
                        r += (cf == FAM_FATHA) ? (B + B / 2) : (-B);
                    } else if (bp.p_noun && bp.pf != FAM_NONE && bp.pf != FAM_SUKUN) {
                        r += (cf == bp.pf) ? (B + B / 2) : (-B);
                    }
                }

                if ((psf & SF_FIVE_NOUN) && c_noun) {
                    if (bp.ptan) r -= 3 * B;
                    if (c_diptote && !c_al) {
                        if (cf == FAM_FATHA && !ctan) r += B + B / 2;
                    } else if (cf == FAM_KASRA) {
                        r += B + B / 2;
                    } else if (cf != FAM_NONE) {
                        r -= B;
                    }
                }

                if (bp.p_noun) {
                    if (!bp.p_al && !(bp.flags & CF_SUFFIX) && !p_non_idafa_mabni && c_al && c_noun) {
                        if (ct.si.particle != PART_REL) {
                            if (cf == FAM_KASRA) {
                                r += B + B / 2;
                                if (bp.ptan && !(bp.ext_flags & EF_DIPTOTE)) r -= 2 * B;
                            } else {
                                if (cf != FAM_NONE) r -= B / 2;
                                if (bp.ptan) r -= B + B / 2;
                            }
                        }
                    }

                    if (!bp.p_al && !bp.ptan && !(bp.flags & (CF_SUFFIX | CF_EXPANDED)) && !p_non_idafa_mabni &&
                        !pt.top_verb && !likely_khabar_plus_adj &&
                        !(bp.ext_flags & (EF_DIPTOTE | EF_DUAL_NOON | EF_MASC_PL_NOON)) &&
                        bp.pf != FAM_NONE && bp.pf != FAM_SUKUN && p_last_not_alef_maks && p2_not_la_ya &&
                        c_noun && !c_al && !ct_sf_ibn && !ct.is_adj_like) {
                        if (c_diptote && nx_dip_non_mudaf) {
                            if (cf == FAM_FATHA && !ctan) r += B;
                        } else {
                            if (cf == FAM_KASRA) r += B;
                            else if (cf != FAM_NONE) r -= B / 2;
                        }
                    }

                    if (!bp.p_al && !(bp.flags & CF_SUFFIX) && !p_non_idafa_mabni && c_noun && !c_al && !(cc.flags & CF_SUFFIX) &&
                        (ct.is_adj_like || c_meem_participle) && !c_fem_at) {
                        if (bp.ptan && bp.pf == cf && ctan) r += 2 * B;
                        else if (bp.ptan && bp.pf != cf && cf == FAM_KASRA && bp.pf != FAM_KASRA) r -= 2 * B;
                        else if (!bp.ptan && ctan && bp.pf == cf && !(bp.ext_flags & EF_DIPTOTE)) r -= B;
                        else if (!bp.ptan && cf == FAM_KASRA && bp.pf != FAM_KASRA) r -= B;
                    }

                    if (!bp.p_al && !(bp.flags & CF_SUFFIX) && !(bp.ext_flags & EF_DIPTOTE) && bp.ptan &&
                        c_noun && !c_al && !ct.is_adj_like && !(cc.flags & CF_OOV) && cf == FAM_KASRA && !ctan && nx_has_al) {
                        r -= B + B / 2;
                    }

                    if (bp.p_al && c_al && c_noun && bp.pf == FAM_NONE) {
                        if ((bp.flags & CF_GEN) || pt.si.has_prep || p2_gen_idafa_hatta) {
                            if (cf == FAM_KASRA) r += B + B / 2;
                            else if (cf != FAM_NONE) r -= B;
                        } else if (p2_inna_an_lian) {
                            if (cf == FAM_FATHA || (c_fem_at && cf == FAM_KASRA)) r += B;
                        }
                    }

                    if (bp.p_al && c_al && c_noun && bp.pf != FAM_NONE && bp.pf != FAM_SUKUN) {
                        if (cf == bp.pf) r += B + B / 3;
                        else if (!((bp.ext_flags & EF_FEM_PL_AT) && bp.pf == FAM_KASRA && cf == FAM_FATHA)) r -= B / 2;
                    }

                    if (!bp.p_al && bp.ptan && !c_al && c_noun && ctan) {
                        r += (cf == bp.pf) ? B : (-B / 2);
                    }

                    if ((bp.ext_flags & EF_DIPTOTE) && !bp.p_al && bp.pf == FAM_FATHA && !c_al && c_noun && ctan) {
                        if (p2_gen_or_prep) {
                            if (cf == FAM_KASRA) r += B;
                        } else {
                            if (cf == FAM_FATHA) r += B;
                        }
                    }

                    if ((bp.flags & CF_SUFFIX) && c_al && c_noun && bp.pf != FAM_NONE && bp.pf != FAM_SUKUN) {
                        if (cf == bp.pf) r += B;
                    }

                    if ((c_noun || cp == PART_IDAFA_NOUN) && c_wf_conj_4 &&
                        bp.pf != FAM_NONE && bp.pf != FAM_SUKUN && (bp.p_al == c_al || (bp.ptan && ctan))) {
                        if (cf == bp.pf) r += B + B / 2;
                    }
                }
            }

            out_bin[p * ccnt + c] = (r == 0) ? 0 : static_cast<int32_t>((static_cast<int64_t>(r) * w_rule) / 1000);
        }
    }
}

std::string Diacritizer::diacritize(const char* text, size_t len) const {
    return diacritize_internal(text, len, nullptr, nullptr, nullptr, nullptr, nullptr);
}

void Diacritizer::diacritize_lines(const std::string* in_lines, std::string* out_lines, size_t count, int num_threads) const {
    if (!in_lines || !out_lines || count == 0) return;
    int nt = num_threads;
    if (nt <= 0) nt = static_cast<int>(std::thread::hardware_concurrency());
    if (nt <= 1 || count < 8) {
        for (size_t i = 0; i < count; ++i) out_lines[i] = diacritize(in_lines[i].data(), in_lines[i].size());
        return;
    }
    nt = static_cast<int>(std::min<size_t>(static_cast<size_t>(nt), count));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(nt));
    for (int t = 0; t < nt; ++t) {
        const size_t s = (count * static_cast<size_t>(t)) / static_cast<size_t>(nt);
        const size_t e = (count * static_cast<size_t>(t + 1)) / static_cast<size_t>(nt);
        workers.emplace_back([this, in_lines, out_lines, s, e]() {
            for (size_t i = s; i < e; ++i) out_lines[i] = diacritize(in_lines[i].data(), in_lines[i].size());
        });
    }
    for (auto& w : workers) w.join();
}

std::string Diacritizer::diacritize_internal(
    const char* text, size_t len,
    std::vector<Irab2DumpSample>* dump_out,
    const std::vector<std::array<uint8_t, MAX_WORD_LEN>>* gold_diacs,
    std::vector<Core2DumpSample>* core2_dump_out,
    uint32_t* core2_group_id,
    std::vector<V9DumpToken>* v9_dump_out) const {
    if (!is_loaded() || !text || len == 0) return text ? std::string(text, len) : std::string();

    struct Span {
        bool is_word;
        uint32_t a;
        uint32_t b;
    };
    constexpr size_t CAND_CACHE_SETS = 8192u;
    constexpr size_t CAND_CACHE_WAYS = 2u;
    constexpr size_t CAND_POOL_CAP = 131072u;
    struct CandCacheEntry {
        const Diacritizer* owner = nullptr;
        uint64_t epoch = 0;
        uint64_t gen = 0;
        uint32_t hash = 0;
        uint32_t pool_off = 0;
        uint32_t word_id = UINT32_MAX;
        uint32_t xflags = 0;
        uint16_t surf_flags = 0;
        uint8_t n = 0;
        uint8_t cand_count = 0;
        uint8_t role = 0;
        uint8_t lex_top_end = 255;
        uint8_t top_verb = 0;
        uint8_t has_noun_lex = 0;
        uint8_t is_fem_at = 0;
        uint8_t is_diptote_lex = 0;
        uint8_t is_ibn = 0;
        uint8_t is_adj_like = 0;
        uint8_t top_pos = POS_UNK;
        SurfaceInfo si;
        uint8_t syms[MAX_WORD_LEN];
    };
    struct CoreBgCacheSlot {
        uint8_t queried_mask = 0; // bit0=L, bit1=R, bit2=C
        uint8_t hit_mask = 0;     // bit0=L, bit1=R, bit2=C
        CoreBgLookupResult el{};
        CoreBgLookupResult er{};
        CoreBgLookupResult ec{};
    };
    struct CoreBgJob {
        uint32_t tok_i;
        uint32_t cc_lim;
        uint64_t kl, kr, kc;
        uint8_t qm;
        uint16_t ch[MAX_CANDS];
    };
    struct NgKeyJob {
        uint64_t k5, k4, k3, kr, kc, ks;
        uint8_t mask; // bit0=5g, bit1=4g, bit2=3g, bit3=rbg, bit4=ctr3, bit5=skip1, bit6=bg
        uint32_t bg_prev;
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
        // 2-way 16384-slot pool-backed candidate cache
        std::vector<CandCacheEntry> cand_cache;
        std::vector<uint8_t> cand_cache_lru;
        std::vector<Cand> cand_pool;
        uint32_t cand_pool_head = 0;
        uint64_t cand_pool_gen = 1;
        // Precomputed per-token valency, per-candidate meta, and cached binary rule scores
        std::vector<ValencyInfo> tok_val;
        std::vector<CandMeta> cmeta;
        std::vector<uint32_t> pair_off;
        std::vector<int32_t> pair_bin;
        std::vector<CoreBgJob> cbg_jobs;
        std::vector<int32_t> irab2_bonus;
        std::vector<int32_t> p1_irab1_scores;
        std::vector<int8_t> p1_irab1_best;
        std::vector<int8_t> p1_bg_best;
        std::vector<int16_t> p1_bg_val;
        std::vector<int8_t> p1_rbg_best;
        std::vector<int16_t> p1_rbg_val;
        std::vector<int8_t> p1_ng_best;
        std::vector<int8_t> p1_skip_best;
        // Reusable buffers eliminating all per-sentence heap allocations
        std::vector<CoreBgCacheSlot> cbg_cache;
        std::vector<uint8_t> cg_arr;
        std::vector<int8_t> dg_arr;
        std::vector<uint8_t> pic_arr;
        std::vector<uint8_t> ric_arr;
        std::vector<uint8_t> core2_locked;
        std::vector<uint8_t> c2_pos, c2_role, c2_fam, c2_mc;
        std::vector<uint8_t> c2_al, c2_gen, c2_verb, c2_pass, c2_tan, c2_suf;
        std::vector<int8_t> prev_vdist, next_vdist;
        std::vector<uint32_t> new_chosen;
        std::vector<uint8_t> p1_end, p1_fam, p1_tan, p1_pos, p1_role;
        std::vector<uint8_t> p1_al, p1_gen, p1_suf, p1_verb, p1_fem, p1_st3, p1_multi;
        std::vector<uint8_t> f_depth, f_gov, f_outer, f_ppfl;
        std::vector<uint8_t> f_idafa, f_hfam, f_mfam, f_pfam, f_npm, f_gvt, f_pic;
        std::vector<int8_t> f_dgov, f_gvo;
        std::vector<uint8_t> r_dverb, r_dprep, r_dend, r_flags;
        std::vector<uint32_t> p2_opts;
        std::vector<uint32_t> p2_opts_off;
        std::vector<uint32_t> p2_ce_mask;
        std::vector<uint8_t> p2_need_irab2;
        // Compact 128-byte feature buffers + sentence-batched Morph/Irab1/Core2/Irab2 scratch buffers
        std::vector<uint32_t> p1_irab1_mask;
        std::vector<int32_t> p1_irab1_best_sc;
        std::vector<std::array<int8_t, 128>> p1_irab1_feats;
        std::vector<uint32_t> i1_tok_idx;
        std::vector<uint32_t> i1_ce_mask;
        std::vector<uint32_t> i1_cls_words[NUM_DIACRITICS];
        std::vector<int32_t> i1_scores_flat;
        std::vector<std::array<int8_t, 128>> m_feat_buf;
        std::vector<uint32_t> m_tok_idx;
        std::vector<uint32_t> m_cls_words[NUM_DIACRITICS];
        std::vector<int32_t> m_scores_flat;
        struct Core2Slice {
            uint32_t tok_i;
            uint32_t vec_off;
            int32_t n_grp;
            int32_t p1_g;
            uint32_t rep_k[8];
            uint16_t core_h[8];
        };
        std::vector<Core2Slice> c2_slices;
        std::vector<std::array<int8_t, 128>> c2_feat_buf;
        std::vector<int32_t> c2_logits;
        std::vector<uint32_t> c2_active_vecs;
        std::vector<uint32_t> c2_active_slices;
        std::vector<std::array<int8_t, 128>> i2_feat_buf;
        std::vector<uint32_t> i2_tok_idx;
        std::vector<uint32_t> i2_cls_words[NUM_DIACRITICS];
        std::vector<int32_t> i2_scores_flat;
        std::vector<NeuralIrabTokInput> v9_in;
        NeuralIrabWorkspace v9_ws;
    };
    thread_local Scratch sc_buf;
    if (sc_buf.cand_cache.size() != CAND_CACHE_SETS * CAND_CACHE_WAYS) {
        sc_buf.cand_cache.resize(CAND_CACHE_SETS * CAND_CACHE_WAYS);
        sc_buf.cand_cache_lru.assign(CAND_CACHE_SETS, 0);
        sc_buf.cand_pool.resize(CAND_POOL_CAP);
        sc_buf.cand_pool_head = 0;
        sc_buf.cand_pool_gen = 1;
    }
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
        const size_t set_idx = static_cast<size_t>(h & (CAND_CACHE_SETS - 1u));
        const size_t base_slot = set_idx * CAND_CACHE_WAYS;
        CandCacheEntry* hit = nullptr;
        uint8_t hit_way = 0;
        for (uint8_t w = 0; w < CAND_CACHE_WAYS; ++w) {
            CandCacheEntry& e = sc_buf.cand_cache[base_slot + w];
            if (e.owner == this && e.epoch == cache_epoch_ && e.gen == sc_buf.cand_pool_gen &&
                e.hash == h && e.n == t.n && std::memcmp(e.syms, ts, t.n) == 0) {
                hit = &e;
                hit_way = w;
                break;
            }
        }
        if (hit) {
            sc_buf.cand_cache_lru[set_idx] = static_cast<uint8_t>(1u - hit_way);
            t.cand_begin = static_cast<uint32_t>(ctx.cands.size());
            t.cand_count = hit->cand_count;
            t.word_id = hit->word_id;
            t.role = hit->role;
            t.lex_top_end = hit->lex_top_end;
            t.top_verb = hit->top_verb;
            t.has_noun_lex = hit->has_noun_lex;
            t.is_fem_at = hit->is_fem_at;
            t.is_diptote_lex = hit->is_diptote_lex;
            t.is_ibn = hit->is_ibn;
            t.is_adj_like = hit->is_adj_like;
            t.top_pos = hit->top_pos;
            t.surf_flags = hit->surf_flags;
            t.xflags = hit->xflags;
            t.si = hit->si;
            if (hit->cand_count > 0) {
                const size_t old_sz = ctx.cands.size();
                ctx.cands.resize(old_sz + hit->cand_count);
                std::memcpy(&ctx.cands[old_sz], &sc_buf.cand_pool[hit->pool_off],
                            static_cast<size_t>(hit->cand_count) * sizeof(Cand));
            }
        } else {
            build_candidates(ctx, t);
            if (t.cand_count > 0) {
                t.top_verb = ctx.cands[t.cand_begin].verb_pattern;
                t.top_pos = ctx.cands[t.cand_begin].pos_tag;
            }
            t.xflags = compute_extra_surf_flags(ts, t.n, t.si, t.surf_flags);
            if (t.n <= MAX_WORD_LEN && t.cand_count <= 32u) {
                if (sc_buf.cand_pool_head + t.cand_count > CAND_POOL_CAP) {
                    sc_buf.cand_pool_head = 0;
                    ++sc_buf.cand_pool_gen;
                }
                CandCacheEntry& e0 = sc_buf.cand_cache[base_slot];
                CandCacheEntry& e1 = sc_buf.cand_cache[base_slot + 1u];
                uint8_t vic = sc_buf.cand_cache_lru[set_idx];
                if (e0.owner != this || e0.epoch != cache_epoch_ || e0.gen != sc_buf.cand_pool_gen) vic = 0;
                else if (e1.owner != this || e1.epoch != cache_epoch_ || e1.gen != sc_buf.cand_pool_gen) vic = 1;
                CandCacheEntry& slot = sc_buf.cand_cache[base_slot + vic];
                slot.owner = this;
                slot.epoch = cache_epoch_;
                slot.gen = sc_buf.cand_pool_gen;
                slot.hash = h;
                slot.pool_off = sc_buf.cand_pool_head;
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
                slot.xflags = t.xflags;
                slot.si = t.si;
                slot.cand_count = static_cast<uint8_t>(t.cand_count);
                if (t.cand_count > 0) {
                    std::memcpy(&sc_buf.cand_pool[sc_buf.cand_pool_head], &ctx.cands[t.cand_begin],
                                static_cast<size_t>(t.cand_count) * sizeof(Cand));
                    sc_buf.cand_pool_head += t.cand_count;
                }
                sc_buf.cand_cache_lru[set_idx] = static_cast<uint8_t>(1u - vic);
            }
        }
    }

    const size_t W = ctx.toks.size();
    for (size_t i = 0; i < W; ++i) {
        apply_unary_rules(ctx, i);
    }

    if (morph_trees_.valid() && weights_[W_MORPH_TREE] != 0) {
        const uint32_t morph_nc = morph_trees_.num_classes();
        const int32_t w_morph = weights_[W_MORPH_TREE];
        if (morph_trees_.has_fast_v5()) {
            sc_buf.m_feat_buf.clear();
            sc_buf.m_tok_idx.clear();
            for (uint32_t c = 0; c < morph_nc; ++c) sc_buf.m_cls_words[c].clear();
            for (size_t i = 0; i < W; ++i) {
                const Token& t = ctx.toks[i];
                if (t.cand_count < 2) continue;
                uint8_t first_mc = ctx.cands[t.cand_begin].morph_class;
                uint32_t mc_mask = (first_mc < morph_nc) ? (1u << first_mc) : 0u;
                bool multi_mc = false;
                for (uint32_t k = 1; k < t.cand_count; ++k) {
                    uint8_t mc = ctx.cands[t.cand_begin + k].morph_class;
                    if (mc != first_mc) multi_mc = true;
                    if (mc < morph_nc) mc_mask |= (1u << mc);
                }
                if (!multi_mc || mc_mask == 0u) continue;
                const uint8_t* s = &ctx.syms[t.off];
                const Token* p = (i >= 1 && !t.boundary_before) ? &ctx.toks[i - 1] : nullptr;
                const Token* p2 = (p && i >= 2 && !p->boundary_before) ? &ctx.toks[i - 2] : nullptr;
                const Token* nx = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? &ctx.toks[i + 1] : nullptr;
                const Token* nx2 = (nx && i + 2 < W && !ctx.toks[i + 2].boundary_before) ? &ctx.toks[i + 2] : nullptr;
                const uint32_t wi = static_cast<uint32_t>(sc_buf.m_feat_buf.size());
                sc_buf.m_feat_buf.resize(wi + 1u);
                sc_buf.m_tok_idx.push_back(static_cast<uint32_t>(i));
                int8_t* mf = sc_buf.m_feat_buf[wi].data();
                mf[0] = p2 ? static_cast<int8_t>(p2->si.particle) : -1;
                mf[1] = p ? static_cast<int8_t>(p->si.particle) : -1;
                mf[2] = static_cast<int8_t>(t.si.particle);
                mf[3] = nx ? static_cast<int8_t>(nx->si.particle) : -1;
                mf[4] = nx2 ? static_cast<int8_t>(nx2->si.particle) : -1;
                mf[5] = p ? static_cast<int8_t>(p->si.al_len) : -1;
                mf[6] = static_cast<int8_t>(t.si.al_len);
                mf[7] = nx ? static_cast<int8_t>(nx->si.al_len) : -1;
                mf[8] = nx2 ? static_cast<int8_t>(nx2->si.al_len) : -1;
                mf[9] = p ? static_cast<int8_t>(p->si.has_prep) : -1;
                mf[10] = static_cast<int8_t>(t.si.has_prep);
                mf[11] = nx ? static_cast<int8_t>(nx->si.has_prep) : -1;
                mf[12] = p ? static_cast<int8_t>(p->si.verb_like) : -1;
                mf[13] = static_cast<int8_t>(t.si.verb_like);
                mf[14] = nx ? static_cast<int8_t>(nx->si.verb_like) : -1;
                mf[15] = (p && p->lex_top_end != 255) ? static_cast<int8_t>(p->lex_top_end) : -1;
                mf[16] = (t.lex_top_end != 255) ? static_cast<int8_t>(t.lex_top_end) : -1;
                mf[17] = (nx && nx->lex_top_end != 255) ? static_cast<int8_t>(nx->lex_top_end) : -1;
                mf[18] = p ? static_cast<int8_t>(p->top_verb) : -1;
                mf[19] = static_cast<int8_t>(t.top_verb);
                mf[20] = nx ? static_cast<int8_t>(nx->top_verb) : -1;
                mf[21] = p ? static_cast<int8_t>(p->top_pos) : -1;
                mf[22] = static_cast<int8_t>(t.top_pos);
                mf[23] = nx ? static_cast<int8_t>(nx->top_pos) : -1;
                mf[24] = static_cast<int8_t>(t.n);
                mf[25] = static_cast<int8_t>(s[0]);
                mf[26] = static_cast<int8_t>(s[std::min<size_t>(t.si.al_len, t.n - 1)]);
                mf[27] = t.n >= 2 ? static_cast<int8_t>(s[t.n - 2]) : -1;
                mf[28] = static_cast<int8_t>(s[t.n - 1]);
                mf[29] = static_cast<int8_t>(t.boundary_before);
                mf[30] = nx ? static_cast<int8_t>(ctx.syms[nx->off]) : -1;
                mf[31] = nx ? static_cast<int8_t>(ctx.syms[nx->off + nx->n - 1]) : -1;
                std::memset(mf + MORPH_NUM_FEATURES, 0xFF, 128 - MORPH_NUM_FEATURES);
                uint32_t m = mc_mask;
                while (m) {
                    const uint32_t c = portable_ctz32(m);
                    m &= (m - 1u);
                    sc_buf.m_cls_words[c].push_back(wi);
                }
            }
            const size_t n_mact = sc_buf.m_tok_idx.size();
            if (n_mact > 0) {
                sc_buf.m_scores_flat.resize(n_mact * NUM_DIACRITICS);
                for (uint32_t c = 0; c < morph_nc; ++c) {
                    if (!sc_buf.m_cls_words[c].empty()) {
                        morph_trees_.eval_class_batch_i8(
                            c, sc_buf.m_feat_buf.data(),
                            sc_buf.m_cls_words[c].data(), sc_buf.m_cls_words[c].size(),
                            sc_buf.m_scores_flat.data(), NUM_DIACRITICS);
                    }
                }
                for (size_t wi = 0; wi < n_mact; ++wi) {
                    const Token& t = ctx.toks[sc_buf.m_tok_idx[wi]];
                    const int32_t* sc = &sc_buf.m_scores_flat[wi * NUM_DIACRITICS];
                    for (uint32_t k = 0; k < t.cand_count; ++k) {
                        Cand& c = ctx.cands[t.cand_begin + k];
                        if (c.morph_class < morph_nc) {
                            int32_t m_bon = static_cast<int32_t>((static_cast<int64_t>(sc[c.morph_class]) * w_morph) / 1000);
                            c.morph_sc = static_cast<int16_t>(std::max<int32_t>(-32000, std::min<int32_t>(32000, m_bon)));
                            c.emis += m_bon;
                        }
                    }
                }
            }
        } else {
            int32_t mf[MORPH_NUM_FEATURES];
            for (size_t i = 0; i < W; ++i) {
                const Token& t = ctx.toks[i];
                if (t.cand_count < 2) continue;
                uint8_t first_mc = ctx.cands[t.cand_begin].morph_class;
                uint32_t mc_mask = (first_mc < morph_nc) ? (1u << first_mc) : 0u;
                bool multi_mc = false;
                for (uint32_t k = 1; k < t.cand_count; ++k) {
                    uint8_t mc = ctx.cands[t.cand_begin + k].morph_class;
                    if (mc != first_mc) multi_mc = true;
                    if (mc < morph_nc) mc_mask |= (1u << mc);
                }
                if (!multi_mc || mc_mask == 0u) continue;
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
                morph_trees_.predict_scores_masked(mf, MORPH_NUM_FEATURES, mc_mask, sc);
                for (uint32_t k = 0; k < t.cand_count; ++k) {
                    Cand& c = ctx.cands[t.cand_begin + k];
                    if (c.morph_class < morph_nc) {
                        int32_t m_bon = static_cast<int32_t>((static_cast<int64_t>(sc[c.morph_class]) * w_morph) / 1000);
                        c.morph_sc = static_cast<int16_t>(std::max<int32_t>(-32000, std::min<int32_t>(32000, m_bon)));
                        c.emis += m_bon;
                    }
                }
            }
        }
    }

    // V7 Lexical Core-Disambiguation Table (left bigram + right bigram + centered trigram for homographs / passive / من / إن / أن)
    const bool cache_cbg_for_c2 = (core2_dump_out != nullptr) || (is_v8_ && core2_trees_.valid() && v8_weights_[V8W_CORE2] != 0);
    if (cache_cbg_for_c2) {
        sc_buf.cbg_cache.assign(W, CoreBgCacheSlot{});
    }
    if (is_v7_ && !core_bg_.empty() &&
        (v7_weights_[V7W_CORE_BG_L] != 0 || v7_weights_[V7W_CORE_BG_R] != 0 || v7_weights_[V7W_CORE_BG_C] != 0)) {
        const int32_t w_cbl = v7_weights_[V7W_CORE_BG_L];
        const int32_t w_cbr = v7_weights_[V7W_CORE_BG_R];
        const int32_t w_cbc = v7_weights_[V7W_CORE_BG_C];
        if (core_bg_.is_compact()) {
            sc_buf.cbg_jobs.clear();
            for (size_t i = 0; i < W; ++i) {
                const Token& t = ctx.toks[i];
                if (t.word_id == UINT32_MAX || t.cand_count < 2) continue;
                const uint8_t* ts = &ctx.syms[t.off];
                const uint8_t lam_skip = (t.si.al_len >= 2) ? t.si.al_lam_pos : 255;
                uint16_t ch0 = compute_core_hash(ts, ctx.cands[t.cand_begin].diac, ctx.cands[t.cand_begin].case_pos, lam_skip);
                bool multi_core = false;
                CoreBgJob job;
                job.ch[0] = ch0;
                const uint32_t cc_lim = std::min<uint32_t>(t.cand_count, MAX_CANDS);
                for (uint32_t k = 1; k < cc_lim; ++k) {
                    const Cand& ck = ctx.cands[t.cand_begin + k];
                    uint16_t chk = compute_core_hash(ts, ck.diac, ck.case_pos, lam_skip);
                    job.ch[k] = chk;
                    if (chk != ch0) multi_core = true;
                }
                if (!multi_core) continue;
                const Token* p = (i >= 1 && !t.boundary_before) ? &ctx.toks[i - 1] : nullptr;
                const Token* nx = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? &ctx.toks[i + 1] : nullptr;
                uint8_t qm = 0;
                if (w_cbl != 0 && p && p->word_id != UINT32_MAX) {
                    uint32_t ws[2] = {p->word_id, t.word_id};
                    job.kl = NgramTableV6::fnv1a_key_tagged(ws, 2, static_cast<uint8_t>(0x80u | CORE_DIR_LEFT));
                    core_bg_.prefetch_bucket(job.kl);
                    qm |= 1u;
                }
                if (w_cbr != 0 && nx && nx->word_id != UINT32_MAX) {
                    uint32_t ws[2] = {t.word_id, nx->word_id};
                    job.kr = NgramTableV6::fnv1a_key_tagged(ws, 2, static_cast<uint8_t>(0x80u | CORE_DIR_RIGHT));
                    core_bg_.prefetch_bucket(job.kr);
                    qm |= 2u;
                }
                if (w_cbc != 0 && p && nx && p->word_id != UINT32_MAX && nx->word_id != UINT32_MAX) {
                    uint32_t ws[3] = {p->word_id, t.word_id, nx->word_id};
                    job.kc = NgramTableV6::fnv1a_key_tagged(ws, 3, static_cast<uint8_t>(0x80u | CORE_DIR_CTR3));
                    core_bg_.prefetch_bucket(job.kc);
                    qm |= 4u;
                }
                if (qm == 0u) continue;
                job.tok_i = static_cast<uint32_t>(i);
                job.cc_lim = cc_lim;
                job.qm = qm;
                sc_buf.cbg_jobs.push_back(job);
            }
            for (const auto& job : sc_buf.cbg_jobs) {
                if (job.qm & 1u) core_bg_.prefetch_entry_range(job.kl);
                if (job.qm & 2u) core_bg_.prefetch_entry_range(job.kr);
                if (job.qm & 4u) core_bg_.prefetch_entry_range(job.kc);
            }
            for (const auto& job : sc_buf.cbg_jobs) {
                CoreBgLookupResult el_v{}, er_v{}, ec_v{};
                const bool has_el = (job.qm & 1u) && core_bg_.find_val_compact_key(job.kl, el_v);
                const bool has_er = (job.qm & 2u) && core_bg_.find_val_compact_key(job.kr, er_v);
                const bool has_ec = (job.qm & 4u) && core_bg_.find_val_compact_key(job.kc, ec_v);
                if (cache_cbg_for_c2) {
                    CoreBgCacheSlot& cs = sc_buf.cbg_cache[job.tok_i];
                    cs.queried_mask = job.qm;
                    cs.hit_mask = static_cast<uint8_t>((has_el ? 1u : 0u) | (has_er ? 2u : 0u) | (has_ec ? 4u : 0u));
                    cs.el = el_v;
                    cs.er = er_v;
                    cs.ec = ec_v;
                }
                if (!has_el && !has_er && !has_ec) continue;
                const Token& t = ctx.toks[job.tok_i];
                for (uint32_t k = 0; k < job.cc_lim; ++k) {
                    Cand& c = ctx.cands[t.cand_begin + k];
                    uint16_t h = job.ch[k];
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
                    c.core_bg_sc = static_cast<int16_t>(std::max<int32_t>(-32000, std::min<int32_t>(32000, bon)));
                    c.emis += bon;
                }
            }
        } else {
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
                uint8_t qm = 0, hm = 0;
                if (w_cbl != 0 && p && p->word_id != UINT32_MAX) {
                    uint32_t ws[2] = {p->word_id, t.word_id};
                    has_el = core_bg_.find_val(CORE_DIR_LEFT, ws, 2, el_v);
                    qm |= 1u;
                    if (has_el) hm |= 1u;
                }
                if (w_cbr != 0 && nx && nx->word_id != UINT32_MAX) {
                    uint32_t ws[2] = {t.word_id, nx->word_id};
                    has_er = core_bg_.find_val(CORE_DIR_RIGHT, ws, 2, er_v);
                    qm |= 2u;
                    if (has_er) hm |= 2u;
                }
                if (w_cbc != 0 && p && nx && p->word_id != UINT32_MAX && nx->word_id != UINT32_MAX) {
                    uint32_t ws[3] = {p->word_id, t.word_id, nx->word_id};
                    has_ec = core_bg_.find_val(CORE_DIR_CTR3, ws, 3, ec_v);
                    qm |= 4u;
                    if (has_ec) hm |= 4u;
                }
                if (cache_cbg_for_c2) {
                    CoreBgCacheSlot& cs = sc_buf.cbg_cache[i];
                    cs.queried_mask = qm;
                    cs.hit_mask = hm;
                    cs.el = el_v;
                    cs.er = er_v;
                    cs.ec = ec_v;
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
                    c.core_bg_sc = static_cast<int16_t>(std::max<int32_t>(-32000, std::min<int32_t>(32000, bon)));
                    c.emis += bon;
                }
            }
        }
    }

    const bool use_v9_neural = is_v9_ && neural_irab_.valid() && (v9_flags_ & V9F_NEURAL_IRAB) != 0 && v9_dump_out == nullptr;
    if (v9_dump_out != nullptr || use_v9_neural) {
        sc_buf.v9_in.resize(W);
        std::vector<int32_t>& seg_end_dist = sc_buf.dp;
        seg_end_dist.resize(W);
        int32_t cur_end = static_cast<int32_t>(W);
        for (int32_t ii = static_cast<int32_t>(W) - 1; ii >= 0; --ii) {
            seg_end_dist[static_cast<size_t>(ii)] = cur_end - 1 - ii;
            if (ctx.toks[static_cast<size_t>(ii)].boundary_before && (ctx.toks[static_cast<size_t>(ii)].bkind_mask & BK_HARD)) {
                cur_end = ii;
            }
        }
        int32_t cur_start = 0;
        for (size_t i = 0; i < W; ++i) {
            const Token& t = ctx.toks[i];
            const bool is_hard_start = (i == 0) || (t.boundary_before && (t.bkind_mask & BK_HARD));
            if (is_hard_start) cur_start = static_cast<int32_t>(i);
            NeuralIrabTokInput& vi = sc_buf.v9_in[i];
            std::memset(&vi, 0, sizeof(vi));
            vi.word_id = t.word_id;
            vi.part_id = t.si.particle < 32 ? t.si.particle : 0;
            vi.pos_id = t.top_pos < 16 ? t.top_pos : 0;
            vi.hard_boundary = is_hard_start ? 1 : 0;

            const uint8_t* s = &ctx.syms[t.off];
            const size_t n = t.n;
            const size_t st = std::min<size_t>(t.si.al_len, n);
            const size_t slen = n - st;
            auto h32_arr = [](const uint8_t* p, size_t blen) -> uint16_t {
                uint32_t h = 2166136261u;
                for (size_t k = 0; k < blen; ++k) { h ^= p[k]; h *= 16777619u; }
                return static_cast<uint16_t>((h >> 16) ^ (h & 0xFFFFu));
            };
            {
                uint8_t b0[5] = {t.si.al_len, t.si.has_prep, t.si.has_conj, n > 0 ? s[0] : uint8_t(0), n > 1 ? s[1] : uint8_t(0)};
                uint8_t b1[3] = {static_cast<uint8_t>(n), n >= 2 ? s[n - 2] : uint8_t(0), n >= 1 ? s[n - 1] : uint8_t(0)};
                uint8_t b2[4] = {static_cast<uint8_t>(slen), st < n ? s[st] : uint8_t(0), st + slen / 2 < n ? s[st + slen / 2] : uint8_t(0), n >= 1 ? s[n - 1] : uint8_t(0)};
                vi.hashes[0] = h32_arr(b0, 5);
                vi.hashes[1] = h32_arr(b1, 3);
                vi.hashes[2] = h32_arr(b2, 4);
                vi.hashes[3] = static_cast<uint16_t>(fnv1a_32_syms(s, n) & 0xFFFFu);
            }

            float max_b[8];
            for (int c = 0; c < 8; ++c) max_b[c] = -1e9f;
            uint8_t amask = 0;
            uint8_t has_sh_end = 0;
            for (uint32_t k = 0; k < t.cand_count; ++k) {
                const Cand& ck = ctx.cands[t.cand_begin + k];
                uint8_t ce = ck.diac[ck.case_pos];
                uint8_t base_e = (ce >= DIAC_SHADDA) ? static_cast<uint8_t>(ce - DIAC_SHADDA) : ce;
                if (ce >= DIAC_SHADDA) has_sh_end = 1;
                if (base_e < 8) {
                    float sc_v = static_cast<float>(ck.emis) / 1000.0f;
                    if (sc_v > max_b[base_e]) max_b[base_e] = sc_v;
                }
                uint8_t fm = diac_family(ce);
                if (fm == FAM_DAMMA) amask |= 1;
                else if (fm == FAM_FATHA) amask |= 2;
                else if (fm == FAM_KASRA) amask |= 4;
                else if (fm == FAM_SUKUN) amask |= 8;
                if (diac_is_tanween(ce)) amask |= 16;
            }
            float mx_all = -1e9f;
            for (int c = 0; c < 8; ++c) if (max_b[c] > mx_all) mx_all = max_b[c];
            if (mx_all > -1e8f) {
                float sm = 0.0f;
                for (int c = 0; c < 8; ++c) {
                    float ev = (max_b[c] > -1e8f) ? std::exp(std::max(-20.0f, max_b[c] - mx_all)) : 0.0f;
                    vi.dense[c] = ev;
                    sm += ev;
                }
                if (sm > 0.0f) for (int c = 0; c < 8; ++c) vi.dense[c] /= sm;
            }
            for (int c = 0; c < 5; ++c) vi.dense[8 + c] = (amask & (1u << c)) ? 1.0f : 0.0f;
            vi.dense[13] = has_sh_end ? 1.0f : 0.0f;

            const int32_t d_eos = seg_end_dist[i];
            const bool is_eos = (d_eos == 0);
            const bool is_cl_end = (i + 1 == W) || ctx.toks[i + 1].boundary_before;
            vi.dense[14] = is_hard_start ? 1.0f : 0.0f;
            vi.dense[15] = is_eos ? 1.0f : 0.0f;
            vi.dense[16] = is_cl_end ? 1.0f : 0.0f;
            vi.dense[17] = (d_eos == 1) ? 1.0f : 0.0f;
            vi.dense[18] = (d_eos == 2) ? 1.0f : 0.0f;
            vi.dense[19] = static_cast<float>(std::min<int32_t>(d_eos, 16)) / 16.0f;
            vi.dense[20] = static_cast<float>(std::min<int32_t>(static_cast<int32_t>(i) - cur_start, 16)) / 16.0f;
            vi.dense[21] = (t.bkind_mask & BK_COMMA) ? 1.0f : 0.0f;

            const bool has_suf = (t.cand_count > 0 && (ctx.cands[t.cand_begin].flags & CF_SUFFIX) != 0);
            const bool is_oov = (t.cand_count > 0 && (ctx.cands[t.cand_begin].flags & CF_OOV) != 0);
            const uint8_t last_s = n > 0 ? s[n - 1] : 0;
            uint8_t need_pron = 0;
            if (n >= 2 && (last_s == S_KAF || last_s == S_HEH)) {
                for (uint32_t k = 0; k < t.cand_count; ++k) {
                    if ((ctx.cands[t.cand_begin + k].flags & CF_SUFFIX) != 0) {
                        need_pron = 1;
                        break;
                    }
                }
            }
            vi.need_pron = need_pron;
            vi.dense[22] = (t.si.al_len > 0) ? 1.0f : 0.0f;
            vi.dense[23] = t.si.has_prep ? 1.0f : 0.0f;
            vi.dense[24] = t.si.has_conj ? 1.0f : 0.0f;
            vi.dense[25] = has_suf ? 1.0f : 0.0f;
            vi.dense[26] = is_oov ? 1.0f : 0.0f;
            vi.dense[27] = t.is_fem_at ? 1.0f : 0.0f;
            vi.dense[28] = (t.is_diptote_lex == 1) ? 1.0f : 0.0f;
            vi.dense[29] = (t.is_diptote_lex == 2) ? 1.0f : 0.0f;
            vi.dense[30] = t.is_ibn ? 1.0f : 0.0f;
            vi.dense[31] = t.is_adj_like ? 1.0f : 0.0f;
            vi.dense[32] = t.top_verb ? 1.0f : 0.0f;
            vi.dense[33] = t.has_noun_lex ? 1.0f : 0.0f;
            vi.dense[34] = (last_s == S_TEH_MARBUTA) ? 1.0f : 0.0f;
            vi.dense[35] = (last_s == S_ALEF || last_s == S_ALEF_MAKSURA || last_s == S_YEH || last_s == S_WAW) ? 1.0f : 0.0f;

            uint8_t pt_id = t.si.particle;
            vi.dense[36] = (pt_id == PART_GEN) ? 1.0f : 0.0f;
            vi.dense[37] = (pt_id == PART_INNA || (t.surf_flags & SF_INNA_PRON)) ? 1.0f : 0.0f;
            vi.dense[38] = (pt_id == PART_KANA) ? 1.0f : 0.0f;
            vi.dense[39] = (pt_id == PART_NASB || pt_id == PART_AN || pt_id == PART_LI_AN || pt_id == PART_HATTA) ? 1.0f : 0.0f;
            vi.dense[40] = (pt_id == PART_JAZM || pt_id == PART_IN || pt_id == PART_LAMA) ? 1.0f : 0.0f;
            vi.dense[41] = (pt_id == PART_CONJ) ? 1.0f : 0.0f;
            vi.dense[42] = (pt_id == PART_REL || pt_id == PART_DEM) ? 1.0f : 0.0f;

            ValencyInfo val = is_v7_ ? get_valency(t.word_id) : ValencyInfo{};
            vi.dense[43] = static_cast<float>(val.v_trans) / 2.0f;
            vi.dense[44] = static_cast<float>(val.verb_prob) / 15.0f;
            vi.dense[45] = static_cast<float>(val.tan_ratio) / 15.0f;
            vi.dense[46] = val.is_gram_fem ? 1.0f : 0.0f;
            vi.dense[47] = static_cast<float>(std::min<size_t>(n, 12)) / 12.0f;
        }
        if (v9_dump_out != nullptr) {
            if (gold_diacs && gold_diacs->size() >= W) {
                for (size_t i = 0; i < W; ++i) {
                    const Token& t = ctx.toks[i];
                    if (t.cand_count == 0) continue;
                    const Cand& c0 = ctx.cands[t.cand_begin];
                    uint16_t cmask = 0;
                    for (uint32_t k = 0; k < t.cand_count; ++k) {
                        uint8_t ce = ctx.cands[t.cand_begin + k].diac[ctx.cands[t.cand_begin + k].case_pos];
                        if (ce < NUM_DIACRITICS) cmask |= static_cast<uint16_t>(1u << ce);
                    }
                    uint8_t ge = (c0.case_pos < MAX_WORD_LEN) ? (*gold_diacs)[i][c0.case_pos] : 0;
                    uint8_t gp = 0;
                    const uint8_t* ts = &ctx.syms[t.off];
                    if ((c0.flags & CF_SUFFIX) && t.n >= 2 && (ts[t.n - 1] == S_KAF || ts[t.n - 1] == S_HEH)) {
                        uint8_t pd = (*gold_diacs)[i][t.n - 1];
                        if (pd == DIAC_FATHA) gp = 1;
                        else if (pd == DIAC_DAMMA) gp = 2;
                        else if (pd == DIAC_KASRA) gp = 3;
                    }
                    V9DumpToken dt;
                    dt.inp = sc_buf.v9_in[i];
                    dt.cand_mask = cmask;
                    dt.gold_end = (ge < NUM_DIACRITICS) ? ge : 0;
                    dt.gold_pron = gp;
                    v9_dump_out->push_back(dt);
                }
            }
            return std::string();
        }
        neural_irab_.forward(sc_buf.v9_in.data(), W, sc_buf.v9_ws);
    }

    const bool track_p2_sigs = (dump_out != nullptr) || (is_v7_ && irab2_trees_.valid() && v7_weights_[V7W_IRAB2] != 0);
    const bool lazy_irab1 = track_p2_sigs && (dump_out == nullptr) && irab_trees_.valid() &&
                            (weights_[W_IRAB_TREE] != 0) && irab_trees_.has_fast_v5();
    if (track_p2_sigs) {
        sc_buf.p1_irab1_scores.assign(W * NUM_DIACRITICS, 0);
        sc_buf.p1_irab1_best.assign(W, -1);
        sc_buf.p1_bg_best.assign(W, -1);
        sc_buf.p1_bg_val.assign(W, 0);
        sc_buf.p1_rbg_best.assign(W, -1);
        sc_buf.p1_rbg_val.assign(W, 0);
        sc_buf.p1_ng_best.assign(W, -1);
        sc_buf.p1_skip_best.assign(W, -1);
        if (lazy_irab1) {
            sc_buf.p1_irab1_mask.assign(W, 0);
            sc_buf.p1_irab1_best_sc.assign(W, NEG_INF);
            sc_buf.p1_irab1_feats.resize(W);
        }
    }

    if (irab_trees_.valid() && weights_[W_IRAB_TREE] != 0) {
        auto& cg_arr = sc_buf.cg_arr;
        auto& dg_arr = sc_buf.dg_arr;
        auto& pic_arr = sc_buf.pic_arr;
        auto& ric_arr = sc_buf.ric_arr;
        const bool use_56 = (irab_feat_count_ >= IRAB_NUM_FEATURES);
        if (use_56) {
            cg_arr.assign(W, 0);
            dg_arr.assign(W, -1);
            pic_arr.assign(W, 0);
            ric_arr.assign(W, 0);
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
        const uint32_t irab_nc = irab_trees_.num_classes();
        const int32_t w_irab = weights_[W_IRAB_TREE];
        const bool fast_i1 = irab_trees_.has_fast_v5();
        if (fast_i1) {
            sc_buf.p1_irab1_feats.resize(W);
            sc_buf.i1_scores_flat.resize(W * NUM_DIACRITICS);
            sc_buf.i1_tok_idx.clear();
            sc_buf.i1_ce_mask.clear();
            for (uint32_t c = 0; c < irab_nc; ++c) sc_buf.i1_cls_words[c].clear();
        }
        for (size_t i = 0; i < W; ++i) {
            const Token& t = ctx.toks[i];
            if (t.cand_count < 2) continue;
            const uint8_t first_end = ctx.cands[t.cand_begin].diac[ctx.cands[t.cand_begin].case_pos];
            uint32_t ce_mask = (first_end < irab_nc) ? (1u << first_end) : 0u;
            bool multi_end = false;
            for (uint32_t k = 1; k < t.cand_count; ++k) {
                const Cand& ck = ctx.cands[t.cand_begin + k];
                const uint8_t ce = ck.diac[ck.case_pos];
                if (ce != first_end) multi_end = true;
                if (ce < irab_nc) ce_mask |= (1u << ce);
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
            if (fast_i1) {
                FlatTreeEnsemble::pack_features_i8_128(f, irab_feat_count_, sc_buf.p1_irab1_feats[i].data());
                sc_buf.i1_tok_idx.push_back(static_cast<uint32_t>(i));
                sc_buf.i1_ce_mask.push_back(ce_mask);
                uint32_t eval_mask = (track_p2_sigs && !lazy_irab1)
                    ? ((1u << irab_nc) - 1u)
                    : (ce_mask & ((1u << irab_nc) - 1u));
                uint32_t m = eval_mask;
                while (m) {
                    const uint32_t c = portable_ctz32(m);
                    m &= (m - 1u);
                    sc_buf.i1_cls_words[c].push_back(static_cast<uint32_t>(i));
                }
            } else {
                int32_t sc[NUM_DIACRITICS];
                for (auto& v : sc) v = 0;
                if (track_p2_sigs) {
                    irab_trees_.predict_scores(f, irab_feat_count_, sc);
                    int32_t b_sc = NEG_INF;
                    int8_t b_id = -1;
                    for (uint32_t c_id = 0; c_id < irab_nc; ++c_id) {
                        sc_buf.p1_irab1_scores[i * NUM_DIACRITICS + c_id] =
                            static_cast<int32_t>((static_cast<int64_t>(sc[c_id]) * w_irab) / 1000);
                        if (sc[c_id] > b_sc) { b_sc = sc[c_id]; b_id = static_cast<int8_t>(c_id); }
                    }
                    sc_buf.p1_irab1_best[i] = b_id;
                } else {
                    irab_trees_.predict_scores_masked(f, irab_feat_count_, ce_mask, sc);
                }
                for (uint32_t k = 0; k < t.cand_count; ++k) {
                    Cand& c = ctx.cands[t.cand_begin + k];
                    uint8_t e = c.diac[c.case_pos];
                    if (e < irab_nc)
                        c.emis += static_cast<int32_t>((static_cast<int64_t>(sc[e]) * w_irab) / 1000);
                }
            }
        }
        if (fast_i1 && !sc_buf.i1_tok_idx.empty()) {
            for (uint32_t c = 0; c < irab_nc; ++c) {
                if (!sc_buf.i1_cls_words[c].empty()) {
                    irab_trees_.eval_class_batch_i8(
                        c, sc_buf.p1_irab1_feats.data(),
                        sc_buf.i1_cls_words[c].data(), sc_buf.i1_cls_words[c].size(),
                        sc_buf.i1_scores_flat.data(), NUM_DIACRITICS);
                }
            }
            const size_t n_i1 = sc_buf.i1_tok_idx.size();
            for (size_t j = 0; j < n_i1; ++j) {
                const size_t i = sc_buf.i1_tok_idx[j];
                const uint32_t ce_mask = sc_buf.i1_ce_mask[j];
                const int32_t* sc = &sc_buf.i1_scores_flat[i * NUM_DIACRITICS];
                const Token& t = ctx.toks[i];
                if (lazy_irab1) {
                    sc_buf.p1_irab1_mask[i] = ce_mask;
                    int32_t b_sc = NEG_INF;
                    int8_t b_id = -1;
                    uint32_t m = ce_mask & ((1u << irab_nc) - 1u);
                    while (m) {
                        const uint32_t c_id = portable_ctz32(m);
                        m &= (m - 1u);
                        sc_buf.p1_irab1_scores[i * NUM_DIACRITICS + c_id] =
                            static_cast<int32_t>((static_cast<int64_t>(sc[c_id]) * w_irab) / 1000);
                        if (sc[c_id] > b_sc) { b_sc = sc[c_id]; b_id = static_cast<int8_t>(c_id); }
                    }
                    sc_buf.p1_irab1_best_sc[i] = b_sc;
                    sc_buf.p1_irab1_best[i] = b_id;
                } else if (track_p2_sigs) {
                    int32_t b_sc = NEG_INF;
                    int8_t b_id = -1;
                    for (uint32_t c_id = 0; c_id < irab_nc; ++c_id) {
                        sc_buf.p1_irab1_scores[i * NUM_DIACRITICS + c_id] =
                            static_cast<int32_t>((static_cast<int64_t>(sc[c_id]) * w_irab) / 1000);
                        if (sc[c_id] > b_sc) { b_sc = sc[c_id]; b_id = static_cast<int8_t>(c_id); }
                    }
                    sc_buf.p1_irab1_best[i] = b_id;
                }
                for (uint32_t k = 0; k < t.cand_count; ++k) {
                    Cand& c = ctx.cands[t.cand_begin + k];
                    uint8_t e = c.diac[c.case_pos];
                    if (e < irab_nc)
                        c.emis += static_cast<int32_t>((static_cast<int64_t>(sc[e]) * w_irab) / 1000);
                }
            }
        }
    }

    if (use_v9_neural) {
        const float* end_lp  = sc_buf.v9_ws.end_logp.data();
        const float* fam_lp  = sc_buf.v9_ws.fam_logp.data();
        const float* tan_lp  = sc_buf.v9_ws.tan_logp.data();
        const float* pron_lp = sc_buf.v9_ws.pron_logp.data();
        const int32_t w_end  = v9_weights_[V9W_NEURAL_END];
        const int32_t w_fam  = v9_weights_[V9W_NEURAL_FAM];
        const int32_t w_tan  = v9_weights_[V9W_NEURAL_TAN];
        const int32_t w_pron = v9_weights_[V9W_NEURAL_PRON];
        const int32_t exp_refund = (weights_[W_EXPAND_PEN] * v9_weights_[V9W_EXP_REFUND]) / 1000;
        const int32_t eos_damp = v9_weights_[V9W_LEX_END_DAMP];
        for (size_t i = 0; i < W; ++i) {
            Token& t = ctx.toks[i];
            if (t.cand_count == 0) continue;
            const uint8_t* ts = &ctx.syms[t.off];
            const bool is_eos = (i + 1 == W) || (ctx.toks[i + 1].boundary_before && (ctx.toks[i + 1].bkind_mask & BK_HARD));
            int32_t max_raw = NEG_INF;
            if (is_eos && t.cand_count >= 2 && eos_damp < 1000) {
                for (uint32_t k = 0; k < t.cand_count; ++k) {
                    int32_t em = ctx.cands[t.cand_begin + k].emis + ((ctx.cands[t.cand_begin + k].flags & CF_EXPANDED) ? exp_refund : 0);
                    if (em > max_raw) max_raw = em;
                }
            }
            const float* el = end_lp  + i * NUM_DIACRITICS;
            const float* fl = fam_lp  + i * 5;
            const float* tl = tan_lp  + i * 2;
            const float* pl = pron_lp + i * 4;
            for (uint32_t k = 0; k < t.cand_count; ++k) {
                Cand& ck = ctx.cands[t.cand_begin + k];
                if (ck.flags & CF_EXPANDED) ck.emis += exp_refund;
                if (is_eos && t.cand_count >= 2 && eos_damp < 1000) {
                    ck.emis = max_raw + static_cast<int32_t>((static_cast<int64_t>(ck.emis - max_raw) * eos_damp) / 1000);
                }
                const uint8_t ce = ck.diac[ck.case_pos];
                const uint8_t cf = diac_family(ce);
                const size_t ti = diac_is_tanween(ce) ? 1u : 0u;
                float nscore = 0.0f;
                if (ce < NUM_DIACRITICS) nscore += el[ce] * static_cast<float>(w_end);
                if (cf < 5) nscore += fl[cf] * static_cast<float>(w_fam);
                nscore += tl[ti] * static_cast<float>(w_tan);
                if ((ck.flags & CF_SUFFIX) && t.n >= 2 && (ts[t.n - 1] == S_KAF || ts[t.n - 1] == S_HEH)) {
                    uint8_t pd = ck.diac[t.n - 1];
                    uint8_t pcls = (pd == DIAC_FATHA) ? 1 : ((pd == DIAC_DAMMA) ? 2 : ((pd == DIAC_KASRA) ? 3 : 0));
                    nscore += pl[pcls] * static_cast<float>(w_pron);
                }
                ck.emis += static_cast<int32_t>(std::lrintf(nscore));
            }
        }
    }

    const uint32_t beam_k_lim = use_v9_neural ? 10u : ((is_v8_ || core2_dump_out != nullptr) ? 14u : 10u);
    for (size_t i = 0; i < W; ++i) {
        Token& t = ctx.toks[i];
        if (t.cand_count > beam_k_lim) {
            auto b = ctx.cands.begin() + t.cand_begin;
            if (is_v8_ || core2_dump_out != nullptr) {
                std::sort(b, b + t.cand_count, [](const Cand& a, const Cand& c) { return a.emis > c.emis; });
                const uint8_t* ts = &ctx.syms[t.off];
                const uint8_t lam_skip = (t.si.al_len >= 2) ? t.si.al_lam_pos : 255;
                uint16_t hk_arr[64];
                const uint32_t n_hk = std::min<uint32_t>(t.cand_count, 64u);
                for (uint32_t k = 0; k < n_hk; ++k) {
                    hk_arr[k] = compute_core_hash(ts, b[k].diac, b[k].case_pos, lam_skip);
                }
                auto get_hk = [&](uint32_t k) -> uint16_t {
                    return (k < 64u) ? hk_arr[k] : compute_core_hash(ts, b[k].diac, b[k].case_pos, lam_skip);
                };
                uint16_t seen_h[8];
                uint32_t n_seen = 0;
                for (uint32_t k = 0; k < beam_k_lim && n_seen < 8; ++k) {
                    uint16_t hk = get_hk(k);
                    bool dup = false;
                    for (uint32_t j = 0; j < n_seen; ++j) if (seen_h[j] == hk) { dup = true; break; }
                    if (!dup) seen_h[n_seen++] = hk;
                }
                uint32_t rep_slot = beam_k_lim;
                for (uint32_t k = beam_k_lim; k < t.cand_count && n_seen < 8 && rep_slot > 4; ++k) {
                    uint16_t hk = get_hk(k);
                    bool dup = false;
                    for (uint32_t j = 0; j < n_seen; ++j) if (seen_h[j] == hk) { dup = true; break; }
                    if (!dup) {
                        seen_h[n_seen++] = hk;
                        for (int r = static_cast<int>(rep_slot) - 1; r >= 4; --r) {
                            uint16_t hr = get_hk(static_cast<uint32_t>(r));
                            bool has_earlier = false;
                            for (int q = 0; q < r; ++q) {
                                if (get_hk(static_cast<uint32_t>(q)) == hr) { has_earlier = true; break; }
                            }
                            if (has_earlier) {
                                std::swap(b[r], b[k]);
                                if (static_cast<uint32_t>(r) < 64u && k < 64u) std::swap(hk_arr[r], hk_arr[k]);
                                rep_slot = static_cast<uint32_t>(r);
                                break;
                            }
                        }
                    }
                }
            } else {
                std::partial_sort(b, b + beam_k_lim, b + t.cand_count, [](const Cand& a, const Cand& c) { return a.emis > c.emis; });
            }
            t.cand_count = beam_k_lim;
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

    // Precompute per-token ValencyInfo once (replaces ~70x redundant bitmap rank1 lookups/token)
    auto& tok_val = sc_buf.tok_val;
    if (is_v7_ && valency_.valid()) {
        tok_val.resize(W);
        for (size_t i = 0; i < W; ++i) tok_val[i] = get_valency(ctx.toks[i].word_id);
    } else {
        tok_val.assign(W, ValencyInfo{});
    }

    // Precompute per-candidate CandMeta (8 bytes/cand) + pair_off/pair_bin layout
    auto& cmeta = sc_buf.cmeta;
    cmeta.resize(dp_off[W]);
    const bool need_st3 = is_v7_ && trans3_ && trans3_size_ == TRANS3_TABLE_SIZE;
    auto& pair_off = sc_buf.pair_off;
    pair_off.resize(W + 1);
    pair_off[0] = 0;
    for (size_t i = 0; i < W; ++i) {
        const Token& t = ctx.toks[i];
        const uint8_t* ts = &ctx.syms[t.off];
        const uint8_t lam_skip = (t.si.al_len >= 2) ? t.si.al_lam_pos : 255;
        const uint32_t dbase = dp_off[i];
        const ValencyInfo& vi = tok_val[i];
        for (uint32_t k = 0; k < t.cand_count; ++k) {
            const Cand& cc = ctx.cands[t.cand_begin + k];
            CandMeta& cm = cmeta[dbase + k];
            const uint8_t ce = cc.diac[cc.case_pos];
            cm.ce = ce;
            cm.cf = diac_family(ce);
            bool ambig_obl = false;
            cm.eff_fam = eff_case_fam(ts, cc, &ambig_obl);
            cm.role = has_v5_24roles_ ? cand_role_v5(t, cc) : t.role;
            cm.st3 = need_st3 ? cand_state3_v7_val(t, cc, ts, vi, cm.eff_fam) : 0;
            cm.core_h = compute_core_hash(ts, cc.diac, cc.case_pos, lam_skip);
            const bool invar_lv = is_invar_long_vowel_end(ts, t.n, cc.case_pos, cc);
            const bool invar_syn = invar_lv || (!(cc.flags & CF_SUFFIX) && cc.case_pos + 1 < t.n && ts[cc.case_pos + 1] == S_ALEF);
            cm.bits = static_cast<uint8_t>(
                (ambig_obl ? static_cast<uint8_t>(CMB_AMBIG_OBL) : static_cast<uint8_t>(0)) |
                (diac_is_tanween(ce) ? static_cast<uint8_t>(CMB_TAN) : static_cast<uint8_t>(0)) |
                (invar_lv ? static_cast<uint8_t>(CMB_INVAR_LV) : static_cast<uint8_t>(0)) |
                (invar_syn ? static_cast<uint8_t>(CMB_INVAR_SYN) : static_cast<uint8_t>(0)) |
                (has_verb_obj_suffix(ts, t.n, cc) ? static_cast<uint8_t>(CMB_HAS_VOBJ) : static_cast<uint8_t>(0)) |
                (is_ali_proper_cand(t, cc) ? static_cast<uint8_t>(CMB_ALI_PROP) : static_cast<uint8_t>(0)) |
                (((cc.flags & CF_HAS_AL) || t.si.al_len > 0) ? static_cast<uint8_t>(CMB_HAS_AL) : static_cast<uint8_t>(0)) |
                (((cc.flags & CF_GEN) || t.si.has_prep) ? static_cast<uint8_t>(CMB_HAS_GEN) : static_cast<uint8_t>(0)));
        }
        const uint32_t p_sz = (i > 0 && !t.boundary_before) ? (ctx.toks[i - 1].cand_count * t.cand_count) : 0u;
        pair_off[i + 1] = pair_off[i] + p_sz;
    }
    auto& pair_bin = sc_buf.pair_bin;
    pair_bin.resize(pair_off[W]);

    const int32_t w_bg = weights_[W_BIGRAM];
    const bool use_v5_bg = bigrams_.is_v5();
    const bool use_v3_bg = bigrams_.is_v3();
    const bool use_compact_ng = ngrams_.is_compact();

    auto calc_ng_entry = [](bool hit, const NgramLookupResult& e, uint8_t ce, int32_t w_scale) -> int32_t {
        if (!hit || w_scale == 0) return 0;
        int32_t raw = 0;
        if (ce == e.best_ending_id) raw = e.best_bonus;
        else if (ce == e.alt_ending_id) raw = e.alt_bonus;
        if (raw == 0) return 0;
        return static_cast<int32_t>((static_cast<int64_t>(raw) * w_scale) / 1000);
    };

    int32_t v9_crf_sc[16][16] = {};
    if (use_v9_neural && neural_irab_.crf_trans()) {
        const float* ct_mat = neural_irab_.crf_trans();
        const int32_t w_crf = v9_weights_[V9W_NEURAL_CRF];
        for (int pe = 0; pe < 15; ++pe) {
            for (int ce = 0; ce < 15; ++ce) {
                v9_crf_sc[pe][ce] = static_cast<int32_t>(std::lrintf(ct_mat[pe * 15 + ce] * static_cast<float>(w_crf)));
            }
        }
    }

    // Process Viterbi in 16-token prefetch chunks so compact ngram + bigram L3 misses overlap in parallel!
    constexpr size_t NG_CHUNK = 16u;
    NgKeyJob ng_jobs[NG_CHUNK];
    for (size_t chunk_s = 0; chunk_s < W; chunk_s += NG_CHUNK) {
        const size_t chunk_e = std::min<size_t>(W, chunk_s + NG_CHUNK);
        if (use_compact_ng) {
            // Phase 1: compute keys & prefetch hash buckets + bigram rows in parallel
            for (size_t i = chunk_s; i < chunk_e; ++i) {
                NgKeyJob& jb = ng_jobs[i - chunk_s];
                jb.mask = 0;
                const Token& ct = ctx.toks[i];
                if (ct.word_id == UINT32_MAX) continue;
                const bool fresh = (i == 0) || ct.boundary_before;
                const Token* pt = fresh ? nullptr : &ctx.toks[i - 1];
                const Token* p2 = (pt && i >= 2 && !pt->boundary_before) ? &ctx.toks[i - 2] : nullptr;
                const Token* p3 = (p2 && i >= 3 && !p2->boundary_before) ? &ctx.toks[i - 3] : nullptr;
                const Token* nx = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? &ctx.toks[i + 1] : nullptr;
                if (pt && pt->word_id != UINT32_MAX) {
                    if (use_v5_bg) {
                        jb.bg_prev = pt->word_id;
                        jb.mask |= 64u;
                        bigrams_.prefetch_row(pt->word_id);
                    }
                    if (p2 && p2->word_id != UINT32_MAX) {
                        const Token* p4 = (p3 && i >= 4 && !p3->boundary_before) ? &ctx.toks[i - 4] : nullptr;
                        if (p4 && p3 && p4->word_id != UINT32_MAX && p3->word_id != UINT32_MAX) {
                            uint32_t w5[5] = {p4->word_id, p3->word_id, p2->word_id, pt->word_id, ct.word_id};
                            jb.k5 = NgramTableV6::fnv1a_key_tagged(w5, 5, 5);
                            jb.mask |= 1u;
                            ngrams_.prefetch_bucket(jb.k5);
                        }
                        if (p3 && p3->word_id != UINT32_MAX) {
                            uint32_t w4[4] = {p3->word_id, p2->word_id, pt->word_id, ct.word_id};
                            jb.k4 = NgramTableV6::fnv1a_key_tagged(w4, 4, 4);
                            jb.mask |= 2u;
                            ngrams_.prefetch_bucket(jb.k4);
                        }
                        uint32_t w3[3] = {p2->word_id, pt->word_id, ct.word_id};
                        jb.k3 = NgramTableV6::fnv1a_key_tagged(w3, 3, 3);
                        jb.mask |= 4u;
                        ngrams_.prefetch_bucket(jb.k3);
                    }
                }
                if (is_v7_) {
                    if (nx && nx->word_id != UINT32_MAX) {
                        uint32_t wr[2] = {ct.word_id, nx->word_id};
                        jb.kr = NgramTableV6::fnv1a_key_tagged(wr, 2, NG_ORDER_RBG);
                        jb.mask |= 8u;
                        ngrams_.prefetch_bucket(jb.kr);
                        if (pt && pt->word_id != UINT32_MAX) {
                            uint32_t wc3[3] = {pt->word_id, ct.word_id, nx->word_id};
                            jb.kc = NgramTableV6::fnv1a_key_tagged(wc3, 3, NG_ORDER_CTR3);
                            jb.mask |= 16u;
                            ngrams_.prefetch_bucket(jb.kc);
                        }
                    }
                    if (p2 && p2->word_id != UINT32_MAX) {
                        uint32_t ws1[2] = {p2->word_id, ct.word_id};
                        jb.ks = NgramTableV6::fnv1a_key_tagged(ws1, 2, NG_ORDER_SKIP1);
                        jb.mask |= 32u;
                        ngrams_.prefetch_bucket(jb.ks);
                    }
                }
            }
            // Phase 2: prefetch entry ranges + bigram entry midpoints
            for (size_t i = chunk_s; i < chunk_e; ++i) {
                const NgKeyJob& jb = ng_jobs[i - chunk_s];
                const uint8_t jm = jb.mask;
                if (jm == 0) continue;
                if (jm & 64u) bigrams_.prefetch_row_mid(jb.bg_prev);
                if (jm & 1u) ngrams_.prefetch_entry_range(jb.k5);
                if (jm & 2u) ngrams_.prefetch_entry_range(jb.k4);
                if (jm & 4u) ngrams_.prefetch_entry_range(jb.k3);
                if (jm & 8u) ngrams_.prefetch_entry_range(jb.kr);
                if (jm & 16u) ngrams_.prefetch_entry_range(jb.kc);
                if (jm & 32u) ngrams_.prefetch_entry_range(jb.ks);
            }
        }

        // Phase 3: probe prefetched tables + fill pair_bin + Viterbi DP transition
        for (size_t i = chunk_s; i < chunk_e; ++i) {
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
            if (use_compact_ng) {
                const NgKeyJob& jb = ng_jobs[i - chunk_s];
                const uint8_t jm = jb.mask;
                if (jm & 64u) {
                    bg_v5 = bigrams_.find_v5(jb.bg_prev, ct.word_id);
                    if (bg_v5 >= 0) bg_mask = meta_bg_mask(ct.word_id);
                } else if (pt && pt->word_id != UINT32_MAX && ct.word_id != UINT32_MAX) {
                    if (use_v3_bg) {
                        bg_v3 = bigrams_.find_v3(pt->word_id, ct.word_id);
                        if (bg_v3) bg_mask = meta_bg_mask(ct.word_id);
                    } else if (!use_v5_bg) {
                        bg_v2 = bigrams_.find(pt->word_id, ct.word_id);
                    }
                }
                if (jm & 1u) has_ng_hit = ngrams_.find_val_compact_key(jb.k5, 5, ng_hit_v);
                if (!has_ng_hit && (jm & 2u)) has_ng_hit = ngrams_.find_val_compact_key(jb.k4, 4, ng_hit_v);
                if (!has_ng_hit && (jm & 4u)) has_ng_hit = ngrams_.find_val_compact_key(jb.k3, 3, ng_hit_v);
                if (jm & 8u) has_ng_rbg = ngrams_.find_val_compact_key(jb.kr, NG_ORDER_RBG, ng_rbg_v);
                if (jm & 16u) has_ng_ctr3 = ngrams_.find_val_compact_key(jb.kc, NG_ORDER_CTR3, ng_ctr3_v);
                if (jm & 32u) has_ng_skip1 = ngrams_.find_val_compact_key(jb.ks, NG_ORDER_SKIP1, ng_skip1_v);
            } else {
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
            }
            if (track_p2_sigs) {
                if (bg_v5 >= 0) {
                    int8_t be = -1;
                    int16_t bv = 0;
                    bigrams_.best_bonus_v5(bg_v5, bg_mask, be, bv);
                    if (bv > 0) { sc_buf.p1_bg_best[i] = be; sc_buf.p1_bg_val[i] = bv; }
                } else if (bg_v3 || bg_v2) {
                    int16_t bv = -32000;
                    int8_t be = -1;
                    for (uint8_t d = 0; d < NUM_DIACRITICS; ++d) {
                        int16_t v = bg_v3 ? bigrams_.bonus_v3(bg_v3, bg_mask, d) : bg_v2->bonus[d];
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

            if (fresh) {
                const uint32_t dbase = dp_off[i];
                for (uint32_t c = 0; c < ct.cand_count; ++c) {
                    const Cand& cc = ctx.cands[ct.cand_begin + c];
                    const CandMeta& cm = cmeta[dbase + c];
                    const int32_t t_sc = trans_score_scaled(0, DIAC_NONE, cm.role, cm.ce);
                    const int32_t bgb_f = is_v7_ ? calc_ng_entry(has_ng_rbg, ng_rbg_v, cm.ce, v7_weights_[V7W_RBG]) : 0;
                    dp[dbase + c] = prev_best + cc.emis + t_sc + bgb_f;
                    dp_bg[dbase + c] = bgb_f;
                    back[dbase + c] = prev_best_idx;
                }
                continue;
            }

            const uint32_t pcnt = pt->cand_count;
            const uint32_t ccnt = ct.cand_count;
            const uint32_t prev_dp_base = dp_off[i - 1];
            const uint32_t cur_dp_base = dp_off[i];
            int32_t* pbin = &pair_bin[pair_off[i]];
            fill_pair_bin(ctx, cmeta.data(), prev_dp_base, cur_dp_base, p3, p2, *pt, ct, nx, pbin);

            int32_t* __restrict dp_cur = &dp[cur_dp_base];
            int* __restrict back_cur = &back[cur_dp_base];
            uint16_t col_idx_stack[64];
            bool fast_p_outer = (ccnt <= 64u && !trans_scaled_16_.empty());
            for (uint32_t c = 0; c < ccnt; ++c) {
                const CandMeta& cm = cmeta[cur_dp_base + c];
                const uint8_t ce = cm.ce;
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
                dp_bg[cur_dp_base + c] = bgb;
                dp_cur[c] = NEG_INF;
                back_cur[c] = 0;
                if (fast_p_outer) {
                    const uint16_t ci = trans_col_index(cm.role, ce);
                    col_idx_stack[c] = ci;
                    if (ci == UINT16_MAX) fast_p_outer = false;
                }
            }
            if (fast_p_outer) {
                for (uint32_t p = 0; p < pcnt; ++p) {
                    const CandMeta& pm = cmeta[prev_dp_base + p];
                    const int32_t dp_p = dp[prev_dp_base + p];
                    const int32_t* __restrict pbin_row = pbin + static_cast<size_t>(p) * ccnt;
                    const int16_t* __restrict tr_row = trans_scaled16_row(pm.role, pm.ce);
                    const int32_t* __restrict crf_row = use_v9_neural ? v9_crf_sc[pm.ce] : nullptr;
                    if (tr_row) {
                        if (crf_row) {
                            for (uint32_t c = 0; c < ccnt; ++c) {
                                const int32_t v = dp_p + tr_row[col_idx_stack[c]] + crf_row[cmeta[cur_dp_base + c].ce] + pbin_row[c];
                                if (v > dp_cur[c]) {
                                    dp_cur[c] = v;
                                    back_cur[c] = static_cast<int>(p);
                                }
                            }
                        } else {
                            for (uint32_t c = 0; c < ccnt; ++c) {
                                const int32_t v = dp_p + tr_row[col_idx_stack[c]] + pbin_row[c];
                                if (v > dp_cur[c]) {
                                    dp_cur[c] = v;
                                    back_cur[c] = static_cast<int>(p);
                                }
                            }
                        }
                    } else {
                        for (uint32_t c = 0; c < ccnt; ++c) {
                            const int32_t v = dp_p + pbin_row[c];
                            if (v > dp_cur[c]) {
                                dp_cur[c] = v;
                                back_cur[c] = static_cast<int>(p);
                            }
                        }
                    }
                }
                for (uint32_t c = 0; c < ccnt; ++c) {
                    dp_cur[c] += ctx.cands[ct.cand_begin + c].emis + dp_bg[cur_dp_base + c];
                }
            } else {
                for (uint32_t c = 0; c < ccnt; ++c) {
                    const CandMeta& cm = cmeta[cur_dp_base + c];
                    const uint8_t ce = cm.ce;
                    const uint8_t cr = cm.role;
                    int32_t best = NEG_INF;
                    int best_p = 0;
                    for (uint32_t p = 0; p < pcnt; ++p) {
                        const CandMeta& pm = cmeta[prev_dp_base + p];
                        const int32_t t_sc = trans_score_scaled(pm.role, pm.ce, cr, ce) + (use_v9_neural ? v9_crf_sc[pm.ce][ce] : 0);
                        const int32_t v = dp[prev_dp_base + p] + t_sc + pbin[p * ccnt + c];
                        if (v > best) {
                            best = v;
                            best_p = static_cast<int>(p);
                        }
                    }
                    dp_cur[c] = best + ctx.cands[ct.cand_begin + c].emis + dp_bg[cur_dp_base + c];
                    back_cur[c] = best_p;
                }
            }
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

        struct SynTokCtx {
            const uint8_t* ts = nullptr;
            ValencyInfo val_cur{};
            ValencyInfo val_pt{};
            int32_t val_w = 0;
            bool use_val = false;
            bool is_kana_v = false;
            bool is_conj_tok = false;
            bool starts_imperf = false;
            bool is_imperf_wn = false;
            bool an_in_tok = false;
            bool an_in_nx_verb = false;
            bool an_in_nx_noun = false;
            bool man_nx_trig = false;
            bool pt_num_gen = false;
            bool pt_is_ay = false;
            bool time_adv = false;
            bool lex_adv = false;
            bool subj_cue = false;
            bool after_innama = false;
            bool is_masdar_adv = false;
            bool is_innama = false;
            bool is_title = false;
            bool is_dem_before_al = false;
            bool can_have_mudaf_ilayh = false;
            bool nx_noun_hatta = false;
            bool is_comp_11_19 = false;
            bool idafa_has_arg = false;
            bool nx_mudaf_base = false;
            bool end_khabar_only_base = false;
            bool afal_pass_blk = false;
            bool is_afal_maksura_nx = false;
            bool nx_has_al = false;
            bool comma_waw_conj = false;
            bool pt_is_prep_pron = false;
            bool pt_no_digit = false;
            bool non_idafa_mabni = false;
            bool decl_surf_blk = false;
        };

        struct SynOpt {
            const Cand* cc = nullptr;
            uint32_t ok = 0;
            int32_t base_score = 0;
            int32_t static_syn = 0;
            uint8_t ce = 0;
            uint8_t cf = 0;
            uint8_t role = 0;
            uint8_t st3 = 0;
            bool tan = false;
            bool c_al = false;
            bool c_gen = false;
            bool c_part_non_noun = false;
            bool is_invar = false;
            bool is_idafa_decl = false;
            bool is_decl_noun = false;
            bool is_bare_noun = false;
            bool has_attached_conj = false;
            bool is_verb_cand = false;
            bool is_fem_at = false;
            bool is_dip = false;
            bool is_dual_pl = false;
            bool is_nom_ok = false;
            bool is_acc_ok = false;
            bool oov_sukun = false;
            bool foreign_sukun_noun = false;
            bool proper_dip = false;
            bool imperf_mood_elig = false;
            bool is_part_inna_sh = false;
            bool is_part_nasb_eff = false;
            bool is_part_jazm_eff = false;
            bool has_vobj = false;
            bool can_open_idafa = false;
            bool invar_open_idafa = false;
            bool coord_al_fallback = false;
        };

        auto step_syn = [&](SynState& st, size_t idx, const Token& t, const SynTokCtx& tc, const SynOpt& op,
                            const Token* pt, const Cand* pc, const Token* nx, bool is_seg_end) -> int32_t {
            const Cand& c = *op.cc;
            int32_t syn = op.static_syn;
            const uint8_t cf = op.cf;
            const bool c_al = op.c_al;
            const bool c_gen = op.c_gen;
            const bool c_part_non_noun = op.c_part_non_noun;
            const bool is_verb_cand = op.is_verb_cand;

            // 1. PP Chunk Closure Check (Solution 2): close active PP tail when syntax breaks PP
            const bool cur_is_coord = (st.conj_pending || op.has_attached_conj) && !is_verb_cand &&
                                      (!c_part_non_noun || t.si.particle == PART_IDAFA_NOUN);
            if (st.pp_state == 2) {
                bool stay_pp = false;
                if (st.idafa_state == 1 && !c_part_non_noun && !is_verb_cand && !c_gen) {
                    stay_pp = true;
                } else if (cur_is_coord && !c_gen && (st.head_np_pp || st.prev_np_pp) &&
                           (c_al == st.head_np_al || c_al == st.prev_np_al)) {
                    stay_pp = true;
                } else if (!c_part_non_noun && !is_verb_cand && !c_gen && !st.conj_pending && !op.has_attached_conj) {
                    if (c_al && st.prev_np_al) stay_pp = true;
                    else if (!c_al && st.prev_np_tan && (t.is_adj_like || t.is_ibn || (t.surf_flags & SF_NAME_HEAD))) stay_pp = true;
                    else if (!c_al && !st.prev_np_al && (t.is_ibn || c.morph_class == MC_NOUN_SHADDA_CORE)) stay_pp = true;
                }
                if (!stay_pp) {
                    st.pp_state = 0;
                }
            }

            // 2. Score Verbs (Mood expectation after ان / لن / لم / حتى)
            if (op.imperf_mood_elig) {
                if (st.mood_expect == FAM_FATHA) {
                    if (cf == FAM_FATHA) syn += 900;
                    else if (cf == FAM_DAMMA) syn -= 900;
                } else if (st.mood_expect == FAM_SUKUN) {
                    if (cf == FAM_SUKUN || cf == FAM_FATHA) syn += 700;
                    else if (cf == FAM_DAMMA) syn -= 1200;
                }
            }

            // 3. Score Declinable Nouns with Chunking, ClauseStack & Hard Negative Invariants
            const bool is_pp_after_prep = (st.pp_state == 1);
            const bool is_adj_agree = !st.conj_pending && !op.has_attached_conj && !is_pp_after_prep &&
                                      st.prev_np_fam != FAM_NONE && st.prev_np_fam != FAM_SUKUN &&
                                      ((c_al && st.prev_np_al) ||
                                       (!c_al && !(c.flags & CF_SUFFIX) && st.prev_np_tan && t.is_adj_like));
            const bool is_coord_agree = (st.conj_pending || op.has_attached_conj) &&
                                        st.head_np_fam != FAM_NONE && st.head_np_fam != FAM_SUKUN &&
                                        (c_al == st.head_np_al || op.coord_al_fallback);

            const bool is_elative_dip = (c.verb_pattern == 2 && op.is_dip &&
                                         (st.pp_state > 0 || st.idafa_state == 1 || c_gen));
            if (op.is_decl_noun || is_elative_dip) {
                const bool gen_licensed = c_gen || (st.pp_state > 0) || (st.idafa_state == 1) ||
                                          (st.prev_np_fam == FAM_KASRA) ||
                                          (is_coord_agree && st.head_np_fam == FAM_KASRA) ||
                                          (t.is_ibn && st.prev_np_fam == FAM_KASRA) ||
                                          tc.pt_num_gen ||
                                          (st.last_was_title && st.prev_np_fam == FAM_KASRA) ||
                                          (tc.pt_is_ay && pc && diac_has_shadda(pc->diac[pt->n - 1]));

                // Hard Negative Invariant (Solution 3): Block unlicensed dangling KASRA (especially at EOS!)
                if (!op.oov_sukun && !gen_licensed && !op.is_fem_at && !op.is_dual_pl && !(t.surf_flags & SF_AMS) && cf == FAM_KASRA) {
                    syn -= (is_seg_end ? 1900 : 1550);
                }

                // Direct preposition / adverbial idafa / proclitic prep government
                if (c_gen || is_pp_after_prep) {
                    if (op.oov_sukun || op.foreign_sukun_noun) {
                        // Preserve lexical/OOV sukun on foreign loanwords inside PP
                    } else if (op.is_dip && !c_al && !op.is_dual_pl) {
                        if (!op.proper_dip && tc.nx_mudaf_base) {
                            if (cf == FAM_KASRA) syn += 1200;
                            else syn -= 1000;
                        } else {
                            if (cf == FAM_FATHA && !op.tan) syn += 1250;
                            else syn -= 1100;
                        }
                    } else if (!op.is_dual_pl) {
                        if (cf == FAM_KASRA) syn += 1300;
                        else syn -= 1800;
                    }
                } else if (st.idafa_state == 1) {
                    if (op.oov_sukun) {
                        // Preserve sukun on foreign proper compounds
                    } else if (op.is_dip && !c_al && !op.is_dual_pl) {
                        if (op.proper_dip || !nx || nx->si.particle != PART_NONE) {
                            if (cf == FAM_FATHA && !op.tan) syn += 950;
                        } else {
                            if (cf == FAM_KASRA) syn += 950;
                        }
                    } else if (!op.is_dual_pl) {
                        if (cf == FAM_KASRA) syn += 950;
                        else if (cf == FAM_DAMMA) syn -= 650;
                    }
                } else if (is_coord_agree && st.pp_state == 0) {
                    if (cf == st.head_np_fam) syn += 850;
                } else if (is_adj_agree) {
                    if (cf == st.prev_np_fam) syn += 750;
                } else if (!is_pp_after_prep && st.pp_state == 2 && cf == FAM_KASRA && !op.oov_sukun) {
                    syn += 650;
                } else if (op.is_bare_noun && st.pp_state == 0) {
                    const uint8_t fk = st.kind[st.depth];
                    const uint8_t fs = st.stage[st.depth];
                    const bool use_val = tc.use_val;
                    const int32_t val_w = tc.val_w;

                    if (fk == FRM_VERB_ACT && fs == 0 && !tc.lex_adv) {
                        const uint8_t gv = st.gov_vtrans[st.depth];
                        const int8_t gvo = st.gov_vodds[st.depth];
                        if (op.is_nom_ok) {
                            syn += tc.subj_cue ? 1250 : 650;
                            if (use_val && gv == 1) syn += val_w;
                            else if (use_val && gv == 2 && gvo >= 3 && !tc.subj_cue) syn -= val_w / 2;
                        } else if (op.is_acc_ok) {
                            if (tc.subj_cue) syn -= 850;
                            if (use_val && gv == 1) syn -= val_w;
                            else if (use_val && gv == 2 && gvo >= 3 && !tc.subj_cue) syn += (val_w * gvo) / 7;
                        }
                    } else if ((fk == FRM_VERB_ACT && fs == 1) || fk == FRM_VERB_HAVE) {
                        const bool after_prep_pron = tc.pt_is_prep_pron && op.is_fem_at && !c_al;
                        const uint8_t gv = st.gov_vtrans[st.depth];
                        const int8_t gvo = st.gov_vodds[st.depth];
                        if (after_prep_pron) {
                            if (op.is_nom_ok) syn += 700;
                        } else if (use_val && gv == 1) {
                            if (op.is_acc_ok && !tc.lex_adv) syn -= val_w / 2;
                            else if (op.is_nom_ok && !tc.lex_adv) syn += val_w / 2;
                        } else {
                            if (op.is_acc_ok) {
                                syn += 750;
                                if (use_val && gv == 2 && gvo > 0) syn += (val_w * gvo) / 7;
                            } else if (op.is_nom_ok && !tc.lex_adv) {
                                syn -= 350;
                                if (use_val && gv == 2 && gvo >= 2) syn -= val_w / 2;
                            }
                        }
                    } else if (fk == FRM_VERB_PASS && fs == 0 && !tc.time_adv &&
                               c.verb_pattern != 2 && !(tc.afal_pass_blk && !c_al)) {
                        if (op.is_nom_ok) syn += 1100;
                        else if (op.is_acc_ok) syn -= 850;
                    } else if (fk == FRM_INNA) {
                        if (fs == 0) {
                            if (op.is_acc_ok) syn += 1250;
                            else if (op.is_nom_ok) syn -= 800;
                        } else if (fs == 1) {
                            if (op.is_nom_ok) syn += 1050;
                            else if (op.is_acc_ok && !tc.lex_adv) syn -= 450;
                        }
                    } else if (fk == FRM_KANA) {
                        if (fs == 0) {
                            if (!c_al && tc.end_khabar_only_base) {
                                if (op.is_acc_ok) syn += 950;
                                else if (op.is_nom_ok) syn += 150;
                            } else {
                                if (op.is_nom_ok) syn += 1050;
                                else if (op.is_acc_ok) syn += 100;
                            }
                        } else if (fs == 1) {
                            if (op.is_acc_ok) syn += 1100;
                            else if (op.is_nom_ok && !tc.lex_adv) syn -= 550;
                        }
                    } else if (fk == FRM_NOM && !tc.lex_adv) {
                        if (fs == 0 && (idx == 0 || tc.after_innama) && !tc.is_masdar_adv) {
                            if (op.is_nom_ok) syn += 850;
                            else if (op.is_acc_ok) syn -= 700;
                        } else if (fs == 1) {
                            if (op.is_nom_ok) syn += (!c_al ? 950 : 750);
                            else if (op.is_acc_ok && !c_al && t.is_adj_like) syn -= 500;
                        }
                    }
                    if (use_val && tc.val_cur.word_syn_type == 3 && op.is_acc_ok) {
                        syn += val_w;
                    }
                    if (tc.val_pt.next_case_pref != 0) {
                        if (tc.val_pt.next_case_pref == FAM_DAMMA && op.is_nom_ok) syn += (val_w * tc.val_pt.next_nom_p) / 24;
                        else if (tc.val_pt.next_case_pref == FAM_FATHA && op.is_acc_ok) syn += (val_w * tc.val_pt.next_acc_p) / 24;
                        else if (tc.val_pt.next_case_pref == FAM_KASRA && cf == FAM_KASRA) syn += (val_w * tc.val_pt.next_gen_p) / 24;
                    }
                }

                // Prevent spurious Construct (tanween drop) unless a real Mudaf Ilayh can follow
                if (!c_al && !(c.flags & CF_SUFFIX) && !op.is_dip && !op.is_fem_at && !op.is_dual_pl &&
                    !op.tan && !(t.surf_flags & SF_NAME_HEAD) && !t.digit_after &&
                    st.idafa_state == 0 && t.si.particle == PART_NONE && !tc.can_have_mudaf_ilayh) {
                    syn -= 650;
                }
            }

            // 4. Advance SynState cleanly
            if (!tc.is_dem_before_al) st.idafa_state = 0;
            if (!tc.is_conj_tok) st.conj_pending = 0;

            if (tc.is_kana_v) {
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
                const bool prev_nom_subj = (st.prev_np_fam == FAM_DAMMA && !st.prev_np_pp && tc.pt_no_digit) ||
                                           (st.kind[st.depth] == FRM_NOM && st.stage[st.depth] >= 1) ||
                                           (st.kind[st.depth] == FRM_INNA && st.stage[st.depth] == 1) ||
                                           (st.kind[st.depth] == FRM_KANA && st.stage[st.depth] == 1);
                const bool svo_after_nom = prev_nom_subj && tc.starts_imperf;
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
                           op.has_vobj || svo_after_nom) {
                    st.kind[st.depth] = FRM_VERB_HAVE;
                    st.stage[st.depth] = 1;
                    st.gov_vtrans[st.depth] = tc.val_cur.v_trans;
                    st.gov_vodds[st.depth] = tc.val_cur.val_odds;
                } else {
                    st.kind[st.depth] = FRM_VERB_ACT;
                    st.stage[st.depth] = 0;
                    st.gov_vtrans[st.depth] = tc.val_cur.v_trans;
                    st.gov_vodds[st.depth] = tc.val_cur.val_odds;
                }
                st.pp_state = 0;
                st.mudaf_head_fam = FAM_NONE;
                st.prev_np_fam = FAM_NONE;
                st.head_np_fam = FAM_NONE;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (op.is_part_inna_sh) {
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
                if (tc.nx_noun_hatta) {
                    st.pp_state = 1;
                    st.prev_np_fam = FAM_NONE;
                    st.mood_expect = FAM_NONE;
                } else {
                    st.mood_expect = FAM_FATHA;
                }
                st.last_was_title = 0;
            } else if (op.is_part_nasb_eff) {
                st.mood_expect = FAM_FATHA;
                st.last_was_title = 0;
            } else if (op.is_part_jazm_eff) {
                st.mood_expect = FAM_SUKUN;
                st.last_was_title = 0;
            } else if (tc.is_conj_tok) {
                st.conj_pending = 1;
                st.prev_np_fam = FAM_NONE;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (tc.is_dem_before_al) {
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (t.si.particle == PART_PRON) {
                if (st.pp_state == 0 && st.kind[st.depth] == FRM_NOM && st.stage[st.depth] == 0) {
                    st.stage[st.depth] = 1;
                }
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if (tc.is_innama) {
                st.kind[st.depth] = FRM_NOM;
                st.stage[st.depth] = 0;
                st.pp_state = 0;
                st.prev_np_fam = FAM_NONE;
                st.head_np_fam = FAM_NONE;
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            } else if ((t.si.particle == PART_IDAFA_NOUN && !op.is_idafa_decl) || t.si.particle == PART_NUM_IDAFA) {
                if (!tc.is_comp_11_19) {
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
            } else if (!c_part_non_noun || op.is_idafa_decl) {
                if (c_gen) st.pp_state = 2;
                else if (st.pp_state == 1) st.pp_state = 2;

                if (st.pp_state == 0 && !is_adj_agree && !is_coord_agree) {
                    const uint8_t fk = st.kind[st.depth];
                    const uint8_t fs = st.stage[st.depth];
                    if (fk == FRM_VERB_ACT && fs == 0 && op.is_nom_ok && !tc.time_adv) {
                        st.stage[st.depth] = 1;
                    } else if (fk == FRM_VERB_PASS && fs == 0 && op.is_nom_ok) {
                        st.stage[st.depth] = 1;
                    } else if (fk == FRM_INNA && fs == 0 && op.is_acc_ok) {
                        st.stage[st.depth] = 1;
                    } else if (fk == FRM_INNA && fs == 1 && op.is_nom_ok) {
                        st.stage[st.depth] = 2;
                    } else if (fk == FRM_KANA && fs == 0 && op.is_nom_ok) {
                        st.stage[st.depth] = 1;
                    } else if (fk == FRM_KANA && fs == 1 && op.is_acc_ok) {
                        st.stage[st.depth] = 2;
                    } else if (fk == FRM_NOM && fs == 0 && op.is_nom_ok) {
                        st.stage[st.depth] = 1;
                    } else if (fk == FRM_NOM && fs == 1 && op.is_nom_ok && !c_al) {
                        st.stage[st.depth] = 2;
                    }
                }

                if (op.can_open_idafa) st.idafa_state = 1;

                const uint8_t eff_track_fam = ((op.is_invar && (st.pp_state > 0 || st.idafa_state == 1)) ||
                                               ((cf == FAM_NONE || cf == FAM_SUKUN) && st.pp_state > 0))
                                              ? static_cast<uint8_t>(FAM_KASRA) : cf;
                if (st.prev_np_al == 0 && (c_al || op.is_invar) && st.prev_np_fam != FAM_NONE &&
                    st.prev_np_fam != FAM_KASRA && eff_track_fam == FAM_KASRA) {
                    st.mudaf_head_fam = st.prev_np_fam;
                } else if (!c_al && !op.is_invar) {
                    st.mudaf_head_fam = FAM_NONE;
                }
                if (st.prev_np_fam == FAM_NONE || !is_adj_agree) {
                    if (!st.conj_pending && !op.has_attached_conj) {
                        st.head_np_fam = eff_track_fam;
                        st.head_np_al = c_al ? 1 : 0;
                        st.head_np_pp = (st.pp_state > 0) ? 1 : 0;
                    }
                }
                st.prev_np_fam = eff_track_fam;
                st.prev_np_al = c_al ? 1 : 0;
                st.prev_np_tan = op.tan ? 1 : 0;
                st.prev_np_adj = t.is_adj_like;
                st.prev_np_suf = (c.flags & CF_SUFFIX) ? 1 : 0;
                st.prev_np_pp = (st.pp_state > 0) ? 1 : 0;
                st.last_was_title = tc.is_title ? 1 : 0;
                st.mood_expect = FAM_NONE;
            } else {
                st.mood_expect = FAM_NONE;
                st.last_was_title = 0;
            }

            if (op.invar_open_idafa) {
                st.idafa_state = 1;
            }

            if (is_seg_end && (st.kind[st.depth] == FRM_INNA || st.kind[st.depth] == FRM_KANA) &&
                st.stage[st.depth] >= 2) {
                syn += 250;
            }

            st.last_role = op.role;
            st.last_end = op.ce;
            return syn;
        };

        auto build_beam_opts = [&](size_t tok_i, const Token& t, uint32_t base_k, bool is_core_locked, std::vector<uint32_t>& out_opts) {
            const Cand& base_c = ctx.cands[t.cand_begin + base_k];
            const uint32_t dbase = dp_off[tok_i];
            const CandMeta& base_cm = cmeta[dbase + base_k];
            const size_t lam_skip = (t.si.al_len >= 2) ? static_cast<size_t>(t.si.al_lam_pos) : static_cast<size_t>(-1);
            const uint16_t base_core_h = base_cm.core_h;

            const size_t start_sz = out_opts.size();
            out_opts.push_back(base_k);
            const bool base_foreign_sukun = (!(base_c.flags & CF_EXPANDED) &&
                                             base_cm.cf == FAM_SUKUN &&
                                             base_c.verb_pattern == 0 && t.si.particle == PART_NONE && t.si.al_len == 0);
            if (t.cand_count >= 2 && !base_foreign_sukun) {
                const bool part_alt = !is_core_locked &&
                                      ((t.si.particle != PART_NONE) || ((t.surf_flags & (SF_ALI | SF_MAN)) != 0));
                const bool can_fem_past = !is_core_locked && (t.n >= 5 && ctx.syms[t.off + t.n - 1] == S_TEH && base_c.verb_pattern == 1);
                const bool base_past_fem = (base_c.verb_pattern == 1 && (base_c.ext_flags & EF_PAST_TEH_FEM) != 0);
                const bool is_imperf_noon_var = (base_c.verb_pattern == 1 && ((t.xflags & (XF_ENDS_WAW_NOON | XF_STARTS_IMPERF)) == (XF_ENDS_WAW_NOON | XF_STARTS_IMPERF)));
                for (uint32_t k = 0; k < t.cand_count && (out_opts.size() - start_sz) < 8; ++k) {
                    if (k == base_k) continue;
                    const Cand& ak = ctx.cands[t.cand_begin + k];
                    if (ak.emis + 4200 < base_c.emis) continue;
                    bool same_core = (ak.case_pos == base_c.case_pos);
                    if (same_core) {
                        if (is_core_locked) {
                            same_core = (cmeta[dbase + k].core_h == base_core_h);
                        } else {
                            for (size_t p = 0; same_core && p < base_c.case_pos; ++p) {
                                if (p != lam_skip && ak.diac[p] != base_c.diac[p]) same_core = false;
                            }
                        }
                    }
                    const bool fem_past_alt = can_fem_past && ak.verb_pattern == 1 &&
                                              ((ak.ext_flags & EF_PAST_TEH_FEM) != (base_c.ext_flags & EF_PAST_TEH_FEM)) &&
                                              ak.diac[t.n - 1] == DIAC_SUKUN;
                    const bool is_past_fem_sukun_var = base_past_fem && ((ak.ext_flags & EF_PAST_TEH_FEM) != 0);
                    if ((same_core && !is_past_fem_sukun_var && !is_imperf_noon_var) || part_alt || fem_past_alt) {
                        out_opts.push_back(k);
                    }
                }
            }
        };

        auto run_case_beam = [&](const std::vector<int32_t>* extra_bonus, int32_t syn_scale,
                                 const std::vector<uint8_t>* core_locked = nullptr,
                                 const std::vector<uint32_t>* pre_opts = nullptr,
                                 const std::vector<uint32_t>* pre_opts_off = nullptr) {
            const bool use_tr3 = is_v7_ && trans3_ && trans3_size_ == TRANS3_TABLE_SIZE && v7_weights_[V7W_TRANS3] != 0;
            const bool use_val_g = is_v7_ && valency_.valid() && v7_weights_[V7W_VALENCY] != 0;
            const int32_t val_w_g = use_val_g ? v7_weights_[V7W_VALENCY] : 0;
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
                        const Token* nx = (i + 1 < seg_e) ? &ctx.toks[i + 1] : nullptr;
                        const bool is_seg_end = (i + 1 == seg_e);

                        const uint32_t* opts_ptr = nullptr;
                        size_t opts_cnt = 0;
                        if (pre_opts != nullptr && pre_opts_off != nullptr) {
                            const uint32_t os = (*pre_opts_off)[i];
                            const uint32_t oe = (*pre_opts_off)[i + 1];
                            opts_ptr = pre_opts->data() + os;
                            opts_cnt = oe - os;
                        } else {
                            opts.clear();
                            const bool is_core_locked = (core_locked != nullptr && (*core_locked)[i] != 0);
                            build_beam_opts(i, t, chosen[i], is_core_locked, opts);
                            opts_ptr = opts.data();
                            opts_cnt = opts.size();
                        }

                        // Hoist token-level syntactic invariants once per token
                        const uint8_t* ts_i = &ctx.syms[t.off];
                        SynTokCtx tc;
                        tc.ts = ts_i;
                        tc.use_val = use_val_g;
                        tc.val_w = val_w_g;
                        tc.val_cur = tok_val[i];
                        if (use_val_g && pt && pt->word_id < valency_.count() && pt->si.particle == PART_NONE) {
                            tc.val_pt = tok_val[i - 1];
                        }
                        tc.is_kana_v = (t.si.particle == PART_KANA) || ((t.xflags & XF_IS_KANA_VERB) != 0);
                        tc.is_conj_tok = (t.si.particle == PART_CONJ);
                        tc.starts_imperf = (t.xflags & XF_STARTS_IMPERF) != 0;
                        if (tc.starts_imperf && !tc.is_kana_v && (t.xflags & XF_ENDS_WAW_NOON)) {
                            size_t st_imp = (t.n >= 4 && (ts_i[0] == S_WAW || ts_i[0] == S_FEH)) ? 1 : 0;
                            if (t.n - st_imp >= 4 && ts_i[st_imp] == S_SEEN) ++st_imp;
                            tc.is_imperf_wn = (t.n - st_imp) >= 6;
                        }
                        tc.an_in_tok = (t.si.particle == PART_AN || t.si.particle == PART_LI_AN || t.si.particle == PART_IN);
                        if (tc.an_in_tok) {
                            tc.an_in_nx_verb = nx && (nx->top_verb == 1 || nx->top_pos == POS_VERB_PAST_SUBJ ||
                                                      ((nx->xflags & XF_ENDS_WAW_ALEF) != 0) ||
                                                      nx->si.verb_like == 2 ||
                                                      (nx->si.verb_like == 1 && !nx->has_noun_lex) ||
                                                      nx->si.particle == PART_LA || nx->si.particle == PART_JAZM ||
                                                      nx->si.particle == PART_QAD ||
                                                      (t.si.particle == PART_IN && (nx->si.particle == PART_KANA ||
                                                       ((nx->xflags & XF_IS_KANA_VERB) != 0))));
                            if (!tc.an_in_nx_verb) {
                                tc.an_in_nx_noun = nx && ((nx->si.particle == PART_NONE && nx->si.verb_like == 0 && !(nx->surf_flags & SF_THEMMA)) ||
                                                          (t.si.particle == PART_IN && (nx->si.particle == PART_PRON || nx->si.particle == PART_DEM ||
                                                                                        (nx->surf_flags & SF_INNA_PRON))));
                            }
                        }
                        tc.man_nx_trig = ((t.surf_flags & SF_MAN) != 0) && !(nx && (nx->surf_flags & SF_THEMMA)) &&
                                         t.n >= 2 && nx &&
                                         (nx->si.particle == PART_PRON || nx->si.particle == PART_KANA ||
                                          nx->si.particle == PART_JAZM || (nx->top_verb == 1 && !nx->has_noun_lex));
                        tc.pt_num_gen = pt && pt->digit_after && (pt->num_after_class == 2 || pt->num_after_class == 4);
                        tc.pt_is_ay = pt && pt->si.particle == PART_AY;
                        tc.time_adv = (t.xflags & XF_IS_TIME_ADV) != 0;
                        tc.lex_adv = tc.time_adv || (use_val_g && tc.val_cur.word_syn_type == 3);
                        tc.is_title = (t.xflags & XF_IS_TITLE) != 0;
                        tc.subj_cue = (nx && ((nx->xflags & XF_IS_INA_CLAUSE) != 0)) || tc.is_title ||
                                      (nx && (nx->surf_flags & SF_NAME_HEAD));
                        tc.after_innama = pt && ((pt->xflags & XF_IS_INNAMA) != 0);
                        tc.is_masdar_adv = (t.xflags & XF_IS_MASDAR_ADV) != 0;
                        tc.is_innama = (t.xflags & XF_IS_INNAMA) != 0;
                        tc.is_dem_before_al = (t.si.particle == PART_DEM && ((t.xflags & XF_IS_DEM_DET) != 0) &&
                                               nx && nx->si.al_len > 0 && !nx->si.has_prep);
                        tc.can_have_mudaf_ilayh = nx && (nx->si.particle == PART_NONE ||
                                                         nx->si.particle == PART_IDAFA_NOUN ||
                                                         nx->si.particle == PART_NUM_IDAFA ||
                                                         nx->si.particle == PART_DEM ||
                                                         (nx->surf_flags & SF_ALI)) &&
                                                  !(nx->surf_flags & SF_INNA_PRON) &&
                                                  !nx->si.has_prep && nx->top_verb != 1 && nx->si.verb_like < 2;
                        tc.nx_noun_hatta = nx && nx->si.particle == PART_NONE && nx->top_verb != 1 && nx->si.verb_like < 2;
                        tc.is_comp_11_19 = (t.si.particle == PART_NUM_IDAFA && (t.surf_flags & SF_ASHAR) &&
                                            pt && (pt->surf_flags & SF_COMP_NUM));
                        tc.idafa_has_arg = nx && (nx->si.particle == PART_NONE || nx->si.particle == PART_DEM ||
                                                  nx->si.particle == PART_IDAFA_NOUN || nx->si.particle == PART_NUM_IDAFA);
                        {
                            const bool afal_super = (t.n == t.si.al_len + 4 && ts_i[t.si.al_len] == S_ALEF_HAMZA_A);
                            const bool nx_coord_mudaf = afal_super && nx &&
                                                        (nx->is_adj_like ||
                                                         ((ctx.syms[nx->off] == S_WAW || ctx.syms[nx->off] == S_FEH) &&
                                                          nx->n == 5 && ctx.syms[nx->off + 1] == S_ALEF_HAMZA_A));
                            tc.nx_mudaf_base = nx_coord_mudaf ||
                                               (nx && nx->si.particle == PART_NONE &&
                                                nx->si.verb_like < 2 && nx->top_verb != 1 &&
                                                !nx->is_adj_like && !nx->is_ibn && !nx->si.has_prep);
                        }
                        tc.end_khabar_only_base = (!nx || (nx->si.particle != PART_NONE &&
                                                           nx->si.particle != PART_IDAFA_NOUN &&
                                                           !nx->digit_after));
                        tc.afal_pass_blk = (t.n == 4 && ts_i[0] == S_ALEF_HAMZA_A);
                        tc.is_afal_maksura_nx = (t.n == 4 && ts_i[0] == S_ALEF_HAMZA_A && ts_i[3] == S_ALEF_MAKSURA &&
                                                 nx && nx->si.particle == PART_NONE && !nx->si.has_prep);
                        tc.nx_has_al = nx && nx->si.al_len > 0;
                        tc.comma_waw_conj = (t.boundary_before && (t.bkind_mask & BK_COMMA) && ts_i[0] == S_WAW);
                        tc.pt_is_prep_pron = pt && pt->si.particle == PART_PREP_PRON;
                        tc.pt_no_digit = pt && !pt->digit_after;
                        tc.non_idafa_mabni = (t.xflags & XF_NON_IDAFA_MABNI) != 0;
                        tc.decl_surf_blk = ((t.surf_flags & (SF_BAAD_QABL | SF_AMS | SF_INNA_PRON | SF_FIVE_NOUN | SF_COMP_NUM | SF_ASHAR)) != 0) || t.digit_after;

                        // Hoist option-level invariants once per option (reused across all BEAM_W hyps)
                        SynOpt opt_info[16];
                        uint16_t opt_tr_col[16];
                        const size_t n_opts_eff = std::min<size_t>(opts_cnt, 16u);
                        const uint32_t dbase = dp_off[i];
                        for (size_t oi = 0; oi < n_opts_eff; ++oi) {
                            const uint32_t ok = opts_ptr[oi];
                            const Cand& cc = ctx.cands[t.cand_begin + ok];
                            const CandMeta& cm = cmeta[dbase + ok];
                            SynOpt& op = opt_info[oi];
                            op.cc = &cc;
                            op.ok = ok;
                            op.ce = cm.ce;
                            op.cf = cm.eff_fam;
                            op.role = cm.role;
                            op.st3 = cm.st3;
                            opt_tr_col[oi] = trans_col_index(cm.role, cm.ce);
                            const bool ambig_obl = (cm.bits & CMB_AMBIG_OBL) != 0;
                            op.tan = (cm.bits & CMB_TAN) != 0;
                            op.c_al = (cm.bits & CMB_HAS_AL) != 0;
                            op.c_gen = (cm.bits & CMB_HAS_GEN) != 0;
                            const bool c_ali_prop = (cm.bits & CMB_ALI_PROP) != 0;
                            op.is_invar = (cm.bits & CMB_INVAR_SYN) != 0;
                            op.has_vobj = (cm.bits & CMB_HAS_VOBJ) != 0;
                            op.c_part_non_noun = (t.si.particle != PART_NONE && !c_ali_prop);
                            op.is_idafa_decl = (t.si.particle == PART_IDAFA_NOUN && !(t.surf_flags & SF_FIVE_NOUN) && !op.is_invar);
                            op.is_decl_noun = (cc.verb_pattern == 0) && (!op.c_part_non_noun || op.is_idafa_decl) &&
                                              !op.is_invar && !tc.decl_surf_blk &&
                                              op.cf != FAM_NONE && op.cf != FAM_SUKUN;
                            op.is_bare_noun = op.is_decl_noun && !op.c_gen;
                            op.has_attached_conj = (!op.c_part_non_noun && t.si.has_conj && !t.si.has_prep);
                            op.is_verb_cand = (cc.verb_pattern == 1) && !tc.is_kana_v;
                            op.is_fem_at = (cc.ext_flags & EF_FEM_PL_AT) != 0;
                            op.is_dip = (cc.ext_flags & EF_DIPTOTE) != 0;
                            op.is_dual_pl = (cc.ext_flags & (EF_DUAL_NOON | EF_MASC_PL_NOON)) != 0;
                            op.is_nom_ok = (op.cf == FAM_DAMMA);
                            op.is_acc_ok = (!op.is_fem_at && op.cf == FAM_FATHA) || (op.is_fem_at && op.cf == FAM_KASRA) || ambig_obl;
                            op.oov_sukun = ((cc.flags & CF_OOV) && op.cf == FAM_SUKUN);
                            op.foreign_sukun_noun = (op.cf == FAM_SUKUN && cc.verb_pattern == 0 && !op.c_part_non_noun &&
                                                     t.si.al_len == 0 && !(cc.flags & CF_EXPANDED));
                            if (op.is_dip && !op.c_al && !op.is_dual_pl) {
                                op.proper_dip = (t.is_diptote_lex == 2) ||
                                                (classify_diptote_stem(ts_i, t.n, t.si.al_len, cc.diac) == 2);
                            }
                            int32_t ssyn = 0;
                            const bool sh = diac_has_shadda(op.ce);
                            if (tc.an_in_tok) {
                                if (tc.an_in_nx_verb) ssyn += sh ? -1200 : 1200;
                                else if (tc.an_in_nx_noun) ssyn += sh ? 850 : -650;
                            }
                            if (tc.man_nx_trig && cc.case_pos == t.n - 1) {
                                ssyn += (cc.diac[t.n - 2] == DIAC_FATHA) ? 1100 : -1100;
                            }
                            if ((op.is_verb_cand || tc.is_kana_v) && tc.starts_imperf && !op.c_al &&
                                !(cc.flags & (CF_SUFFIX | CF_LAM_NASB)) && cc.case_pos == t.n - 1 && !op.is_invar) {
                                if (tc.is_imperf_wn) {
                                    ssyn += (op.ce == DIAC_FATHA) ? 1200 : -1500;
                                } else {
                                    op.imperf_mood_elig = true;
                                }
                            }
                            op.static_syn = ssyn;
                            op.is_part_inna_sh = (t.si.particle == PART_INNA) ||
                                                 ((t.si.particle == PART_AN || t.si.particle == PART_LI_AN ||
                                                   t.si.particle == PART_LAKIN || t.si.particle == PART_IN) && sh);
                            op.is_part_nasb_eff = (t.si.particle == PART_NASB) ||
                                                  ((t.si.particle == PART_AN || t.si.particle == PART_LI_AN) && !sh);
                            op.is_part_jazm_eff = (t.si.particle == PART_JAZM) || (t.si.particle == PART_IN && !sh);
                            op.can_open_idafa = (!op.c_al && !(cc.flags & CF_SUFFIX) && !op.tan && (t.is_diptote_lex != 2) &&
                                                 !tc.non_idafa_mabni && !op.oov_sukun &&
                                                 !(op.cf == FAM_SUKUN && t.si.al_len == 0 && !(cc.flags & CF_EXPANDED)) &&
                                                 !op.is_dual_pl && !t.digit_after) ||
                                                (op.is_idafa_decl && !op.tan && tc.idafa_has_arg);
                            op.invar_open_idafa = (!op.c_part_non_noun && !op.is_verb_cand && op.is_invar && !op.c_al &&
                                                   !(cc.flags & CF_SUFFIX) && !op.tan && t.is_diptote_lex != 2 && !t.digit_after) ||
                                                  (tc.is_afal_maksura_nx && !(cc.flags & CF_HAS_AL));
                            op.coord_al_fallback = (!op.c_al && tc.nx_has_al) || (tc.comma_waw_conj && op.has_attached_conj);
                            const int32_t ex_v = extra_bonus ? (*extra_bonus)[dbase + ok] : 0;
                            op.base_score = cc.emis + ex_v + dp_bg[dbase + ok];
                        }

                        beam_nxt.clear();
                        const int32_t* pbin_base = pt ? &pair_bin[pair_off[i]] : nullptr;
                        const uint32_t ccnt = t.cand_count;
                        for (const BeamHyp& h : beam_cur) {
                            const Cand* pc_ptr = pt ? &ctx.cands[pt->cand_begin + h.cand_k] : nullptr;
                            const int32_t* pbin_row = pbin_base ? (pbin_base + h.cand_k * ccnt) : nullptr;
                            const int16_t* tr_row = pt
                                ? trans_scaled16_row(h.st.last_role, h.st.last_end)
                                : trans_scaled16_row(0, DIAC_NONE);
                            const int8_t* tr3_row = use_tr3
                                ? trans3_row_ptr(h.st.prev2_st3, h.st.prev1_st3)
                                : nullptr;
                            for (size_t oi = 0; oi < n_opts_eff; ++oi) {
                                const SynOpt& op = opt_info[oi];
                                int32_t trans_v = (tr_row && opt_tr_col[oi] != UINT16_MAX)
                                    ? static_cast<int32_t>(tr_row[opt_tr_col[oi]])
                                    : (pt
                                        ? trans_score_scaled(h.st.last_role, h.st.last_end, op.role, op.ce)
                                        : trans_score_scaled(0, DIAC_NONE, op.role, op.ce));
                                if (pt && use_v9_neural && h.st.last_end < NUM_DIACRITICS && op.ce < NUM_DIACRITICS) {
                                    trans_v += v9_crf_sc[h.st.last_end][op.ce];
                                }
                                const int32_t bin_v = pbin_row ? pbin_row[op.ok] : 0;
                                SynState nst = h.st;
                                int32_t syn_v = step_syn(nst, i, t, tc, op, pt, pc_ptr, nx, is_seg_end);
                                if (syn_scale != 1000) {
                                    syn_v = static_cast<int32_t>((static_cast<int64_t>(syn_v) * syn_scale) / 1000);
                                }
                                int32_t tr3_v = 0;
                                if (use_tr3) {
                                    if (tr3_row && op.st3 < NUM_TRANS3_STATES) {
                                        tr3_v = trans3_lut_[static_cast<uint8_t>(tr3_row[op.st3])];
                                    }
                                    nst.prev2_st3 = h.st.prev1_st3;
                                    nst.prev1_st3 = op.st3;
                                }
                                const int32_t total = h.score + op.base_score + trans_v + tr3_v + bin_v + syn_v;

                                const uint32_t bp_id = static_cast<uint32_t>(beam_bp.size());
                                beam_bp.push_back({h.prev_node, op.ok});
                                BeamHyp nh;
                                nh.st = nst;
                                nh.score = total;
                                nh.prev_node = bp_id;
                                nh.cand_k = op.ok;
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
                                    if (kept.st.last_end == cnd.st.last_end &&
                                        kept.st.prev_np_fam == cnd.st.prev_np_fam &&
                                        kept.st.head_np_fam == cnd.st.head_np_fam &&
                                        kept.st.pp_state == cnd.st.pp_state &&
                                        kept.st.idafa_state == cnd.st.idafa_state &&
                                        kept.st.depth == cnd.st.depth &&
                                        kept.st.kind[kept.st.depth] == cnd.st.kind[cnd.st.depth] &&
                                        kept.st.stage[kept.st.depth] == cnd.st.stage[cnd.st.depth]) {
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
                    const CandMeta& sm1 = cmeta[dp_off[seg_s] + chosen[seg_s]];
                    uint8_t f1 = sm1.eff_fam;
                    if (sc1.verb_pattern == 0 && (st1.si.particle == PART_NONE || st1.si.particle == PART_IDAFA_NOUN) &&
                        f1 != FAM_NONE && f1 != FAM_SUKUN) {
                        prev_seg_head_fam = f1;
                        prev_seg_head_al = (sm1.bits & CMB_HAS_AL) ? 1 : 0;
                        prev_seg_head_pp = (sm1.bits & CMB_HAS_GEN) ? 1 : 0;
                    } else {
                        prev_seg_head_fam = FAM_NONE;
                    }
                }
                seg_s = seg_e;
            }
        };

        // Pass-1 Beam Search
        run_case_beam(nullptr, use_v9_neural ? v9_weights_[V9W_SYN_KEEP] : 1000);

        // Pass-2 Stacked Bidirectional Core2-GBDT Candidate Ranker (V8)
        auto& core2_locked = sc_buf.core2_locked;
        bool has_core2_locks = false;
        if (core2_dump_out != nullptr || (is_v8_ && core2_trees_.valid() && v8_weights_[V8W_CORE2] != 0)) {
            auto clamp_c2 = [](int32_t v) -> int32_t { return v < -8 ? -8 : (v > 119 ? 119 : v); };
            auto clamp_c2_i8 = [](int32_t v) -> int8_t { return static_cast<int8_t>(v < -8 ? -8 : (v > 119 ? 119 : v)); };
            auto& c2_pos = sc_buf.c2_pos;
            auto& c2_role = sc_buf.c2_role;
            auto& c2_fam = sc_buf.c2_fam;
            auto& c2_mc = sc_buf.c2_mc;
            auto& c2_al = sc_buf.c2_al;
            auto& c2_gen = sc_buf.c2_gen;
            auto& c2_verb = sc_buf.c2_verb;
            auto& c2_pass = sc_buf.c2_pass;
            auto& c2_tan = sc_buf.c2_tan;
            auto& c2_suf = sc_buf.c2_suf;
            auto& prev_vdist = sc_buf.prev_vdist;
            auto& next_vdist = sc_buf.next_vdist;
            c2_pos.assign(W, 0); c2_role.assign(W, 0); c2_fam.assign(W, 0); c2_mc.assign(W, 0);
            c2_al.assign(W, 0); c2_gen.assign(W, 0); c2_verb.assign(W, 0); c2_pass.assign(W, 0); c2_tan.assign(W, 0); c2_suf.assign(W, 0);
            prev_vdist.assign(W, -1); next_vdist.assign(W, -1);
            int last_v = -1;
            for (size_t i = 0; i < W; ++i) {
                const Token& tk = ctx.toks[i];
                if (tk.boundary_before) last_v = -1;
                prev_vdist[i] = (last_v >= 0) ? static_cast<int8_t>(std::min<int>(15, static_cast<int>(i) - last_v)) : -1;
                if (tk.cand_count == 0) continue;
                const Cand& c = ctx.cands[tk.cand_begin + chosen[i]];
                const CandMeta& cm = cmeta[dp_off[i] + chosen[i]];
                c2_pos[i] = c.pos_tag;
                c2_role[i] = cm.role;
                c2_fam[i] = cm.eff_fam;
                c2_mc[i] = c.morph_class;
                c2_al[i] = (cm.bits & CMB_HAS_AL) ? 1 : 0;
                c2_gen[i] = (cm.bits & CMB_HAS_GEN) ? 1 : 0;
                c2_verb[i] = (c.verb_pattern == 1) ? 1 : 0;
                c2_pass[i] = (c.ext_flags & EF_PASSIVE) ? 1 : 0;
                c2_tan[i] = (cm.bits & CMB_TAN) ? 1 : 0;
                c2_suf[i] = (c.flags & CF_SUFFIX) ? 1 : 0;
                if (c2_verb[i]) last_v = static_cast<int>(i);
            }
            int next_v = -1;
            for (int ii = static_cast<int>(W) - 1; ii >= 0; --ii) {
                size_t i = static_cast<size_t>(ii);
                next_vdist[i] = (next_v >= 0) ? static_cast<int8_t>(std::min<int>(15, next_v - ii)) : -1;
                if (c2_verb[i]) next_v = ii;
                if (ctx.toks[i].boundary_before) next_v = -1;
            }

            auto& new_chosen = sc_buf.new_chosen;
            new_chosen.assign(chosen.begin(), chosen.end());
            if (core2_dump_out == nullptr) core2_locked.assign(W, 0);

            struct CoreGrp {
                uint16_t core_h = 0;
                uint32_t rep_k = 0;
                int32_t rep_score = NEG_INF;
                int32_t lex_max = NEG_INF;
                int32_t emis_max = NEG_INF;
                int32_t morph_sc = 0;
                int32_t cbg_l = 0;
                int32_t cbg_r = 0;
                int32_t cbg_c = 0;
                int32_t tr_sc = 0;
                int32_t bin_sc = 0;
                int32_t n_end_max = NEG_INF;
                int32_t n_fam_max = NEG_INF;
                int32_t n_tan_max = NEG_INF;
                uint8_t has_n_best_end = 0;
                uint8_t count = 0;
                uint8_t is_p1 = 0;
                uint8_t has_tan = 0;
                uint8_t has_kasra = 0;
                uint8_t has_non_exp = 0;
                uint8_t cr = 0;
            };

            auto get_nb_c2 = [&](size_t i, int delta) -> int {
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

            auto pick_best_k_in_group = [&](size_t tok_i, uint16_t target_core_h, uint32_t fallback_k) -> uint32_t {
                const Token& tk = ctx.toks[tok_i];
                const uint32_t dbase = dp_off[tok_i];
                const int im1 = get_nb_c2(tok_i, -1);
                const int ip1 = get_nb_c2(tok_i, 1);
                const Token* pt = (im1 >= 0) ? &ctx.toks[static_cast<size_t>(im1)] : nullptr;
                const Token* nx = (ip1 >= 0) ? &ctx.toks[static_cast<size_t>(ip1)] : nullptr;
                const uint32_t p_ch = (pt && pt->cand_count > 0) ? chosen[static_cast<size_t>(im1)] : 0u;
                const uint32_t n_ch = (nx && nx->cand_count > 0) ? chosen[static_cast<size_t>(ip1)] : 0u;
                const bool has_p = (pt && pt->cand_count > 0);
                const bool has_n = (nx && nx->cand_count > 0);
                const uint8_t pr = has_p ? c2_role[static_cast<size_t>(im1)] : 0u;
                const uint8_t pe = has_p ? ctx.cands[pt->cand_begin + p_ch].diac[ctx.cands[pt->cand_begin + p_ch].case_pos] : DIAC_NONE;
                const uint8_t nr = has_n ? c2_role[static_cast<size_t>(ip1)] : 0u;
                const uint8_t ne = has_n ? ctx.cands[nx->cand_begin + n_ch].diac[ctx.cands[nx->cand_begin + n_ch].case_pos] : DIAC_NONE;
                const int32_t* pbin_l = has_p ? &pair_bin[pair_off[tok_i] + p_ch * tk.cand_count] : nullptr;
                const int32_t* pbin_r = has_n ? &pair_bin[pair_off[tok_i + 1]] : nullptr;
                const uint32_t nx_ccnt = has_n ? nx->cand_count : 0u;

                uint32_t best_k = fallback_k;
                int32_t best_sc = NEG_INF;
                for (uint32_t k = 0; k < tk.cand_count; ++k) {
                    const CandMeta& cm = cmeta[dbase + k];
                    if (cm.core_h != target_core_h) continue;
                    const Cand& ck = ctx.cands[tk.cand_begin + k];
                    int32_t sc = ck.emis + dp_bg[dbase + k];
                    const uint8_t ce = cm.ce;
                    const uint8_t cr = cm.role;
                    if (has_p) {
                        sc += trans_score_scaled(pr, pe, cr, ce) + pbin_l[k];
                        if (use_v9_neural && pe < NUM_DIACRITICS && ce < NUM_DIACRITICS) {
                            sc += v9_crf_sc[pe][ce];
                        }
                    } else {
                        sc += trans_score_scaled(0, DIAC_NONE, cr, ce);
                    }
                    if (has_n) {
                        sc += trans_score_scaled(cr, ce, nr, ne) + pbin_r[k * nx_ccnt + n_ch];
                        if (use_v9_neural && ce < NUM_DIACRITICS && ne < NUM_DIACRITICS) {
                            sc += v9_crf_sc[ce][ne];
                        }
                    }
                    if (sc > best_sc) {
                        best_sc = sc;
                        best_k = k;
                    }
                }
                return best_k;
            };

            const bool use_fast_c2 = (core2_dump_out == nullptr) && core2_trees_.has_fast_v5() && (core2_trees_.num_classes() >= 2);
            const size_t c2_fc_lim = std::min<size_t>(core2_feat_count_, CORE2_NUM_FEATURES);
            alignas(64) int8_t fc_base[128];
            if (use_fast_c2) {
                std::memset(fc_base, 0xFF, 128);
                sc_buf.c2_slices.clear();
                sc_buf.c2_feat_buf.clear();
            }

            const float* c2_end_lp_base = use_v9_neural ? sc_buf.v9_ws.end_logp.data() : nullptr;
            const float* c2_fam_lp_base = use_v9_neural ? sc_buf.v9_ws.fam_logp.data() : nullptr;
            const float* c2_tan_lp_base = use_v9_neural ? sc_buf.v9_ws.tan_logp.data() : nullptr;

            int32_t fc[CORE2_NUM_FEATURES];
            for (size_t i = 0; i < W; ++i) {
                const Token& t = ctx.toks[i];
                if (t.cand_count < 2) continue;
                const uint8_t* ts = &ctx.syms[t.off];
                const uint8_t lam_skip = (t.si.al_len >= 2) ? t.si.al_lam_pos : 255;
                const uint32_t dbase = dp_off[i];
                const float* el_i = c2_end_lp_base ? (c2_end_lp_base + i * NUM_DIACRITICS) : nullptr;
                const float* fl_i = c2_fam_lp_base ? (c2_fam_lp_base + i * 5) : nullptr;
                const float* tl_i = c2_tan_lp_base ? (c2_tan_lp_base + i * 2) : nullptr;
                uint8_t n_best_end = 255;
                if (el_i) {
                    float best_el = -1e30f;
                    for (uint8_t e = 0; e < NUM_DIACRITICS; ++e) {
                        if (el_i[e] > best_el) { best_el = el_i[e]; n_best_end = e; }
                    }
                }

                CoreGrp grps[8];
                int n_grp = 0;
                int p1_g = -1;
                for (uint32_t k = 0; k < t.cand_count; ++k) {
                    const Cand& ck = ctx.cands[t.cand_begin + k];
                    const CandMeta& cm = cmeta[dbase + k];
                    uint16_t hk = cm.core_h;
                    int gi = -1;
                    for (int g = 0; g < n_grp; ++g) {
                        if (grps[g].core_h == hk) { gi = g; break; }
                    }
                    if (gi < 0) {
                        if (n_grp >= 8) continue;
                        gi = n_grp++;
                        grps[gi].core_h = hk;
                        grps[gi].rep_k = k;
                    }
                    CoreGrp& gr = grps[gi];
                    ++gr.count;
                    if (cm.bits & CMB_TAN) gr.has_tan = 1;
                    if (cm.cf == FAM_KASRA) gr.has_kasra = 1;
                    if (el_i) {
                        const uint8_t ce = cm.ce;
                        const uint8_t cf = cm.cf;
                        const size_t ti = (cm.bits & CMB_TAN) ? 1u : 0u;
                        const int32_t nev = (ce < NUM_DIACRITICS) ? static_cast<int32_t>(std::lrintf(el_i[ce] * 100.0f)) : -1000;
                        const int32_t nfv = (cf < 5) ? static_cast<int32_t>(std::lrintf(fl_i[cf] * 100.0f)) : -1000;
                        const int32_t ntv = static_cast<int32_t>(std::lrintf(tl_i[ti] * 100.0f));
                        if (nev > gr.n_end_max) gr.n_end_max = nev;
                        if (nfv > gr.n_fam_max) gr.n_fam_max = nfv;
                        if (ntv > gr.n_tan_max) gr.n_tan_max = ntv;
                        if (ce == n_best_end) gr.has_n_best_end = 1;
                    }
                    const bool is_exp = (ck.flags & CF_EXPANDED) != 0;
                    if (!is_exp) {
                        if (!gr.has_non_exp || ck.lex_emis > gr.lex_max) gr.lex_max = ck.lex_emis;
                        gr.has_non_exp = 1;
                    } else if (!gr.has_non_exp && ck.lex_emis > gr.lex_max) {
                        gr.lex_max = ck.lex_emis;
                    }
                    int32_t em = ck.emis + dp_bg[dbase + k];
                    if (em > gr.emis_max) gr.emis_max = em;
                    if (k == chosen[i]) {
                        gr.is_p1 = 1;
                        gr.rep_k = k;
                        gr.rep_score = em;
                        gr.morph_sc = ck.morph_sc;
                        p1_g = gi;
                    } else if (!gr.is_p1) {
                        int32_t rsc = em + (is_exp ? -100 : 0);
                        if (rsc > gr.rep_score) {
                            gr.rep_score = rsc;
                            gr.rep_k = k;
                            gr.morph_sc = ck.morph_sc;
                        }
                    }
                }
                if (n_grp < 2 || p1_g < 0) continue;

                // If dumping training data, verify exact unique gold core match
                uint8_t gold_match[8] = {};
                uint8_t p1_correct = 0;
                uint32_t cur_gid = 0;
                if (core2_dump_out != nullptr) {
                    if (!gold_diacs || i >= gold_diacs->size()) continue;
                    const auto& gd = (*gold_diacs)[i];
                    bool any_core_voc = false;
                    for (uint8_t p = 0; p < t.n; ++p) {
                        if (p == lam_skip) continue;
                        if (gd[p] != DIAC_NONE) { any_core_voc = true; break; }
                    }
                    if (!any_core_voc) continue;
                    int n_pos = 0;
                    for (int g = 0; g < n_grp; ++g) {
                        const Cand& rc = ctx.cands[t.cand_begin + grps[g].rep_k];
                        uint16_t gh = compute_core_hash(ts, gd.data(), rc.case_pos, lam_skip);
                        if (gh == grps[g].core_h) { gold_match[g] = 1; ++n_pos; }
                    }
                    if (n_pos != 1) continue;
                    p1_correct = gold_match[p1_g];
                    cur_gid = core2_group_id ? (*core2_group_id)++ : 0;
                }

                const int im2 = get_nb_c2(i, -2), im1 = get_nb_c2(i, -1);
                const int ip1 = get_nb_c2(i, 1), ip2 = get_nb_c2(i, 2);
                const Token* pt = (im1 >= 0) ? &ctx.toks[static_cast<size_t>(im1)] : nullptr;
                const Token* p2 = (im2 >= 0) ? &ctx.toks[static_cast<size_t>(im2)] : nullptr;
                const Token* nx = (ip1 >= 0) ? &ctx.toks[static_cast<size_t>(ip1)] : nullptr;
                const Token* nx2 = (ip2 >= 0) ? &ctx.toks[static_cast<size_t>(ip2)] : nullptr;

                // Reuse Pass-1 CoreBg lookup results when already queried
                CoreBgLookupResult el_v{}, er_v{}, ec_v{};
                bool has_el = false, has_er = false, has_ec = false;
                if (!core_bg_.empty() && t.word_id != UINT32_MAX) {
                    const CoreBgCacheSlot& cs = sc_buf.cbg_cache[i];
                    if (pt && pt->word_id != UINT32_MAX) {
                        if (cs.queried_mask & 1u) {
                            has_el = (cs.hit_mask & 1u) != 0;
                            if (has_el) el_v = cs.el;
                        } else {
                            uint32_t ws[2] = {pt->word_id, t.word_id};
                            has_el = core_bg_.find_val(CORE_DIR_LEFT, ws, 2, el_v);
                        }
                    }
                    if (nx && nx->word_id != UINT32_MAX) {
                        if (cs.queried_mask & 2u) {
                            has_er = (cs.hit_mask & 2u) != 0;
                            if (has_er) er_v = cs.er;
                        } else {
                            uint32_t ws[2] = {t.word_id, nx->word_id};
                            has_er = core_bg_.find_val(CORE_DIR_RIGHT, ws, 2, er_v);
                        }
                    }
                    if (pt && nx && pt->word_id != UINT32_MAX && nx->word_id != UINT32_MAX) {
                        if (cs.queried_mask & 4u) {
                            has_ec = (cs.hit_mask & 4u) != 0;
                            if (has_ec) ec_v = cs.ec;
                        } else {
                            uint32_t ws[3] = {pt->word_id, t.word_id, nx->word_id};
                            has_ec = core_bg_.find_val(CORE_DIR_CTR3, ws, 3, ec_v);
                        }
                    }
                }

                // Compute contrast bitmask + per-group core_bg + transition/binary scores
                bool has_act_v = false, has_pass_v = false, has_verb_g = false, has_noun_g = false;
                bool has_sh_core = false, has_nosh_core = false;
                bool has_suk_core = false, has_nosuk_core = false;
                bool has_part_sh = false, has_part_nosh = false;
                const Cand* pc_p1 = (pt && pt->cand_count > 0) ? &ctx.cands[pt->cand_begin + chosen[static_cast<size_t>(im1)]] : nullptr;
                const Cand* nc_p1 = (nx && nx->cand_count > 0) ? &ctx.cands[nx->cand_begin + chosen[static_cast<size_t>(ip1)]] : nullptr;
                const uint8_t st = static_cast<uint8_t>(std::min<size_t>(t.si.al_len, t.n - 1));
                const int32_t* pbin_l = (pt && pc_p1) ? &pair_bin[pair_off[i] + chosen[static_cast<size_t>(im1)] * t.cand_count] : nullptr;
                const int32_t* pbin_r = (nx && nc_p1) ? &pair_bin[pair_off[i + 1]] : nullptr;
                const uint32_t nx_ccnt = nx ? nx->cand_count : 0u;
                const uint32_t nx_ch = (nx && nc_p1) ? chosen[static_cast<size_t>(ip1)] : 0u;

                for (int g = 0; g < n_grp; ++g) {
                    CoreGrp& gr = grps[g];
                    const Cand& rc = ctx.cands[t.cand_begin + gr.rep_k];
                    const CandMeta& cm_g = cmeta[dbase + gr.rep_k];
                    if (has_el) {
                        if (gr.core_h == el_v.best_core_h) gr.cbg_l = el_v.best_bonus;
                        else if (gr.core_h == el_v.alt_core_h) gr.cbg_l = el_v.alt_bonus;
                    }
                    if (has_er) {
                        if (gr.core_h == er_v.best_core_h) gr.cbg_r = er_v.best_bonus;
                        else if (gr.core_h == er_v.alt_core_h) gr.cbg_r = er_v.alt_bonus;
                    }
                    if (has_ec) {
                        if (gr.core_h == ec_v.best_core_h) gr.cbg_c = ec_v.best_bonus;
                        else if (gr.core_h == ec_v.alt_core_h) gr.cbg_c = ec_v.alt_bonus;
                    }
                    if (rc.verb_pattern == 1) {
                        has_verb_g = true;
                        if (rc.ext_flags & EF_PASSIVE) has_pass_v = true;
                        else has_act_v = true;
                    } else if (t.si.particle == PART_NONE) {
                        has_noun_g = true;
                    }
                    if (t.si.particle != PART_NONE) {
                        if (diac_has_shadda(cm_g.ce)) has_part_sh = true;
                        else has_part_nosh = true;
                    }
                    bool g_sh = false, g_suk = false;
                    for (uint8_t p = st; p < rc.case_pos; ++p) {
                        if (p == lam_skip) continue;
                        if (diac_has_shadda(rc.diac[p])) g_sh = true;
                        if (rc.diac[p] == DIAC_SUKUN) g_suk = true;
                    }
                    if (g_sh) has_sh_core = true; else has_nosh_core = true;
                    if (g_suk) has_suk_core = true; else has_nosuk_core = true;

                    const uint8_t cr = cm_g.role;
                    gr.cr = cr;
                    const uint8_t ce = cm_g.ce;
                    int32_t tr_v = 0, bin_v = 0;
                    if (pt && pc_p1) {
                        uint8_t pr = c2_role[static_cast<size_t>(im1)];
                        uint8_t pe = pc_p1->diac[pc_p1->case_pos];
                        tr_v += trans_score(pr, pe, cr, ce);
                        bin_v += pbin_l[gr.rep_k];
                    } else {
                        tr_v += trans_score(0, DIAC_NONE, cr, ce);
                    }
                    if (nx && nc_p1) {
                        uint8_t nr = c2_role[static_cast<size_t>(ip1)];
                        uint8_t ne = nc_p1->diac[nc_p1->case_pos];
                        tr_v += trans_score(cr, ce, nr, ne);
                        bin_v += pbin_r[gr.rep_k * nx_ccnt + nx_ch];
                    }
                    gr.tr_sc = tr_v;
                    gr.bin_sc = bin_v;
                }

                const int32_t contrast_mask =
                    ((has_act_v && has_pass_v) ? 1 : 0) |
                    ((has_verb_g && has_noun_g) ? 2 : 0) |
                    ((has_sh_core && has_nosh_core) ? 4 : 0) |
                    ((t.surf_flags & SF_MAN) ? 8 : 0) |
                    ((has_part_sh && has_part_nosh) ? 16 : 0) |
                    ((has_suk_core && has_nosuk_core) ? 32 : 0);

                const ValencyInfo val_cur = tok_val[i];
                const ValencyInfo val_m1 = (im1 >= 0) ? tok_val[static_cast<size_t>(im1)] : ValencyInfo{};
                const ValencyInfo val_p1 = (ip1 >= 0) ? tok_val[static_cast<size_t>(ip1)] : ValencyInfo{};

                int32_t clause_fl = 0;
                if (t.si.has_prep || (pt && (pt->si.particle == PART_GEN || pt->si.particle == PART_IDAFA_NOUN))) clause_fl |= 1;
                if (pt && (pt->si.particle == PART_QAD || pt->si.particle == PART_JAZM || pt->si.particle == PART_NASB ||
                           pt->si.particle == PART_LA ||
                           ((pt->si.particle == PART_AN || pt->si.particle == PART_LI_AN) && pc_p1 && !diac_has_shadda(pc_p1->diac[pc_p1->case_pos])))) clause_fl |= 2;
                if (nx && (nx->si.particle == PART_GEN || nx->si.particle == PART_PREP_PRON || c2_gen[static_cast<size_t>(ip1)])) clause_fl |= 4;
                if (nx && !c2_verb[static_cast<size_t>(ip1)] && c2_fam[static_cast<size_t>(ip1)] == FAM_FATHA) clause_fl |= 8;
                if (nx && !c2_verb[static_cast<size_t>(ip1)] && c2_fam[static_cast<size_t>(ip1)] == FAM_DAMMA) clause_fl |= 16;
                if (nx && (nx->si.particle == PART_GEN || (nx->si.particle == PART_NONE && !c2_verb[static_cast<size_t>(ip1)]))) clause_fl |= 32;

                // Hoist all group-invariant features once per word
                const Cand& p1_rc = ctx.cands[t.cand_begin + grps[p1_g].rep_k];
                const int32_t fc64_base = (t.si.has_conj ? 1 : 0) | (t.si.has_prep ? 2 : 0) |
                                          ((ts[0] == S_WAW || ts[0] == S_FEH) ? 4 : 0) |
                                          ((ts[0] == S_LAM) ? 8 : 0) | ((ts[0] == S_BEH) ? 16 : 0);

                fc[13] = clamp_c2(n_grp);
                fc[15] = clamp_c2(contrast_mask);
                fc[27] = clamp_c2(t.si.particle);
                fc[28] = clamp_c2(((t.surf_flags & SF_MAN) ? 1 : 0) | ((t.surf_flags & SF_ALI) ? 2 : 0) |
                                  ((t.surf_flags & SF_AM) ? 4 : 0) | ((t.surf_flags & SF_THEMMA) ? 8 : 0) |
                                  ((t.surf_flags & SF_INNA_PRON) ? 16 : 0) | (t.si.has_prep ? 32 : 0));
                fc[29] = clamp_c2(t.n);
                fc[30] = clamp_c2(p2 ? p2->si.particle : -1);
                fc[31] = clamp_c2(pt ? pt->si.particle : -1);
                fc[32] = clamp_c2(nx ? nx->si.particle : -1);
                fc[33] = clamp_c2(nx2 ? nx2->si.particle : -1);
                fc[34] = clamp_c2((im2 >= 0) ? c2_pos[static_cast<size_t>(im2)] : -1);
                fc[35] = clamp_c2((im1 >= 0) ? c2_pos[static_cast<size_t>(im1)] : -1);
                fc[36] = clamp_c2((ip1 >= 0) ? c2_pos[static_cast<size_t>(ip1)] : -1);
                fc[37] = clamp_c2((ip2 >= 0) ? c2_pos[static_cast<size_t>(ip2)] : -1);
                fc[38] = clamp_c2((im1 >= 0) ? c2_role[static_cast<size_t>(im1)] : -1);
                fc[39] = clamp_c2((ip1 >= 0) ? c2_role[static_cast<size_t>(ip1)] : -1);
                fc[40] = clamp_c2((im1 >= 0) ? c2_fam[static_cast<size_t>(im1)] : -1);
                fc[41] = clamp_c2((ip1 >= 0) ? c2_fam[static_cast<size_t>(ip1)] : -1);
                fc[42] = clamp_c2((ip2 >= 0) ? c2_fam[static_cast<size_t>(ip2)] : -1);
                fc[43] = clamp_c2((im1 >= 0) ? c2_mc[static_cast<size_t>(im1)] : -1);
                fc[44] = clamp_c2((ip1 >= 0) ? c2_mc[static_cast<size_t>(ip1)] : -1);
                fc[45] = clamp_c2((im1 >= 0) ? static_cast<int32_t>(c2_al[im1] | (c2_gen[im1] << 1) | (c2_verb[im1] << 2) |
                                                                   (c2_tan[im1] << 3) | (c2_suf[im1] << 4) | (c2_pass[im1] << 5)) : -1);
                fc[46] = clamp_c2((ip1 >= 0) ? static_cast<int32_t>(c2_al[ip1] | (c2_gen[ip1] << 1) | (c2_verb[ip1] << 2) |
                                                                   (c2_tan[ip1] << 3) | (c2_suf[ip1] << 4) | (c2_pass[ip1] << 5)) : -1);
                fc[47] = clamp_c2((ip2 >= 0) ? static_cast<int32_t>(c2_al[ip2] | (c2_gen[ip2] << 1) | (c2_verb[ip2] << 2) |
                                                                   (c2_tan[ip2] << 3) | (c2_suf[ip2] << 4) | (c2_pass[ip2] << 5)) : -1);
                fc[48] = clamp_c2(ts[st]);
                fc[49] = clamp_c2(ts[t.n - 1]);
                fc[50] = clamp_c2(pt ? ctx.syms[pt->off + std::min<size_t>(pt->si.al_len, pt->n - 1)] : -1);
                fc[51] = clamp_c2(nx ? ctx.syms[nx->off + std::min<size_t>(nx->si.al_len, nx->n - 1)] : -1);
                fc[52] = clamp_c2(val_cur.v_trans + 4 * val_cur.word_syn_type + 16 * (val_cur.verb_prob >= 8 ? 1 : 0));
                fc[53] = clamp_c2(val_cur.verb_prob);
                fc[54] = clamp_c2(pt ? (val_m1.v_trans + 4 * val_m1.next_case_pref + 16 * val_m1.word_syn_type) : -1);
                fc[55] = clamp_c2(nx ? (val_p1.word_syn_type + 4 * val_p1.is_gram_fem +
                                        8 * (val_p1.verb_prob >= 8 ? 1 : 0) + 16 * val_p1.v_trans) : -1);
                fc[56] = clamp_c2(t.bkind_mask);
                fc[57] = clamp_c2((i + 1 < W) ? ctx.toks[i + 1].bkind_mask : static_cast<uint8_t>(BK_HARD));
                fc[58] = clamp_c2(clause_fl);
                fc[59] = clamp_c2(prev_vdist[i]);
                fc[60] = clamp_c2(next_vdist[i]);
                fc[63] = clamp_c2(t.si.al_len + 4 * t.si.verb_like + 16 * (t.has_noun_lex ? 1 : 0) + 32 * (t.is_adj_like ? 1 : 0));
                fc[65] = clamp_c2((st + 1 < t.n) ? ts[st + 1] : -1);
                fc[66] = clamp_c2((im2 >= 0) ? static_cast<int32_t>(c2_al[im2] | (c2_gen[im2] << 1) | (c2_verb[im2] << 2) |
                                                                    (c2_tan[im2] << 3) | (c2_suf[im2] << 4) | (c2_pass[im2] << 5)) : -1);
                fc[67] = clamp_c2((im2 >= 0) ? c2_fam[static_cast<size_t>(im2)] : -1);
                fc[68] = clamp_c2(nx ? (nx->top_verb + 2 * nx->has_noun_lex + 4 * nx->si.verb_like +
                                        16 * (nx->si.al_len > 0 ? 1 : 0) + 32 * (nx->si.has_prep ? 1 : 0)) : -1);
                fc[69] = clamp_c2(pt ? (pt->top_verb + 2 * pt->has_noun_lex + 4 * pt->si.verb_like +
                                        16 * (pt->si.al_len > 0 ? 1 : 0) + 32 * (pt->si.has_prep ? 1 : 0)) : -1);
                fc[70] = clamp_c2(p1_rc.pos_tag + 16 * p1_rc.verb_pattern + 48 * ((p1_rc.ext_flags & EF_PASSIVE) ? 1 : 0));
                fc[71] = clamp_c2(p1_rc.diac[st] + 16 * ((st + 1 < t.n) ? diac_family(p1_rc.diac[st + 1]) : 0));
                fc[76] = clamp_c2((im1 < 0 ? 1 : 0) | (ip1 < 0 ? 2 : 0) | (t.boundary_before ? 4 : 0) |
                                  ((i + 1 < W && ctx.toks[i + 1].boundary_before) ? 8 : 0));
                fc[78] = clamp_c2((t.lex_top_end != 255) ? static_cast<int32_t>(t.lex_top_end) : -1);
                fc[79] = clamp_c2((pt && pt->si.particle == PART_CONJ ? 1 : 0) |
                                  (pt && pt->si.has_prep ? 2 : 0) |
                                  (pt && pt->top_verb == 1 ? 4 : 0) |
                                  (nx && nx->top_verb == 1 ? 8 : 0) |
                                  (nx && nx->si.al_len > 0 ? 16 : 0) |
                                  (t.is_fem_at ? 32 : 0));

                if (use_fast_c2) {
                    for (size_t fi = 13; fi < c2_fc_lim; ++fi) fc_base[fi] = static_cast<int8_t>(fc[fi]);
                    const uint32_t vec_off = static_cast<uint32_t>(sc_buf.c2_feat_buf.size());
                    sc_buf.c2_feat_buf.resize(vec_off + static_cast<size_t>(n_grp));
                    Scratch::Core2Slice sl{};
                    sl.tok_i = static_cast<uint32_t>(i);
                    sl.vec_off = vec_off;
                    sl.n_grp = n_grp;
                    sl.p1_g = p1_g;
                    for (int g = 0; g < n_grp; ++g) {
                        sl.rep_k[g] = grps[g].rep_k;
                        sl.core_h[g] = grps[g].core_h;
                    }
                    sc_buf.c2_slices.push_back(sl);

                    for (int g = 0; g < n_grp; ++g) {
                        const CoreGrp& gr = grps[g];
                        const Cand& rc = ctx.cands[t.cand_begin + gr.rep_k];

                        int32_t o_lex = NEG_INF, o_em = NEG_INF, o_mo = NEG_INF;
                        int32_t o_cl = NEG_INF, o_cr = NEG_INF, o_cc = NEG_INF;
                        int32_t o_tr = NEG_INF, o_bn = NEG_INF;
                        int32_t o_ne = NEG_INF, o_nf = NEG_INF, o_nt = NEG_INF;
                        int rank_lex = 0, rank_em = 0;
                        for (int o = 0; o < n_grp; ++o) {
                            if (o == g) continue;
                            if (grps[o].lex_max > gr.lex_max) ++rank_lex;
                            if (grps[o].emis_max > gr.emis_max) ++rank_em;
                            if (grps[o].lex_max > o_lex) o_lex = grps[o].lex_max;
                            if (grps[o].emis_max > o_em) o_em = grps[o].emis_max;
                            if (grps[o].morph_sc > o_mo) o_mo = grps[o].morph_sc;
                            if (grps[o].cbg_l > o_cl) o_cl = grps[o].cbg_l;
                            if (grps[o].cbg_r > o_cr) o_cr = grps[o].cbg_r;
                            if (grps[o].cbg_c > o_cc) o_cc = grps[o].cbg_c;
                            if (grps[o].tr_sc > o_tr) o_tr = grps[o].tr_sc;
                            if (grps[o].bin_sc > o_bn) o_bn = grps[o].bin_sc;
                            if (grps[o].n_end_max > o_ne) o_ne = grps[o].n_end_max;
                            if (grps[o].n_fam_max > o_nf) o_nf = grps[o].n_fam_max;
                            if (grps[o].n_tan_max > o_nt) o_nt = grps[o].n_tan_max;
                        }

                        uint8_t sh_cnt = 0, suk_cnt = 0;
                        for (uint8_t p = st; p < rc.case_pos; ++p) {
                            if (p == lam_skip) continue;
                            if (diac_has_shadda(rc.diac[p]) && sh_cnt < 3) ++sh_cnt;
                            if (rc.diac[p] == DIAC_SUKUN && suk_cnt < 3) ++suk_cnt;
                        }

                        int8_t* fg = sc_buf.c2_feat_buf[vec_off + static_cast<size_t>(g)].data();
                        std::memcpy(fg, fc_base, 128);
                        fg[0] = clamp_c2_i8(gr.is_p1);
                        fg[1] = clamp_c2_i8(rank_lex);
                        fg[2] = clamp_c2_i8(rank_em);
                        fg[3] = clamp_c2_i8(60 + (gr.lex_max - o_lex) / 80);
                        fg[4] = clamp_c2_i8(60 + (gr.emis_max - o_em) / 100);
                        fg[5] = clamp_c2_i8(60 + (gr.morph_sc - o_mo) / 60);
                        fg[6] = clamp_c2_i8(60 + (gr.cbg_l - o_cl) / 50);
                        fg[7] = clamp_c2_i8(60 + (gr.cbg_r - o_cr) / 50);
                        fg[8] = clamp_c2_i8(60 + (gr.cbg_c - o_cc) / 50);
                        fg[9] = clamp_c2_i8(60 + gr.cbg_l / 50);
                        fg[10] = clamp_c2_i8(60 + gr.cbg_r / 50);
                        fg[11] = clamp_c2_i8(60 + gr.cbg_c / 50);
                        fg[12] = clamp_c2_i8(gr.count);
                        fg[14] = clamp_c2_i8((gr.has_tan ? 1 : 0) | (gr.has_kasra ? 2 : 0) | (gr.has_non_exp ? 4 : 0) |
                                             ((rc.flags & CF_SUFFIX) ? 8 : 0) | ((rc.flags & CF_HAS_AL) ? 16 : 0) | ((rc.flags & CF_GEN) ? 32 : 0));
                        fg[16] = clamp_c2_i8(rc.morph_class);
                        fg[17] = clamp_c2_i8(rc.pos_tag);
                        fg[18] = clamp_c2_i8(rc.verb_pattern);
                        fg[19] = (rc.ext_flags & EF_PASSIVE) ? 1 : 0;
                        fg[20] = clamp_c2_i8(gr.cr);
                        fg[21] = clamp_c2_i8(rc.diac[st]);
                        fg[22] = clamp_c2_i8((st + 1 < t.n) ? rc.diac[st + 1] : -1);
                        fg[23] = clamp_c2_i8((st + 2 < rc.case_pos) ? rc.diac[st + 2] : -1);
                        fg[24] = clamp_c2_i8((rc.case_pos >= 1) ? rc.diac[rc.case_pos - 1] : -1);
                        fg[25] = clamp_c2_i8(rc.diac[rc.case_pos]);
                        fg[26] = clamp_c2_i8(static_cast<int32_t>(sh_cnt * 8 + suk_cnt));
                        fg[61] = clamp_c2_i8(60 + (gr.tr_sc - o_tr) / 80);
                        fg[62] = clamp_c2_i8(60 + (gr.bin_sc - o_bn) / 100);
                        if (c2_fc_lim > 64) {
                            fg[64] = static_cast<int8_t>(fc64_base | ((rc.flags & CF_EXPANDED) ? 32 : 0));
                        }
                        if (c2_fc_lim > 72) {
                            fg[72] = clamp_c2_i8(el_i ? (60 + (gr.n_end_max - o_ne) / 10) : -1);
                            fg[73] = clamp_c2_i8(fl_i ? (60 + (gr.n_fam_max - o_nf) / 10) : -1);
                            fg[74] = clamp_c2_i8(tl_i ? (60 + (gr.n_tan_max - o_nt) / 10) : -1);
                            fg[75] = clamp_c2_i8(el_i ? gr.has_n_best_end : -1);
                            fg[77] = clamp_c2_i8((rc.diac[st] == DIAC_DAMMA ? 1 : 0) |
                                                 (rc.diac[st] == DIAC_KASRA ? 2 : 0) |
                                                 (rc.diac[st] == DIAC_FATHA ? 4 : 0) |
                                                 ((st + 1 < rc.case_pos && rc.diac[st + 1] == DIAC_SUKUN) ? 8 : 0) |
                                                 ((rc.case_pos >= 1 && rc.diac[rc.case_pos - 1] == DIAC_KASRA) ? 16 : 0) |
                                                 ((rc.case_pos >= 1 && rc.diac[rc.case_pos - 1] == DIAC_FATHA) ? 32 : 0));
                        }
                    }
                    continue;
                }

                int32_t best_logit = NEG_INF;
                int32_t p1_logit = 0;
                int best_g = p1_g;

                for (int g = 0; g < n_grp; ++g) {
                    const CoreGrp& gr = grps[g];
                    const Cand& rc = ctx.cands[t.cand_begin + gr.rep_k];

                    int32_t o_lex = NEG_INF, o_em = NEG_INF, o_mo = NEG_INF;
                    int32_t o_cl = NEG_INF, o_cr = NEG_INF, o_cc = NEG_INF;
                    int32_t o_tr = NEG_INF, o_bn = NEG_INF;
                    int32_t o_ne = NEG_INF, o_nf = NEG_INF, o_nt = NEG_INF;
                    int rank_lex = 0, rank_em = 0;
                    for (int o = 0; o < n_grp; ++o) {
                        if (o == g) continue;
                        if (grps[o].lex_max > gr.lex_max) ++rank_lex;
                        if (grps[o].emis_max > gr.emis_max) ++rank_em;
                        if (grps[o].lex_max > o_lex) o_lex = grps[o].lex_max;
                        if (grps[o].emis_max > o_em) o_em = grps[o].emis_max;
                        if (grps[o].morph_sc > o_mo) o_mo = grps[o].morph_sc;
                        if (grps[o].cbg_l > o_cl) o_cl = grps[o].cbg_l;
                        if (grps[o].cbg_r > o_cr) o_cr = grps[o].cbg_r;
                        if (grps[o].cbg_c > o_cc) o_cc = grps[o].cbg_c;
                        if (grps[o].tr_sc > o_tr) o_tr = grps[o].tr_sc;
                        if (grps[o].bin_sc > o_bn) o_bn = grps[o].bin_sc;
                        if (grps[o].n_end_max > o_ne) o_ne = grps[o].n_end_max;
                        if (grps[o].n_fam_max > o_nf) o_nf = grps[o].n_fam_max;
                        if (grps[o].n_tan_max > o_nt) o_nt = grps[o].n_tan_max;
                    }

                    uint8_t sh_cnt = 0, suk_cnt = 0;
                    for (uint8_t p = st; p < rc.case_pos; ++p) {
                        if (p == lam_skip) continue;
                        if (diac_has_shadda(rc.diac[p]) && sh_cnt < 3) ++sh_cnt;
                        if (rc.diac[p] == DIAC_SUKUN && suk_cnt < 3) ++suk_cnt;
                    }

                    fc[0] = clamp_c2(gr.is_p1);
                    fc[1] = clamp_c2(rank_lex);
                    fc[2] = clamp_c2(rank_em);
                    fc[3] = clamp_c2(60 + (gr.lex_max - o_lex) / 80);
                    fc[4] = clamp_c2(60 + (gr.emis_max - o_em) / 100);
                    fc[5] = clamp_c2(60 + (gr.morph_sc - o_mo) / 60);
                    fc[6] = clamp_c2(60 + (gr.cbg_l - o_cl) / 50);
                    fc[7] = clamp_c2(60 + (gr.cbg_r - o_cr) / 50);
                    fc[8] = clamp_c2(60 + (gr.cbg_c - o_cc) / 50);
                    fc[9] = clamp_c2(60 + gr.cbg_l / 50);
                    fc[10] = clamp_c2(60 + gr.cbg_r / 50);
                    fc[11] = clamp_c2(60 + gr.cbg_c / 50);
                    fc[12] = clamp_c2(gr.count);
                    fc[14] = clamp_c2((gr.has_tan ? 1 : 0) | (gr.has_kasra ? 2 : 0) | (gr.has_non_exp ? 4 : 0) |
                                      ((rc.flags & CF_SUFFIX) ? 8 : 0) | ((rc.flags & CF_HAS_AL) ? 16 : 0) | ((rc.flags & CF_GEN) ? 32 : 0));
                    fc[16] = clamp_c2(rc.morph_class);
                    fc[17] = clamp_c2(rc.pos_tag);
                    fc[18] = clamp_c2(rc.verb_pattern);
                    fc[19] = (rc.ext_flags & EF_PASSIVE) ? 1 : 0;
                    fc[20] = clamp_c2(gr.cr);
                    fc[21] = clamp_c2(rc.diac[st]);
                    fc[22] = clamp_c2((st + 1 < t.n) ? rc.diac[st + 1] : -1);
                    fc[23] = clamp_c2((st + 2 < rc.case_pos) ? rc.diac[st + 2] : -1);
                    fc[24] = clamp_c2((rc.case_pos >= 1) ? rc.diac[rc.case_pos - 1] : -1);
                    fc[25] = clamp_c2(rc.diac[rc.case_pos]);
                    fc[26] = clamp_c2(static_cast<int32_t>(sh_cnt * 8 + suk_cnt));
                    fc[61] = clamp_c2(60 + (gr.tr_sc - o_tr) / 80);
                    fc[62] = clamp_c2(60 + (gr.bin_sc - o_bn) / 100);
                    fc[64] = clamp_c2(fc64_base | ((rc.flags & CF_EXPANDED) ? 32 : 0));
                    fc[72] = clamp_c2(el_i ? (60 + (gr.n_end_max - o_ne) / 10) : -1);
                    fc[73] = clamp_c2(fl_i ? (60 + (gr.n_fam_max - o_nf) / 10) : -1);
                    fc[74] = clamp_c2(tl_i ? (60 + (gr.n_tan_max - o_nt) / 10) : -1);
                    fc[75] = clamp_c2(el_i ? gr.has_n_best_end : -1);
                    fc[77] = clamp_c2((rc.diac[st] == DIAC_DAMMA ? 1 : 0) |
                                      (rc.diac[st] == DIAC_KASRA ? 2 : 0) |
                                      (rc.diac[st] == DIAC_FATHA ? 4 : 0) |
                                      ((st + 1 < rc.case_pos && rc.diac[st + 1] == DIAC_SUKUN) ? 8 : 0) |
                                      ((rc.case_pos >= 1 && rc.diac[rc.case_pos - 1] == DIAC_KASRA) ? 16 : 0) |
                                      ((rc.case_pos >= 1 && rc.diac[rc.case_pos - 1] == DIAC_FATHA) ? 32 : 0));

                    if (core2_dump_out != nullptr) {
                        Core2DumpSample smp;
                        std::memcpy(smp.feat, fc, sizeof(fc));
                        smp.label = gold_match[g];
                        smp.p1_correct = p1_correct;
                        smp.group_id = cur_gid;
                        core2_dump_out->push_back(smp);
                    } else {
                        int32_t sc2[4] = {0, 0, 0, 0};
                        core2_trees_.predict_scores(fc, core2_feat_count_, sc2);
                        int32_t lg = sc2[1] - sc2[0];
                        if (g == p1_g) p1_logit = lg;
                        if (lg > best_logit) { best_logit = lg; best_g = g; }
                    }
                }

                if (core2_dump_out == nullptr && best_g != p1_g) {
                    int32_t b_sc = static_cast<int32_t>((static_cast<int64_t>(best_logit) * v8_weights_[V8W_CORE2]) / 1000);
                    int32_t p_sc = static_cast<int32_t>((static_cast<int64_t>(p1_logit) * v8_weights_[V8W_CORE2]) / 1000);
                    if (b_sc > p_sc + v8_weights_[V8W_CORE2_MARGIN]) {
                        new_chosen[i] = pick_best_k_in_group(i, grps[best_g].core_h, grps[best_g].rep_k);
                        core2_locked[i] = 1;
                        has_core2_locks = true;
                    }
                }
            }

            // Sentence-batched L1-tiled Core2 evaluation with exact branch-and-bound pruning
            if (use_fast_c2 && !sc_buf.c2_feat_buf.empty()) {
                const size_t n_vecs = sc_buf.c2_feat_buf.size();
                sc_buf.c2_logits.resize(n_vecs);
                const int32_t w_c2 = v8_weights_[V8W_CORE2];
                const int32_t c2_mar = v8_weights_[V8W_CORE2_MARGIN];
                core2_trees_.predict_batch_binary_pruned_i8(
                    sc_buf.c2_feat_buf.data(), n_vecs,
                    sc_buf.c2_slices.data(), sc_buf.c2_slices.size(),
                    w_c2, c2_mar,
                    sc_buf.c2_active_vecs, sc_buf.c2_active_slices,
                    sc_buf.c2_logits.data());
                for (const auto& sl : sc_buf.c2_slices) {
                    int32_t best_logit = NEG_INF;
                    int32_t p1_logit = 0;
                    int best_g = sl.p1_g;
                    for (int g = 0; g < sl.n_grp; ++g) {
                        const int32_t lg = sc_buf.c2_logits[sl.vec_off + static_cast<size_t>(g)];
                        if (g == sl.p1_g) p1_logit = lg;
                        if (lg > best_logit) { best_logit = lg; best_g = g; }
                    }
                    if (best_g != sl.p1_g) {
                        const int32_t b_sc = static_cast<int32_t>((static_cast<int64_t>(best_logit) * w_c2) / 1000);
                        const int32_t p_sc = static_cast<int32_t>((static_cast<int64_t>(p1_logit) * w_c2) / 1000);
                        if (b_sc > p_sc + c2_mar) {
                            new_chosen[sl.tok_i] = pick_best_k_in_group(sl.tok_i, sl.core_h[best_g], sl.rep_k[best_g]);
                            core2_locked[sl.tok_i] = 1;
                            has_core2_locks = true;
                        }
                    }
                }
            }

            if (core2_dump_out != nullptr) {
                return std::string();
            }
            if (has_core2_locks) {
                chosen.swap(new_chosen);
            }
        }

        // Pass-2 Stacked Bidirectional Irab2-GBDT + Case-Lattice Beam Re-Decoding (V7)
        if (dump_out != nullptr || (is_v7_ && irab2_trees_.valid() && v7_weights_[V7W_IRAB2] != 0)) {
            auto clamp_f = [](int32_t v) -> int32_t { return v < -8 ? -8 : (v > 119 ? 119 : v); };
            auto is_bare_fem_p2 = [&](size_t idx, const Token& tk) -> bool {
                const uint8_t* ws = &ctx.syms[tk.off];
                if (tk.n > tk.si.al_len + 1 && ws[tk.n - 1] == S_TEH_MARBUTA) return true;
                if (tok_val[idx].is_gram_fem) return true;
                return false;
            };

            // Precompute Pass-1 per-token decoded properties (using Scratch buffers — zero per-sentence heap allocs)
            auto& p1_end = sc_buf.p1_end;
            auto& p1_fam = sc_buf.p1_fam;
            auto& p1_tan = sc_buf.p1_tan;
            auto& p1_pos = sc_buf.p1_pos;
            auto& p1_role = sc_buf.p1_role;
            auto& p1_al = sc_buf.p1_al;
            auto& p1_gen = sc_buf.p1_gen;
            auto& p1_suf = sc_buf.p1_suf;
            auto& p1_verb = sc_buf.p1_verb;
            auto& p1_fem = sc_buf.p1_fem;
            auto& p1_st3 = sc_buf.p1_st3;
            auto& p1_multi = sc_buf.p1_multi;
            p1_end.assign(W, 0); p1_fam.assign(W, 0); p1_tan.assign(W, 0); p1_pos.assign(W, 0); p1_role.assign(W, 0);
            p1_al.assign(W, 0); p1_gen.assign(W, 0); p1_suf.assign(W, 0); p1_verb.assign(W, 0); p1_fem.assign(W, 0);
            p1_st3.assign(W, 0);
            if (dump_out != nullptr) p1_multi.assign(W, 0);

            for (size_t i = 0; i < W; ++i) {
                const Token& tk = ctx.toks[i];
                if (tk.cand_count == 0) continue;
                const Cand& c = ctx.cands[tk.cand_begin + chosen[i]];
                const CandMeta& cm = cmeta[dp_off[i] + chosen[i]];
                p1_end[i] = cm.ce;
                p1_fam[i] = cm.eff_fam;
                p1_tan[i] = (cm.bits & CMB_TAN) ? 1 : 0;
                p1_pos[i] = c.pos_tag;
                p1_role[i] = cm.role;
                p1_al[i] = (cm.bits & CMB_HAS_AL) ? 1 : 0;
                p1_gen[i] = (cm.bits & CMB_HAS_GEN) ? 1 : 0;
                p1_suf[i] = (c.flags & CF_SUFFIX) ? 1 : 0;
                p1_verb[i] = (c.verb_pattern == 1) ? 1 : 0;
                p1_fem[i] = is_bare_fem_p2(i, tk) ? 1 : 0;
                p1_st3[i] = cm.st3;
                if (dump_out != nullptr) {
                    uint8_t fe = cmeta[dp_off[i]].ce;
                    for (uint32_t k = 1; k < tk.cand_count; ++k) {
                        if (cmeta[dp_off[i] + k].ce != fe) { p1_multi[i] = 1; break; }
                    }
                }
            }

            // When inferring (dump_out == nullptr), precompute exact Pass-2 beam opts once and find
            // exact active ending bitmask per token. Tokens with <2 distinct endings in opts cannot be
            // affected by Irab2 (100% bit-exact), and active tokens only evaluate classes present in opts!
            bool any_irab2 = (dump_out != nullptr);
            const uint32_t irab2_nc = irab2_trees_.num_classes();
            if (dump_out == nullptr) {
                sc_buf.irab2_bonus.assign(dp_off[W], 0);
                sc_buf.p2_opts.clear();
                sc_buf.p2_opts_off.resize(W + 1);
                sc_buf.p2_ce_mask.assign(W, 0);
                sc_buf.p2_need_irab2.assign(W, 0);
                for (size_t i = 0; i < W; ++i) {
                    sc_buf.p2_opts_off[i] = static_cast<uint32_t>(sc_buf.p2_opts.size());
                    const Token& t = ctx.toks[i];
                    if (t.cand_count == 0) continue;
                    const bool is_locked = has_core2_locks && (core2_locked[i] != 0);
                    const size_t os = sc_buf.p2_opts.size();
                    build_beam_opts(i, t, chosen[i], is_locked, sc_buf.p2_opts);
                    const size_t oe = sc_buf.p2_opts.size();
                    if (oe - os >= 2) {
                        uint32_t all_ce = 0;
                        uint32_t valid_ce = 0;
                        const uint32_t dbase = dp_off[i];
                        for (size_t oi = os; oi < oe; ++oi) {
                            const uint8_t ce = cmeta[dbase + sc_buf.p2_opts[oi]].ce;
                            if (ce < 32u) all_ce |= (1u << ce);
                            if (ce < irab2_nc) valid_ce |= (1u << ce);
                        }
                        if ((all_ce & (all_ce - 1u)) != 0u) {
                            const Cand& bc = ctx.cands[t.cand_begin + chosen[i]];
                            const int ip1_chk = (i + 1 < W && !ctx.toks[i + 1].boundary_before) ? static_cast<int>(i + 1) : -1;
                            const bool skip_iltiqa =
                                ((t.surf_flags & SF_MAN) != 0) || ((t.xflags & XF_IS_AN_PREP) != 0) ||
                                (ip1_chk >= 0 && p1_al[ip1_chk] && (bc.ext_flags & (EF_PLURAL_MEEM | EF_PAST_TEH_FEM)));
                            if (!skip_iltiqa) {
                                sc_buf.p2_ce_mask[i] = valid_ce;
                                sc_buf.p2_need_irab2[i] = 1;
                                any_irab2 = true;
                            }
                        }
                    }
                }
                sc_buf.p2_opts_off[W] = static_cast<uint32_t>(sc_buf.p2_opts.size());
            }

            if (any_irab2) {
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
                auto& f_depth = sc_buf.f_depth;
                auto& f_gov = sc_buf.f_gov;
                auto& f_outer = sc_buf.f_outer;
                auto& f_ppfl = sc_buf.f_ppfl;
                auto& f_idafa = sc_buf.f_idafa;
                auto& f_hfam = sc_buf.f_hfam;
                auto& f_mfam = sc_buf.f_mfam;
                auto& f_pfam = sc_buf.f_pfam;
                auto& f_npm = sc_buf.f_npm;
                auto& f_gvt = sc_buf.f_gvt;
                auto& f_pic = sc_buf.f_pic;
                auto& f_dgov = sc_buf.f_dgov;
                auto& f_gvo = sc_buf.f_gvo;
                f_depth.assign(W, 0); f_gov.assign(W, 0); f_outer.assign(W, 0); f_ppfl.assign(W, 0);
                f_idafa.assign(W, 0); f_hfam.assign(W, 0); f_mfam.assign(W, 0); f_pfam.assign(W, 0);
                f_npm.assign(W, 0); f_gvt.assign(W, 0); f_pic.assign(W, 0);
                f_dgov.assign(W, -1); f_gvo.assign(W, 0);

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
                    const CandMeta& fin_cm = cmeta[dp_off[i] + chosen[i]];
                    const bool is_kana = (t.si.particle == PART_KANA) || ((t.xflags & XF_IS_KANA_VERB) != 0);
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
                        else if (fin.pos_tag == POS_VERB_PAST_SUBJ || fin.pos_tag == POS_VERB_IMPERF_12 || (fin_cm.bits & CMB_HAS_VOBJ)) fr.gov = 2;
                        else fr.gov = 1;
                        fr.gov_idx = static_cast<int>(i); fr.pp_depth = 0; fr.fronted_pp = 0; fr.ism_def = 0;
                        fr.ism_mod = 0; fr.prev_fam = 0; fr.head_fam = 0; fr.mudaf_fam = 0; fr.idafa_depth = 0;
                        const ValencyInfo& vi = tok_val[i];
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
                    } else if (t.si.particle == PART_DEM && ((t.xflags & XF_IS_DEM_DET) != 0) &&
                               i + 1 < W && !ctx.toks[i + 1].boundary_before && p1_al[i + 1] && !p1_gen[i + 1]) {
                        // Propagate ClauseFrame, pp_depth, idafa_depth across هذا/هذه/ذلك/تلك to following AL-noun
                    } else if (!p1_verb[i] && (t.si.particle == PART_NONE || t.si.particle == PART_IDAFA_NOUN || t.si.particle == PART_NUM_IDAFA || (fin_cm.bits & CMB_ALI_PROP))) {
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
                auto& r_dverb = sc_buf.r_dverb;
                auto& r_dprep = sc_buf.r_dprep;
                auto& r_dend = sc_buf.r_dend;
                auto& r_flags = sc_buf.r_flags;
                r_dverb.assign(W, 31); r_dprep.assign(W, 31); r_dend.assign(W, 0); r_flags.assign(W, 0);
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

                const bool use_fast_i2 = (dump_out == nullptr) && irab2_trees_.has_fast_v5();
                const size_t i2_fc_lim = std::min<size_t>(irab2_feat_count_, IRAB2_NUM_FEATURES);
                if (use_fast_i2) {
                    sc_buf.i2_feat_buf.clear();
                    sc_buf.i2_tok_idx.clear();
                    for (uint32_t c = 0; c < irab2_nc; ++c) sc_buf.i2_cls_words[c].clear();
                }

                int32_t f2[IRAB2_NUM_FEATURES];
                const int32_t w_i2 = v7_weights_[V7W_IRAB2];
                const int32_t p2_i1_delta = v7_weights_[V7W_P2_IRAB1_SCALE] - 1000;
                const uint32_t irab1_nc = irab_trees_.num_classes();
                const uint32_t irab1_all_mask = (irab1_nc > 0 && irab1_nc <= NUM_DIACRITICS) ? ((1u << irab1_nc) - 1u) : 0u;

                // Sentence-batched class-outer evaluation of remaining lazy_irab1 classes!
                if (lazy_irab1 && irab1_all_mask != 0u) {
                    sc_buf.i1_tok_idx.clear();
                    sc_buf.i1_ce_mask.clear();
                    for (uint32_t c = 0; c < irab1_nc; ++c) sc_buf.i1_cls_words[c].clear();
                    for (size_t i = 0; i < W; ++i) {
                        if (ctx.toks[i].cand_count == 0) continue;
                        if (dump_out == nullptr && !sc_buf.p2_need_irab2[i]) continue;
                        if (sc_buf.p1_irab1_mask[i] == 0u) continue;
                        const uint32_t rem_mask = irab1_all_mask & ~sc_buf.p1_irab1_mask[i];
                        if (rem_mask == 0u) continue;
                        sc_buf.i1_tok_idx.push_back(static_cast<uint32_t>(i));
                        sc_buf.i1_ce_mask.push_back(rem_mask);
                        uint32_t m = rem_mask;
                        while (m) {
                            const uint32_t c_id = portable_ctz32(m);
                            m &= (m - 1u);
                            sc_buf.i1_cls_words[c_id].push_back(static_cast<uint32_t>(i));
                        }
                    }
                    if (!sc_buf.i1_tok_idx.empty()) {
                        if (sc_buf.i1_scores_flat.size() < W * NUM_DIACRITICS) {
                            sc_buf.i1_scores_flat.resize(W * NUM_DIACRITICS);
                        }
                        for (uint32_t c = 0; c < irab1_nc; ++c) {
                            if (!sc_buf.i1_cls_words[c].empty()) {
                                irab_trees_.eval_class_batch_i8(
                                    c, sc_buf.p1_irab1_feats.data(),
                                    sc_buf.i1_cls_words[c].data(), sc_buf.i1_cls_words[c].size(),
                                    sc_buf.i1_scores_flat.data(), NUM_DIACRITICS);
                            }
                        }
                        const size_t n_rem = sc_buf.i1_tok_idx.size();
                        for (size_t j = 0; j < n_rem; ++j) {
                            const size_t i = sc_buf.i1_tok_idx[j];
                            uint32_t m = sc_buf.i1_ce_mask[j];
                            const int32_t* sc_rem = &sc_buf.i1_scores_flat[i * NUM_DIACRITICS];
                            int32_t b_sc = sc_buf.p1_irab1_best_sc[i];
                            int8_t b_id = sc_buf.p1_irab1_best[i];
                            while (m) {
                                const uint32_t c_id = portable_ctz32(m);
                                m &= (m - 1u);
                                const int32_t v = sc_rem[c_id];
                                if (v > b_sc || (v == b_sc && static_cast<int8_t>(c_id) < b_id)) {
                                    b_sc = v;
                                    b_id = static_cast<int8_t>(c_id);
                                }
                            }
                            sc_buf.p1_irab1_best[i] = b_id;
                            sc_buf.p1_irab1_best_sc[i] = b_sc;
                        }
                    }
                }

                for (size_t i = 0; i < W; ++i) {
                    const Token& t = ctx.toks[i];
                    if (t.cand_count == 0) continue;
                    if (dump_out == nullptr && !sc_buf.p2_need_irab2[i]) continue;
                    const Cand& bc = ctx.cands[t.cand_begin + chosen[i]];
                    const CandMeta& bc_cm = cmeta[dp_off[i] + chosen[i]];
                    const uint8_t* ts = &ctx.syms[t.off];

                    // Compute Pass-1 margin & runner-up ending + case availability bitmask
                    const uint32_t dbase = dp_off[i];
                    int32_t best_em = bc.emis + dp_bg[dbase + chosen[i]];
                    int32_t sec_em = NEG_INF;
                    int8_t sec_end = -1;
                    uint8_t avail_mask = 0;
                    const size_t lskip = (t.si.al_len >= 2) ? static_cast<size_t>(t.si.al_lam_pos) : static_cast<size_t>(-1);
                    for (uint32_t k = 0; k < t.cand_count; ++k) {
                        const Cand& ak = ctx.cands[t.cand_begin + k];
                        const CandMeta& am = cmeta[dbase + k];
                        const uint8_t ae = am.ce;
                        const uint8_t af = am.cf;
                        if (af == FAM_DAMMA) avail_mask |= 1;
                        else if (af == FAM_FATHA) avail_mask |= 2;
                        else if (af == FAM_KASRA) avail_mask |= 4;
                        else if (af == FAM_SUKUN) avail_mask |= 8;
                        if (am.bits & CMB_TAN) avail_mask |= 16;
                        if (ae == p1_end[i]) continue;
                        if (ak.case_pos != bc.case_pos) continue;
                        bool sc_ok = true;
                        for (size_t pp = 0; sc_ok && pp < bc.case_pos; ++pp) {
                            if (pp != lskip && ak.diac[pp] != bc.diac[pp]) sc_ok = false;
                        }
                        if (!sc_ok) continue;
                        int32_t em = ak.emis + dp_bg[dbase + k];
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
                                    ((bc_cm.bits & CMB_INVAR_LV) ? 32 : 0);
                    uint8_t wfl_p = 0;
                    if (im1 >= 0) {
                        const Token& pt_n = ctx.toks[im1];
                        wfl_p = (pt_n.is_fem_at ? 1 : 0) | (pt_n.is_adj_like ? 2 : 0) | (p1_fem[im1] ? 4 : 0) |
                                ((pt_n.xflags & XF_IS_TITLE) ? 8 : 0) |
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
                    const ValencyInfo vm2 = (im2 >= 0) ? tok_val[static_cast<size_t>(im2)] : ValencyInfo{};
                    const ValencyInfo vm1 = (im1 >= 0) ? tok_val[static_cast<size_t>(im1)] : ValencyInfo{};
                    const ValencyInfo vc0 = tok_val[i];
                    const ValencyInfo vp1 = (ip1 >= 0) ? tok_val[static_cast<size_t>(ip1)] : ValencyInfo{};
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
                        if (nx_n.si.particle == PART_GEN || p1_gen[ip1] || ((nx_n.xflags & XF_IS_INA_CLAUSE) != 0)) nr_fl |= 4;
                    }
                    if (((t.xflags & XF_IS_TIME_ADV) != 0) || vc0.word_syn_type == 3) nr_fl |= 8;
                    f2[93] = nr_fl;
                    f2[94] = (im1 >= 0) ? ctx.syms[ctx.toks[im1].off + ctx.toks[im1].n - 1] : -1;
                    f2[95] = (ip1 >= 0) ? ctx.syms[ctx.toks[ip1].off + std::min<size_t>(ctx.toks[ip1].si.al_len, ctx.toks[ip1].n - 1)] : -1;

                    if (dump_out != nullptr) {
                        for (size_t fi = 0; fi < IRAB2_NUM_FEATURES; ++fi) f2[fi] = clamp_f(f2[fi]);
                        const bool skip_iltiqa_irab2 =
                            ((t.surf_flags & SF_MAN) != 0) || ((t.xflags & XF_IS_AN_PREP) != 0) ||
                            (ip1 >= 0 && p1_al[ip1] && (bc.ext_flags & (EF_PLURAL_MEEM | EF_PAST_TEH_FEM)));
                        if (skip_iltiqa_irab2) continue;
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
                    } else if (use_fast_i2) {
                        const uint32_t ce_mask = sc_buf.p2_ce_mask[i];
                        const uint32_t wi = static_cast<uint32_t>(sc_buf.i2_feat_buf.size());
                        sc_buf.i2_feat_buf.resize(wi + 1u);
                        sc_buf.i2_tok_idx.push_back(static_cast<uint32_t>(i));
                        int8_t* dst = sc_buf.i2_feat_buf[wi].data();
                        for (size_t fi = 0; fi < i2_fc_lim; ++fi) {
                            dst[fi] = static_cast<int8_t>(clamp_f(f2[fi]));
                        }
                        if (i2_fc_lim < 128) std::memset(dst + i2_fc_lim, 0xFF, 128 - i2_fc_lim);
                        uint32_t m = ce_mask & ((1u << irab2_nc) - 1u);
                        while (m) {
                            const uint32_t c = portable_ctz32(m);
                            m &= (m - 1u);
                            sc_buf.i2_cls_words[c].push_back(wi);
                        }
                    } else {
                        int32_t sc2[NUM_DIACRITICS];
                        for (auto& v : sc2) v = 0;
                        const uint32_t ce_mask = sc_buf.p2_ce_mask[i];
                        if (ce_mask != 0u) {
                            for (size_t fi = 0; fi < IRAB2_NUM_FEATURES; ++fi) f2[fi] = clamp_f(f2[fi]);
                            irab2_trees_.predict_scores_masked(f2, irab2_feat_count_, ce_mask, sc2);
                        }
                        const uint32_t os = sc_buf.p2_opts_off[i];
                        const uint32_t oe = sc_buf.p2_opts_off[i + 1];
                        for (uint32_t oi = os; oi < oe; ++oi) {
                            const uint32_t k = sc_buf.p2_opts[oi];
                            const Cand& ck = ctx.cands[t.cand_begin + k];
                            const uint8_t ce = ck.diac[ck.case_pos];
                            if (ce < irab2_nc) {
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

                // Class-outer L1-tiled sentence-batched Irab2 evaluation!
                if (use_fast_i2 && !sc_buf.i2_tok_idx.empty()) {
                    const size_t n_act = sc_buf.i2_tok_idx.size();
                    sc_buf.i2_scores_flat.assign(n_act * NUM_DIACRITICS, 0);
                    for (uint32_t c = 0; c < irab2_nc; ++c) {
                        if (!sc_buf.i2_cls_words[c].empty()) {
                            irab2_trees_.eval_class_batch_i8(
                                c, sc_buf.i2_feat_buf.data(),
                                sc_buf.i2_cls_words[c].data(), sc_buf.i2_cls_words[c].size(),
                                sc_buf.i2_scores_flat.data(), NUM_DIACRITICS);
                        }
                    }
                    for (size_t wi = 0; wi < n_act; ++wi) {
                        const size_t i = sc_buf.i2_tok_idx[wi];
                        const Token& t = ctx.toks[i];
                        const int32_t* sc2 = &sc_buf.i2_scores_flat[wi * NUM_DIACRITICS];
                        const uint32_t os = sc_buf.p2_opts_off[i];
                        const uint32_t oe = sc_buf.p2_opts_off[i + 1];
                        for (uint32_t oi = os; oi < oe; ++oi) {
                            const uint32_t k = sc_buf.p2_opts[oi];
                            const Cand& ck = ctx.cands[t.cand_begin + k];
                            const uint8_t ce = ck.diac[ck.case_pos];
                            if (ce < irab2_nc) {
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
            }

            if (dump_out != nullptr) {
                return std::string();
            }

            // Pass-2 Bidirectional Case-Lattice Beam Re-Decode (reusing precomputed p2_opts)!
            run_case_beam(&sc_buf.irab2_bonus, v7_weights_[V7W_P2_SYN_SCALE],
                          has_core2_locks ? &core2_locked : nullptr,
                          &sc_buf.p2_opts, &sc_buf.p2_opts_off);
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
            if (t.si.al_len >= 2 && !(t.xflags & XF_AL_SUN)) {
                al_sukun_vote += (c.ext_flags & EF_LAM_SUKUN) ? 1 : -1;
            }
        }
        if (al_sukun_vote != 0) {
            bool want_suk = al_sukun_vote > 0;
            for (size_t i = 0; i < W; ++i) {
                const Token& t = ctx.toks[i];
                if (t.cand_count < 2 || t.si.al_len < 2 || (t.xflags & XF_AL_SUN)) continue;
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
            if ((t.xflags & XF_IS_HAYTHU) && target_fam != FAM_DAMMA) return false;
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
                    const bool is_noun_or_idafa = (pre_cur.verb_pattern == 0 && !(t.xflags & XF_IS_ABA_VERB) &&
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
                if (pt && !t.boundary_before && (pt->xflags & XF_IS_AN_PREP) && (t.xflags & XF_IS_BARE_BAAD)) {
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
                    (cur.verb_pattern == 1 || (t.top_verb == 1 && (t.xflags & XF_STARTS_IMPERF))) &&
                    !c_al && !(cur.flags & CF_SUFFIX) && cur.case_pos == t.n - 1 &&
                    ts[cur.case_pos] != S_WAW && ts[cur.case_pos] != S_YEH && ts[cur.case_pos] != S_ALEF_MAKSURA &&
                    cf == FAM_DAMMA) {
                    const uint8_t target_j = diac_has_shadda(cur.diac[cur.case_pos]) ? FAM_FATHA : FAM_SUKUN;
                    enforce_case_fam(i, target_j, 0, 3500, t.n >= 4);
                } else if (f.clause_verb_mood == FAM_FATHA &&
                           (cur.verb_pattern == 1 || t.si.particle == PART_KANA) &&
                           (t.xflags & XF_STARTS_IMPERF) && !c_al && !(cur.flags & CF_SUFFIX) &&
                           cur.case_pos == t.n - 1 && !is_invar_long_vowel_end(ts, t.n, cur.case_pos, cur) &&
                           (cf == FAM_DAMMA || cf == FAM_KASRA)) {
                    enforce_case_fam(i, FAM_FATHA, 0, 2600, false);
                } else if (f.clause_verb_mood == FAM_NONE &&
                           (cur.verb_pattern == 1 || t.si.particle == PART_KANA) &&
                           (t.xflags & XF_STARTS_IMPERF) && !c_al && !(cur.flags & (CF_SUFFIX | CF_LAM_NASB)) &&
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

                if (t.xflags & XF_IS_HAYTHU) {
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
                    (t.xflags & XF_IS_DAY_WEEK) && cf == FAM_KASRA) {
                    enforce_case_fam(i, FAM_FATHA, 0, 3500, true);
                }

                // Law C: Preposition / shadda-أيّ + non-diptote noun (including declinable locative & idafa nouns) -> mandatory KASRA
                const bool c_idafa_or_loc = is_declinable_prep_noun(ts, t.n) ||
                                            (t.si.particle == PART_IDAFA_NOUN && !(t.surf_flags & SF_FIVE_NOUN) &&
                                             ts[t.n - 1] != S_ALEF_MAKSURA && ts[t.n - 1] != S_ALEF);
                if ((c_noun_any || (cur.verb_pattern == 0 && c_idafa_or_loc)) &&
                    !comp_num_tok && !eff_diptote && !(t.xflags & XF_IS_HAYTHU) &&
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
                             (pt->xflags & XF_IS_TITLE))))) {
                    const Cand& cur_ali = get_cand(i);
                    const bool cur_is_prep_alayya = (cur_ali.diac[t.n - 2] == DIAC_FATHA);
                    const bool pt_is_title = pt && ((pt->xflags & XF_IS_TITLE) != 0);
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
                         (pc->verb_pattern == 1 && (pt->xflags & XF_IS_KANA_VERB))) &&
                        c_al && cf != FAM_DAMMA && !(cur.ext_flags & EF_FEM_PL_AT)) {
                        const bool kana_elative_tamyiz = (pc->ext_flags & EF_PAST_TEH_FEM) &&
                                                         t.n >= 6 && ts[2] == S_ALEF_HAMZA_A &&
                                                         nx && !nx->si.al_len && nx->si.particle == PART_NONE &&
                                                         (nx->lex_top_end == DIAC_FATHATAN || nx->lex_top_end == DIAC_SHADDA_FATHATAN);
                        if (!kana_elative_tamyiz) enforce_case_fam(i, FAM_DAMMA, 0, 3500, true);
                    } else if (f.gov == GOV_VERB_NEED_SUBJ && pc->verb_pattern == 1 &&
                               !(t.xflags & XF_IS_TIME_ADV)) {
                        const bool nx_ina_comp = nx && ((nx->xflags & XF_IS_INA_CLAUSE) != 0);
                        const bool afal_after_pass = (pc->pos_tag == POS_VERB_PASS && !c_al && t.n == 4 && ts[0] == S_ALEF_HAMZA_A);
                        if (!(cur.ext_flags & EF_FEM_PL_AT) && !afal_after_pass &&
                            (pc->pos_tag == POS_VERB_PASS || has_verb_obj_suffix(&ctx.syms[pt->off], pt->n, *pc) ||
                             nx_ina_comp || (t.xflags & XF_IS_TITLE))) {
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
                                           !(s_cur.flags & CF_SUFFIX) && !(t.xflags & XF_IS_TIME_ADV) &&
                                           !s_ith_adv && !(t.xflags & XF_IS_HUNAKA) && !(t.surf_flags & SF_THEMMA) &&
                                           (t.top_verb != 1 || s_elative);
                    const bool s_ghayr_tok = (t.si.particle == PART_IDAFA_NOUN && t.n == 3 &&
                                              ts[0] == S_GHAIN && ts[1] == S_YEH && ts[2] == S_REH);
                    if (f.fronted_pp && (f.gov == GOV_KANA_ISM || f.gov == GOV_INNA_ISM) && s_noun_ok &&
                        t.si.particle == PART_NONE && !t.digit_after && pt &&
                        (pt->si.particle == PART_PREP_PRON || (pt->surf_flags & SF_THEMMA) ||
                         (pt->xflags & XF_IS_HUNAKA)) &&
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
                    (t.xflags & XF_IS_MASDAR_ADV) && nx && nx->si.particle == PART_GEN && cf == FAM_KASRA) {
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
                               (pt->si.al_len > 0 || (pt->xflags & XF_IS_ALLAH)) && !pt->si.has_prep &&
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
                           (pt->xflags & XF_IS_TITLE) &&
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
            if (t.si.particle == PART_KANA || (fin.verb_pattern == 1 && (t.xflags & XF_IS_KANA_VERB))) {
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
                           (t.xflags & XF_IS_HUNAKA)) {
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
    diacritize_internal(plain.data(), plain.size(), &out, &gold_diacs, nullptr, nullptr, nullptr);
}

void Diacritizer::dump_core2_sentence(const char* gold_utf8, size_t len,
                                      std::vector<Core2DumpSample>& out, uint32_t& next_group_id) const {
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
    diacritize_internal(plain.data(), plain.size(), nullptr, &gold_diacs, &out, &next_group_id, nullptr);
}

void Diacritizer::dump_v9_sentence(const char* gold_utf8, size_t len, std::vector<V9DumpToken>& out) const {
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
    diacritize_internal(plain.data(), plain.size(), nullptr, &gold_diacs, nullptr, nullptr, &out);
}

}
