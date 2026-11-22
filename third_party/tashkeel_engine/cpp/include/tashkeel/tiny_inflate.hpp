#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tashkeel {

inline uint32_t crc32_ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    static uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = true;
    }
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

namespace detail {

constexpr int INFLATE_MAX_BITS = 15;
constexpr int INFLATE_LUT_BITS = 9;
constexpr uint32_t INFLATE_LUT_SIZE = 1u << INFLATE_LUT_BITS;
constexpr uint32_t INFLATE_LUT_MASK = INFLATE_LUT_SIZE - 1u;

struct HuffmanTable {
    uint16_t lut[INFLATE_LUT_SIZE];
    uint16_t bl_count[INFLATE_MAX_BITS + 1];
    uint16_t first_code[INFLATE_MAX_BITS + 1];
    uint16_t first_sym[INFLATE_MAX_BITS + 1];
    uint16_t syms[320];

    bool build(const uint8_t* lengths, int num_syms) {
        std::memset(lut, 0, sizeof(lut));
        std::memset(bl_count, 0, sizeof(bl_count));
        std::memset(first_code, 0, sizeof(first_code));
        std::memset(first_sym, 0, sizeof(first_sym));

        for (int i = 0; i < num_syms; ++i) {
            if (lengths[i] > INFLATE_MAX_BITS) return false;
            if (lengths[i] > 0) ++bl_count[lengths[i]];
        }
        int left = 1;
        for (int b = 1; b <= INFLATE_MAX_BITS; ++b) {
            left = (left << 1) - static_cast<int>(bl_count[b]);
            if (left < 0) return false;
        }

        uint32_t code = 0;
        uint16_t sym_off = 0;
        uint32_t next_code[INFLATE_MAX_BITS + 1];
        uint16_t sym_pos[INFLATE_MAX_BITS + 1];
        next_code[0] = 0;
        sym_pos[0] = 0;
        for (int b = 1; b <= INFLATE_MAX_BITS; ++b) {
            code = (code + bl_count[b - 1]) << 1;
            first_code[b] = static_cast<uint16_t>(code);
            next_code[b] = code;
            first_sym[b] = sym_off;
            sym_pos[b] = sym_off;
            sym_off = static_cast<uint16_t>(sym_off + bl_count[b]);
        }

        for (int s = 0; s < num_syms; ++s) {
            int len = lengths[s];
            if (len == 0) continue;
            uint32_t c = next_code[len]++;
            syms[sym_pos[len]++] = static_cast<uint16_t>(s);
            if (len <= INFLATE_LUT_BITS) {
                uint32_t rev = 0;
                for (int b = 0; b < len; ++b) {
                    rev = (rev << 1) | ((c >> b) & 1u);
                }
                uint16_t entry = static_cast<uint16_t>((static_cast<uint32_t>(s) << 4) | static_cast<uint32_t>(len));
                uint32_t step = 1u << len;
                for (uint32_t idx = rev; idx < INFLATE_LUT_SIZE; idx += step) {
                    lut[idx] = entry;
                }
            }
        }
        return true;
    }
};

struct BitReader {
    const uint8_t* src;
    size_t len;
    size_t pos;
    uint64_t buf;
    int cnt;

    BitReader(const uint8_t* s, size_t n) : src(s), len(n), pos(0), buf(0), cnt(0) {}

    inline void refill() {
        while (cnt <= 56 && pos < len) {
            buf |= static_cast<uint64_t>(src[pos++]) << cnt;
            cnt += 8;
        }
    }

    inline bool read_bits(int n, uint32_t& out) {
        if (cnt < n) refill();
        if (cnt < n) return false;
        out = static_cast<uint32_t>(buf & ((1ULL << n) - 1ULL));
        buf >>= n;
        cnt -= n;
        return true;
    }

    inline void align_to_byte() {
        int drop = cnt & 7;
        buf >>= drop;
        cnt -= drop;
        size_t full_bytes = static_cast<size_t>(cnt >> 3);
        pos -= full_bytes;
        buf = 0;
        cnt = 0;
    }

    inline int decode_sym(const HuffmanTable& h) {
        if (cnt < INFLATE_MAX_BITS) refill();
        uint16_t e = h.lut[buf & INFLATE_LUT_MASK];
        if (e != 0) {
            int l = static_cast<int>(e & 15u);
            if (l > cnt) return -1;
            buf >>= l;
            cnt -= l;
            return static_cast<int>(e >> 4);
        }
        uint32_t code = 0;
        for (int l = 1; l <= INFLATE_MAX_BITS; ++l) {
            if (cnt == 0) {
                refill();
                if (cnt == 0) return -1;
            }
            code = (code << 1) | static_cast<uint32_t>(buf & 1u);
            buf >>= 1;
            --cnt;
            uint32_t c = h.bl_count[l];
            if (c > 0 && code >= h.first_code[l] && (code - h.first_code[l]) < c) {
                return static_cast<int>(h.syms[h.first_sym[l] + (code - h.first_code[l])]);
            }
        }
        return -1;
    }
};

} // namespace detail

