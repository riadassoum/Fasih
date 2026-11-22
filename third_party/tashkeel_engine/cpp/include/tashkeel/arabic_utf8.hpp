#pragma once

#include "format.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace tashkeel {

inline uint32_t utf8_next(const char* s, size_t len, size_t& pos) {
    uint8_t c = static_cast<uint8_t>(s[pos]);
    if (c < 0x80) {
        ++pos;
        return c;
    } else if ((c & 0xE0) == 0xC0 && pos + 1 < len) {
        uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(s[pos + 1]) & 0x3F);
        pos += 2;
        return cp;
    } else if ((c & 0xF0) == 0xE0 && pos + 2 < len) {
        uint32_t cp = ((c & 0x0F) << 12) |
                      ((static_cast<uint8_t>(s[pos + 1]) & 0x3F) << 6) |
                      (static_cast<uint8_t>(s[pos + 2]) & 0x3F);
        pos += 3;
        return cp;
    } else if ((c & 0xF8) == 0xF0 && pos + 3 < len) {
        uint32_t cp = ((c & 0x07) << 18) |
                      ((static_cast<uint8_t>(s[pos + 1]) & 0x3F) << 12) |
                      ((static_cast<uint8_t>(s[pos + 2]) & 0x3F) << 6) |
                      (static_cast<uint8_t>(s[pos + 3]) & 0x3F);
        pos += 4;
        return cp;
    }
    ++pos;
    return c;
}

inline void utf8_append(std::string& out, uint32_t cp) {
    if (cp < 0x800) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else {
            char buf[2] = {
                static_cast<char>(0xC0 | (cp >> 6)),
                static_cast<char>(0x80 | (cp & 0x3F))
            };
            out.append(buf, 2);
        }
    } else if (cp < 0x10000) {
        char buf[3] = {
            static_cast<char>(0xE0 | (cp >> 12)),
            static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
            static_cast<char>(0x80 | (cp & 0x3F))
        };
        out.append(buf, 3);
    } else {
        char buf[4] = {
            static_cast<char>(0xF0 | (cp >> 18)),
            static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
            static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
            static_cast<char>(0x80 | (cp & 0x3F))
        };
        out.append(buf, 4);
    }
}

inline bool is_arabic_diacritic(uint32_t cp) {
    return (cp >= 0x064B && cp <= 0x0652) || cp == 0x0670;
}

inline bool is_tatweel(uint32_t cp) {
    return cp == 0x0640;
}

inline bool is_arabic_letter(uint32_t cp) {
    return (cp >= 0x0621 && cp <= 0x063A) ||
           (cp >= 0x0641 && cp <= 0x064A) ||
           cp == 0x0671;
}

// Map Arabic base letter to compact symbol ID [0..36], or 37 for non-standard
inline uint8_t char_to_sym(uint32_t cp) {
    if (cp >= 0x0621 && cp <= 0x063A) {
        return static_cast<uint8_t>(cp - 0x0621); // 0..25
    }
    if (cp >= 0x0641 && cp <= 0x064A) {
        return static_cast<uint8_t>(26 + (cp - 0x0641)); // 26..35
    }
    if (cp == 0x0671) {
        return 36; // Alef Wasla
    }
    return 37;
}

inline void append_diacritic(std::string& out, uint8_t diac_id) {
    if (diac_id == DIAC_NONE || diac_id >= NUM_DIACRITICS) return;
    struct DiacUtf8 {
        uint8_t len;
        char bytes[4];
    };
    static constexpr DiacUtf8 LUT[NUM_DIACRITICS] = {
        {0, {0, 0, 0, 0}},                                                         // 0: NONE
        {2, {'\xD9', '\x8E', 0, 0}},                                               // 1: FATHA 064E
        {2, {'\xD9', '\x8F', 0, 0}},                                               // 2: DAMMA 064F
        {2, {'\xD9', '\x90', 0, 0}},                                               // 3: KASRA 0650
        {2, {'\xD9', '\x92', 0, 0}},                                               // 4: SUKUN 0652
        {2, {'\xD9', '\x8B', 0, 0}},                                               // 5: FATHATAN 064B
        {2, {'\xD9', '\x8C', 0, 0}},                                               // 6: DAMMATAN 064C
        {2, {'\xD9', '\x8D', 0, 0}},                                               // 7: KASRATAN 064D
        {2, {'\xD9', '\x91', 0, 0}},                                               // 8: SHADDA 0651
        {4, {'\xD9', '\x91', '\xD9', '\x8E'}},                                     // 9: SHADDA+FATHA
        {4, {'\xD9', '\x91', '\xD9', '\x8F'}},                                     // 10: SHADDA+DAMMA
        {4, {'\xD9', '\x91', '\xD9', '\x90'}},                                     // 11: SHADDA+KASRA
        {4, {'\xD9', '\x91', '\xD9', '\x8B'}},                                     // 12: SHADDA+FATHATAN
        {4, {'\xD9', '\x91', '\xD9', '\x8C'}},                                     // 13: SHADDA+DAMMATAN
        {4, {'\xD9', '\x91', '\xD9', '\x8D'}},                                     // 14: SHADDA+KASRATAN
    };
    const DiacUtf8& e = LUT[diac_id];
    out.append(e.bytes, e.len);
}

} // namespace tashkeel
