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
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
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
    switch (diac_id) {
        case DIAC_NONE: break;
        case DIAC_FATHA: utf8_append(out, 0x064E); break;
        case DIAC_DAMMA: utf8_append(out, 0x064F); break;
        case DIAC_KASRA: utf8_append(out, 0x0650); break;
        case DIAC_SUKUN: utf8_append(out, 0x0652); break;
        case DIAC_FATHATAN: utf8_append(out, 0x064B); break;
        case DIAC_DAMMATAN: utf8_append(out, 0x064C); break;
        case DIAC_KASRATAN: utf8_append(out, 0x064D); break;
        case DIAC_SHADDA: utf8_append(out, 0x0651); break;
        case DIAC_SHADDA_FATHA:
            utf8_append(out, 0x0651); utf8_append(out, 0x064E); break;
        case DIAC_SHADDA_DAMMA:
            utf8_append(out, 0x0651); utf8_append(out, 0x064F); break;
        case DIAC_SHADDA_KASRA:
            utf8_append(out, 0x0651); utf8_append(out, 0x0650); break;
        case DIAC_SHADDA_FATHATAN:
            utf8_append(out, 0x0651); utf8_append(out, 0x064B); break;
        case DIAC_SHADDA_DAMMATAN:
            utf8_append(out, 0x0651); utf8_append(out, 0x064C); break;
        case DIAC_SHADDA_KASRATAN:
            utf8_append(out, 0x0651); utf8_append(out, 0x064D); break;
        default: break;
    }
}

} // namespace tashkeel
