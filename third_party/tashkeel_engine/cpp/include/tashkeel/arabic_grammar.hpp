#pragma once

#include "format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace tashkeel {

enum Sym : uint8_t {
    S_HAMZA = 0, S_ALEF_MADDA = 1, S_ALEF_HAMZA_A = 2, S_WAW_HAMZA = 3, S_ALEF_HAMZA_B = 4,
    S_YEH_HAMZA = 5, S_ALEF = 6, S_BEH = 7, S_TEH_MARBUTA = 8, S_TEH = 9, S_THEH = 10,
    S_JEEM = 11, S_HAH = 12, S_KHAH = 13, S_DAL = 14, S_THAL = 15, S_REH = 16, S_ZAIN = 17,
    S_SEEN = 18, S_SHEEN = 19, S_SAD = 20, S_DAD = 21, S_TAH = 22, S_ZAH = 23, S_AIN = 24,
    S_GHAIN = 25, S_FEH = 26, S_QAF = 27, S_KAF = 28, S_LAM = 29, S_MEEM = 30, S_NOON = 31,
    S_HEH = 32, S_WAW = 33, S_ALEF_MAKSURA = 34, S_YEH = 35, S_ALEF_WASLA = 36, S_OTHER = 37
};

enum ParticleId : uint8_t {
    PART_NONE = 0, PART_GEN = 1, PART_AN = 2, PART_NASB = 3, PART_JAZM = 4, PART_INNA = 5,
    PART_IN = 6, PART_KANA = 7, PART_CONJ = 8, PART_LA = 9, PART_SAWFA = 10, PART_QAD = 11,
    PART_YA = 12, PART_DEM = 13, PART_REL = 14, PART_PRON = 15, PART_QUEST = 16, PART_COND = 17,
    PART_IDAFA_NOUN = 18, PART_PREP_PRON = 19, PART_AMMA = 20,
    PART_LAKIN = 21, PART_AY = 22, PART_LAMA = 23, PART_LI_AN = 24, PART_NUM_IDAFA = 25,
    PART_HATTA = 26, PART_ILLA = 27,
    NUM_PARTICLES = 28
};

enum CaseFamily : uint8_t { FAM_NONE = 0, FAM_DAMMA = 1, FAM_FATHA = 2, FAM_KASRA = 3, FAM_SUKUN = 4 };

inline uint32_t sym_to_cp(uint8_t s) {
    if (s <= 25) return 0x0621u + s;
    if (s <= 35) return 0x0641u + (s - 26);
    if (s == 36) return 0x0671u;
    return 0x0640u;
}

inline bool is_alef_or_wasla(uint8_t s) {
    return s == S_ALEF || s == S_ALEF_WASLA;
}

inline bool is_sun_letter(uint8_t s) {
    constexpr uint64_t SUN_MASK =
        (1ULL << S_TEH) | (1ULL << S_THEH) | (1ULL << S_DAL) | (1ULL << S_THAL) |
        (1ULL << S_REH) | (1ULL << S_ZAIN) | (1ULL << S_SEEN) | (1ULL << S_SHEEN) |
        (1ULL << S_SAD) | (1ULL << S_DAD) | (1ULL << S_TAH) | (1ULL << S_ZAH) |
        (1ULL << S_LAM) | (1ULL << S_NOON);
    return s < 64u && ((SUN_MASK >> s) & 1ULL) != 0ULL;
}