// Pure C++17 RFC-1951 raw DEFLATE decompressor (zero external dependencies).
inline bool tiny_inflate_raw(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap, size_t* out_len) {
    static constexpr uint16_t LEN_BASE[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static constexpr uint8_t LEN_EXTRA[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static constexpr uint16_t DIST_BASE[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static constexpr uint8_t DIST_EXTRA[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static constexpr uint8_t CL_ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    detail::BitReader br(src, src_len);
    detail::HuffmanTable lit_huff;
    detail::HuffmanTable dist_huff;
    detail::HuffmanTable cl_huff;
    size_t out = 0;

    for (;;) {
        uint32_t bfinal = 0, btype = 0;
        if (!br.read_bits(1, bfinal) || !br.read_bits(2, btype)) return false;

        if (btype == 0) {
            br.align_to_byte();
            if (br.pos + 4 > br.len) return false;
            uint16_t blen = static_cast<uint16_t>(br.src[br.pos] | (static_cast<uint16_t>(br.src[br.pos + 1]) << 8));
            uint16_t nlen = static_cast<uint16_t>(br.src[br.pos + 2] | (static_cast<uint16_t>(br.src[br.pos + 3]) << 8));
            br.pos += 4;
            if ((blen ^ 0xFFFFu) != nlen) return false;
            if (br.pos + blen > br.len || out + blen > dst_cap) return false;
            if (blen > 0) {
                std::memcpy(dst + out, br.src + br.pos, blen);
                br.pos += blen;
                out += blen;
            }
        } else if (btype == 1 || btype == 2) {
            if (btype == 1) {
                uint8_t ll[288];
                uint8_t dl[32];
                for (int i = 0; i <= 143; ++i) ll[i] = 8;
                for (int i = 144; i <= 255; ++i) ll[i] = 9;
                for (int i = 256; i <= 279; ++i) ll[i] = 7;
                for (int i = 280; i <= 287; ++i) ll[i] = 8;
                for (int i = 0; i < 32; ++i) dl[i] = 5;
                if (!lit_huff.build(ll, 288) || !dist_huff.build(dl, 32)) return false;
            } else {
                uint32_t hlit = 0, hdist = 0, hclen = 0;
                if (!br.read_bits(5, hlit) || !br.read_bits(5, hdist) || !br.read_bits(4, hclen)) return false;
                hlit += 257;
                hdist += 1;
                hclen += 4;
                if (hlit > 286 || hdist > 32) return false;

                uint8_t cl_lens[19];
                std::memset(cl_lens, 0, sizeof(cl_lens));
                for (uint32_t i = 0; i < hclen; ++i) {
                    uint32_t v = 0;
                    if (!br.read_bits(3, v)) return false;
                    cl_lens[CL_ORDER[i]] = static_cast<uint8_t>(v);
                }
                if (!cl_huff.build(cl_lens, 19)) return false;

                uint8_t all_lens[320];
                uint32_t total = hlit + hdist;
                uint32_t idx = 0;
                while (idx < total) {
                    int sym = br.decode_sym(cl_huff);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        all_lens[idx++] = static_cast<uint8_t>(sym);
                    } else if (sym == 16) {
                        if (idx == 0) return false;
                        uint32_t ex = 0;
                        if (!br.read_bits(2, ex)) return false;
                        uint32_t rep = 3 + ex;
                        if (idx + rep > total) return false;
                        uint8_t prev = all_lens[idx - 1];
                        for (uint32_t k = 0; k < rep; ++k) all_lens[idx++] = prev;
                    } else if (sym == 17) {
                        uint32_t ex = 0;
                        if (!br.read_bits(3, ex)) return false;
                        uint32_t rep = 3 + ex;
                        if (idx + rep > total) return false;
                        for (uint32_t k = 0; k < rep; ++k) all_lens[idx++] = 0;
                    } else if (sym == 18) {
                        uint32_t ex = 0;
                        if (!br.read_bits(7, ex)) return false;
                        uint32_t rep = 11 + ex;
                        if (idx + rep > total) return false;
                        for (uint32_t k = 0; k < rep; ++k) all_lens[idx++] = 0;
                    } else {
                        return false;
                    }
                }
                if (!lit_huff.build(all_lens, static_cast<int>(hlit)) ||
                    !dist_huff.build(all_lens + hlit, static_cast<int>(hdist))) {
                    return false;
                }
            }

            for (;;) {
                int sym = br.decode_sym(lit_huff);
                if (sym < 0) return false;
                if (sym < 256) {
                    if (out >= dst_cap) return false;
                    dst[out++] = static_cast<uint8_t>(sym);
                } else if (sym == 256) {
                    break;
                } else if (sym <= 285) {
                    int li = sym - 257;
                    uint32_t ex = 0;
                    if (LEN_EXTRA[li] > 0 && !br.read_bits(LEN_EXTRA[li], ex)) return false;
                    size_t match_len = static_cast<size_t>(LEN_BASE[li]) + ex;

                    int dsym = br.decode_sym(dist_huff);
                    if (dsym < 0 || dsym >= 30) return false;
                    uint32_t dex = 0;
                    if (DIST_EXTRA[dsym] > 0 && !br.read_bits(DIST_EXTRA[dsym], dex)) return false;
                    size_t dist = static_cast<size_t>(DIST_BASE[dsym]) + dex;

                    if (dist == 0 || dist > out || out + match_len > dst_cap) return false;
                    if (dist >= match_len) {
                        std::memcpy(dst + out, dst + out - dist, match_len);
                        out += match_len;
                    } else {
                        for (size_t k = 0; k < match_len; ++k) {
                            dst[out] = dst[out - dist];
                            ++out;
                        }
                    }
                } else {
                    return false;
                }
            }
        } else {
            return false;
        }

        if (bfinal) break;
    }

    if (out_len) *out_len = out;
    return true;
}

} // namespace tashkeel