inline uint8_t diac_family(uint8_t d) {
    static constexpr uint8_t LUT[16] = {
        FAM_NONE,  // 0: DIAC_NONE
        FAM_FATHA, // 1: DIAC_FATHA
        FAM_DAMMA, // 2: DIAC_DAMMA
        FAM_KASRA, // 3: DIAC_KASRA
        FAM_SUKUN, // 4: DIAC_SUKUN
        FAM_FATHA, // 5: DIAC_FATHATAN
        FAM_DAMMA, // 6: DIAC_DAMMATAN
        FAM_KASRA, // 7: DIAC_KASRATAN
        FAM_NONE,  // 8: DIAC_SHADDA
        FAM_FATHA, // 9: DIAC_SHADDA_FATHA
        FAM_DAMMA, // 10: DIAC_SHADDA_DAMMA
        FAM_KASRA, // 11: DIAC_SHADDA_KASRA
        FAM_FATHA, // 12: DIAC_SHADDA_FATHATAN
        FAM_DAMMA, // 13: DIAC_SHADDA_DAMMATAN
        FAM_KASRA, // 14: DIAC_SHADDA_KASRATAN
        FAM_NONE
    };
    return (d < 16u) ? LUT[d] : static_cast<uint8_t>(FAM_NONE);
}

inline bool diac_is_tanween(uint8_t d) {
    constexpr uint16_t TAN_MASK =
        (1u << DIAC_FATHATAN) | (1u << DIAC_DAMMATAN) | (1u << DIAC_KASRATAN) |
        (1u << DIAC_SHADDA_FATHATAN) | (1u << DIAC_SHADDA_DAMMATAN) | (1u << DIAC_SHADDA_KASRATAN);
    return d < 16u && ((TAN_MASK >> d) & 1u) != 0u;
}

inline bool diac_has_shadda(uint8_t d) { return d >= DIAC_SHADDA; }

inline uint8_t make_diac(uint8_t fam, bool tanween, bool shadda) {
    static constexpr uint8_t LUT[5][4] = {
        {DIAC_NONE,  DIAC_SHADDA,       DIAC_NONE,     DIAC_SHADDA},          // FAM_NONE
        {DIAC_DAMMA, DIAC_SHADDA_DAMMA, DIAC_DAMMATAN, DIAC_SHADDA_DAMMATAN}, // FAM_DAMMA
        {DIAC_FATHA, DIAC_SHADDA_FATHA, DIAC_FATHATAN, DIAC_SHADDA_FATHATAN}, // FAM_FATHA
        {DIAC_KASRA, DIAC_SHADDA_KASRA, DIAC_KASRATAN, DIAC_SHADDA_KASRATAN}, // FAM_KASRA
        {DIAC_SUKUN, DIAC_SHADDA,       DIAC_SUKUN,    DIAC_SHADDA},          // FAM_SUKUN
    };
    if (fam < 5u) return LUT[fam][(static_cast<uint8_t>(tanween) << 1) | static_cast<uint8_t>(shadda)];
    return shadda ? DIAC_SHADDA : DIAC_NONE;
}

inline uint8_t strip_tanween(uint8_t d) {
    static constexpr uint8_t LUT[16] = {
        DIAC_NONE, DIAC_FATHA, DIAC_DAMMA, DIAC_KASRA, DIAC_SUKUN,
        DIAC_FATHA, DIAC_DAMMA, DIAC_KASRA,
        DIAC_SHADDA, DIAC_SHADDA_FATHA, DIAC_SHADDA_DAMMA, DIAC_SHADDA_KASRA,
        DIAC_SHADDA_FATHA, DIAC_SHADDA_DAMMA, DIAC_SHADDA_KASRA, DIAC_NONE
    };
    return (d < 16u) ? LUT[d] : d;
}

inline uint8_t add_shadda(uint8_t d) {
    static constexpr uint8_t LUT[16] = {
        DIAC_SHADDA, DIAC_SHADDA_FATHA, DIAC_SHADDA_DAMMA, DIAC_SHADDA_KASRA, DIAC_SHADDA,
        DIAC_SHADDA_FATHATAN, DIAC_SHADDA_DAMMATAN, DIAC_SHADDA_KASRATAN,
        DIAC_SHADDA, DIAC_SHADDA_FATHA, DIAC_SHADDA_DAMMA, DIAC_SHADDA_KASRA,
        DIAC_SHADDA_FATHATAN, DIAC_SHADDA_DAMMATAN, DIAC_SHADDA_KASRATAN, DIAC_SHADDA
    };
    return (d < 16u) ? LUT[d] : d;
}

struct ParticleEntry {
    const char* word;
    uint8_t id;
};

#include "particles_table.inc"

inline std::string utf8_to_sym_key(const char* w) {
    std::string key;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(w);
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) { cp = *p; p += 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        uint8_t s = S_OTHER;
        if (cp >= 0x0621 && cp <= 0x063A) s = static_cast<uint8_t>(cp - 0x0621);
        else if (cp >= 0x0641 && cp <= 0x064A) s = static_cast<uint8_t>(26 + cp - 0x0641);
        else if (cp == 0x0671) s = S_ALEF_WASLA;
        key.push_back(static_cast<char>(s));
    }
    return key;
}

inline uint64_t fnv1a_syms(const uint8_t* s, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= s[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

struct ParticleSlot {
    uint64_t hash = 0;
    uint8_t syms[16] = {};
    uint8_t len = 0;
    uint8_t id = 0;
};

struct ParticleFastTable {
    static constexpr size_t CAP = 1024;
    std::array<ParticleSlot, CAP> slots{};

    ParticleFastTable() {
        for (const auto& e : PARTICLE_TABLE) {
            std::string k = utf8_to_sym_key(e.word);
            if (k.empty() || k.size() > 16) continue;
            const auto* ks = reinterpret_cast<const uint8_t*>(k.data());
            uint64_t h = fnv1a_syms(ks, k.size());
            size_t idx = static_cast<size_t>(h) & (CAP - 1);
            for (size_t step = 0; step < CAP; ++step) {
                ParticleSlot& sl = slots[(idx + step) & (CAP - 1)];
                if (sl.hash == 0) {
                    sl.hash = h;
                    sl.len = static_cast<uint8_t>(k.size());
                    sl.id = e.id;
                    std::memcpy(sl.syms, ks, k.size());
                    break;
                }
                if (sl.hash == h && sl.len == k.size() && std::memcmp(sl.syms, ks, k.size()) == 0) {
                    break;
                }
            }
        }
    }

    inline uint8_t find(const uint8_t* s, size_t n) const {
        if (n == 0 || n > 16) return PART_NONE;
        uint64_t h = fnv1a_syms(s, n);
        size_t idx = static_cast<size_t>(h) & (CAP - 1);
        for (size_t step = 0; step < 32; ++step) {
            const ParticleSlot& sl = slots[(idx + step) & (CAP - 1)];
            if (sl.hash == 0) return PART_NONE;
            if (sl.hash == h && sl.len == n && std::memcmp(sl.syms, s, n) == 0) return sl.id;
        }
        return PART_NONE;
    }
};

inline const ParticleFastTable& particle_map() {
    static const ParticleFastTable tbl;
    return tbl;
}

inline uint8_t particle_id(const uint8_t* syms, size_t n) {
    if (n == 0) return PART_NONE;
    const auto& m = particle_map();
    uint8_t id = m.find(syms, n);
    if (id != PART_NONE) return id;
    if (n >= 3 && (syms[0] == S_WAW || syms[0] == S_FEH)) {
        id = m.find(syms + 1, n - 1);
        if (id != PART_NONE && id != PART_CONJ) return id;
    }
    return PART_NONE;
}

struct SurfaceInfo {
    uint8_t al_len = 0;
    uint8_t al_lam_pos = 0;
    uint8_t has_prep = 0;
    uint8_t has_conj = 0;
    uint8_t verb_like = 0;
    uint8_t particle = 0;
    uint8_t pre_diac[6] = {0, 0, 0, 0, 0, 0};
};

inline uint8_t proclitic_diac(uint8_t s) {
    switch (s) {
        case S_WAW: case S_FEH: case S_KAF: return DIAC_FATHA;
        case S_BEH: case S_LAM: return DIAC_KASRA;
        default: return DIAC_NONE;
    }
}

inline SurfaceInfo analyze_surface(const uint8_t* syms, size_t n) {
    SurfaceInfo si;
    si.particle = particle_id(syms, n);

    static const uint8_t PREFIXES[9][2] = {
        {0, 0}, {S_BEH, 0}, {S_KAF, 0}, {S_WAW, 0}, {S_FEH, 0},
        {S_WAW, S_BEH}, {S_WAW, S_KAF}, {S_FEH, S_BEH}, {S_FEH, S_KAF}
    };
    static const uint8_t PREFIX_LEN[9] = {0, 1, 1, 1, 1, 2, 2, 2, 2};

    if (si.particle == PART_NONE) {
        bool found = false;
        for (int k = 0; k < 9 && !found; ++k) {
            size_t pl = PREFIX_LEN[k];
            size_t min_stem = pl == 0 ? 2 : 3;
            if (n < pl + 2 + min_stem) continue;
            bool ok = true;
            for (size_t j = 0; j < pl; ++j) if (syms[j] != PREFIXES[k][j]) { ok = false; break; }
            if (!ok || !is_alef_or_wasla(syms[pl]) || syms[pl + 1] != S_LAM) continue;
            si.al_len = static_cast<uint8_t>(pl + 2);
            si.al_lam_pos = static_cast<uint8_t>(pl + 1);
            for (size_t j = 0; j < pl; ++j) {
                si.pre_diac[j] = proclitic_diac(syms[j]);
                if (syms[j] == S_BEH || syms[j] == S_KAF) si.has_prep = 1;
                if (syms[j] == S_WAW || syms[j] == S_FEH) si.has_conj = 1;
            }
            si.pre_diac[pl] = DIAC_NONE;
            si.pre_diac[pl + 1] = DIAC_NONE;
            found = true;
        }
        static const uint8_t LIL_PRE[3] = {0, S_WAW, S_FEH};
        for (int k = 0; k < 3 && !found; ++k) {
            size_t pl = k == 0 ? 0 : 1;
            if (n < pl + 2 + 3) continue;
            if (pl == 1 && syms[0] != LIL_PRE[k]) continue;
            if (syms[pl] != S_LAM || syms[pl + 1] != S_LAM) continue;
            si.al_len = static_cast<uint8_t>(pl + 2);
            si.al_lam_pos = static_cast<uint8_t>(pl + 1);
            if (pl == 1) { si.pre_diac[0] = DIAC_FATHA; si.has_conj = 1; }
            si.pre_diac[pl] = DIAC_KASRA;
            si.pre_diac[pl + 1] = DIAC_NONE;
            si.has_prep = 1;
            found = true;
        }
    }

    const bool is_noun_ending = (syms[n - 1] == S_TEH_MARBUTA) ||
                                (n >= 3 && syms[n - 2] == S_YEH && syms[n - 1] == S_ALEF);
    if (si.al_len == 0 && si.particle == PART_NONE && n >= 3 && !is_noun_ending) {
        size_t s = 0;
        bool fut = false;
        if (n >= 4 && (syms[0] == S_WAW || syms[0] == S_FEH) &&
            (syms[1] == S_YEH || syms[1] == S_TEH || syms[1] == S_NOON || syms[1] == S_SEEN)) {
            s = 1;
        }
        if (syms[s] == S_SEEN && n - s >= 4 &&
            (syms[s + 1] == S_YEH || syms[s + 1] == S_TEH || syms[s + 1] == S_NOON || syms[s + 1] == S_ALEF_HAMZA_A)) {
            s += 1;
            fut = true;
        }
        if (fut) si.verb_like = 2;
        else if (syms[s] == S_YEH && n - s >= 3) si.verb_like = 2;
        else if ((syms[s] == S_TEH || syms[s] == S_NOON) && n - s >= 4) si.verb_like = 1;
    }
    return si;
}

}
