#pragma once

#include "format.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace tashkeel {

inline uint32_t portable_popcnt64(uint64_t x) {
#if defined(_MSC_VER) && defined(_M_X64)
    return static_cast<uint32_t>(__popcnt64(x));
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_popcountll(x));
#else
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    return static_cast<uint32_t>((((x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL) * 0x0101010101010101ULL) >> 56);
#endif
}

inline uint32_t portable_popcnt32(uint32_t x) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return static_cast<uint32_t>(__popcnt(x));
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_popcount(x));
#else
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (((x + (x >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
#endif
}

inline uint64_t read_u48(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, 6);
    return v;
}

inline uint8_t decode_compact_ngram_order(uint8_t code) {
    static constexpr uint8_t MAP[8] = {3, 4, 5, NG_ORDER_RBG, NG_ORDER_CTR3, NG_ORDER_SKIP1, 0, 0};
    return MAP[code & 7u];
}

class BitmappedFST {
public:
    BitmappedFST() = default;

    void init(const BitTrieNode* nodes, uint32_t count) {
        nodes_v2_ = nodes;
        nodes_v3_ = nullptr;
        v5_valid_ = false;
        count_ = count;
    }

    void init_v3(const BitTrieNodeV3* nodes, uint32_t count) {
        nodes_v2_ = nullptr;
        nodes_v3_ = nodes;
        v5_valid_ = false;
        count_ = count;
    }

    bool init_v5(const uint8_t* blob, uint32_t byte_size) {
        nodes_v2_ = nullptr;
        nodes_v3_ = nullptr;
        v5_valid_ = false;
        count_ = 0;
        if (!blob || byte_size < sizeof(FstV5Header)) return false;
        const auto* h = reinterpret_cast<const FstV5Header*>(blob);
        size_t off = (sizeof(FstV5Header) + 7u) & ~7u;
        size_t bw_bytes = static_cast<size_t>(h->bitmap_words) * sizeof(uint64_t);
        size_t sb_bytes = ((static_cast<size_t>(h->super_blocks) * sizeof(uint32_t)) + 7u) & ~7u;
        size_t in_bytes = static_cast<size_t>(h->num_internal) * sizeof(uint64_t);
        size_t tl_bytes = h->tails_bytes;
        if (off + 2 * bw_bytes + 2 * sb_bytes + in_bytes + tl_bytes > byte_size) return false;

        v5_num_nodes_ = h->num_nodes;
        v5_num_internal_ = h->num_internal;
        v5_num_words_ = h->num_words;
        v5_tails_bytes_ = h->tails_bytes;
        v5_is_term_bits_ = reinterpret_cast<const uint64_t*>(blob + off);
        off += bw_bytes;
        v5_is_term_super_ = reinterpret_cast<const uint32_t*>(blob + off);
        off += sb_bytes;
        v5_has_child_bits_ = reinterpret_cast<const uint64_t*>(blob + off);
        off += bw_bytes;
        v5_has_child_super_ = reinterpret_cast<const uint32_t*>(blob + off);
        off += sb_bytes;
        v5_internal_ = reinterpret_cast<const uint64_t*>(blob + off);
        off += in_bytes;
        v5_tails_ = blob + off;
        count_ = h->num_nodes;
        v5_valid_ = true;
        return true;
    }

    bool empty() const { return !v5_valid_ && ((nodes_v2_ == nullptr && nodes_v3_ == nullptr) || count_ == 0); }
    uint32_t node_count() const { return count_; }

    inline uint32_t lookup(const uint8_t* syms, size_t len) const {
        if (count_ == 0 || len == 0) return UINT32_MAX;
        if (v5_valid_) return lookup_v5(syms, len);
        if (nodes_v3_) return lookup_v3(syms, len);
        if (!nodes_v2_) return UINT32_MAX;
        uint32_t idx = 0;
        for (size_t i = 0; i < len; ++i) {
            uint8_t s = syms[i];
            if (s >= 64 || idx >= count_) return UINT32_MAX;
            const BitTrieNode& node = nodes_v2_[idx];
            uint64_t bit = 1ULL << s;
            if ((node.child_mask & bit) == 0) return UINT32_MAX;
            uint64_t below = node.child_mask & (bit - 1ULL);
            uint32_t offset = portable_popcnt64(below);
            idx = node.first_child + offset;
        }
        if (idx >= count_) return UINT32_MAX;
        return nodes_v2_[idx].value;
    }

private:
    static inline bool get_bit(const uint64_t* bits, uint32_t idx) {
        return ((bits[idx >> 6] >> (idx & 63u)) & 1ULL) != 0;
    }

    static inline uint32_t rank1(const uint64_t* bits, const uint32_t* super, uint32_t idx) {
        uint32_t sb = idx >> 8;
        uint32_t r = super[sb];
        uint32_t w_start = sb << 2;
        uint32_t w_end = idx >> 6;
        for (uint32_t w = w_start; w < w_end; ++w) {
            r += portable_popcnt64(bits[w]);
        }
        uint32_t rem = idx & 63u;
        if (rem) {
            r += portable_popcnt64(bits[w_end] & ((1ULL << rem) - 1ULL));
        }
        return r;
    }

    inline uint32_t lookup_v5(const uint8_t* syms, size_t len) const {
        constexpr uint64_t MASK38 = (1ULL << 38) - 1ULL;
        uint32_t idx = 0;
        for (size_t i = 0; i < len; ++i) {
            if (idx >= v5_num_nodes_ || !get_bit(v5_has_child_bits_, idx)) return UINT32_MAX;
            uint32_t int_idx = rank1(v5_has_child_bits_, v5_has_child_super_, idx);
            if (int_idx >= v5_num_internal_) return UINT32_MAX;
            uint64_t node = v5_internal_[int_idx];
            uint64_t mask = node & MASK38;
            if (mask == 0) {
                // Collapsed linear tail chain
                uint32_t toff = static_cast<uint32_t>(node >> 38);
                if (toff >= v5_tails_bytes_) return UINT32_MAX;
                uint8_t tlen = v5_tails_[toff];
                if (len - i != tlen || toff + 1u + tlen + 3u > v5_tails_bytes_) return UINT32_MAX;
                if (std::memcmp(syms + i, v5_tails_ + toff + 1u, tlen) != 0) return UINT32_MAX;
                return read_u24(v5_tails_ + toff + 1u + tlen);
            }
            uint8_t s = syms[i];
            if (s >= 38) return UINT32_MAX;
            uint64_t bit = 1ULL << s;
            if ((mask & bit) == 0) return UINT32_MAX;
            uint32_t offset = portable_popcnt64(mask & (bit - 1ULL));
            uint32_t first_child = static_cast<uint32_t>(node >> 38);
            idx = first_child + offset;
        }
        if (idx >= v5_num_nodes_ || !get_bit(v5_is_term_bits_, idx)) return UINT32_MAX;
        return rank1(v5_is_term_bits_, v5_is_term_super_, idx);
    }

    inline uint32_t lookup_v3(const uint8_t* syms, size_t len) const {
        constexpr uint64_t MASK38 = (1ULL << 38) - 1ULL;
        constexpr uint32_t NON_TERM = 0x1FFFFFU;
        uint32_t idx = 0;
        for (size_t i = 0; i < len; ++i) {
            uint8_t s = syms[i];
            if (s >= 38 || idx >= count_) return UINT32_MAX;
            const uint64_t lo = nodes_v3_[idx].lo;
            const uint64_t mask = lo & MASK38;
            const uint64_t bit = 1ULL << s;
            if ((mask & bit) == 0) return UINT32_MAX;
            const uint64_t below = mask & (bit - 1ULL);
            const uint32_t offset = portable_popcnt64(below);
            const uint32_t first_child = static_cast<uint32_t>((lo >> 38) & NON_TERM);
            idx = first_child + offset;
        }
        if (idx >= count_) return UINT32_MAX;
        const BitTrieNodeV3& end = nodes_v3_[idx];
        const uint32_t val = static_cast<uint32_t>((end.lo >> 59) | (static_cast<uint32_t>(end.hi) << 5));
        return val == NON_TERM ? UINT32_MAX : val;
    }

    const BitTrieNode* nodes_v2_ = nullptr;
    const BitTrieNodeV3* nodes_v3_ = nullptr;
    uint32_t count_ = 0;

    bool v5_valid_ = false;
    uint32_t v5_num_nodes_ = 0;
    uint32_t v5_num_internal_ = 0;
    uint32_t v5_num_words_ = 0;
    uint32_t v5_tails_bytes_ = 0;
    const uint64_t* v5_is_term_bits_ = nullptr;
    const uint32_t* v5_is_term_super_ = nullptr;
    const uint64_t* v5_has_child_bits_ = nullptr;
    const uint32_t* v5_has_child_super_ = nullptr;
    const uint64_t* v5_internal_ = nullptr;
    const uint8_t* v5_tails_ = nullptr;
};

class BigramTable {
public:
    BigramTable() = default;

    void init(const BigramEntry* entries, uint32_t capacity) {
        entries_v2_ = entries;
        capacity_ = capacity;
        mask_ = capacity > 0 ? (capacity - 1) : 0;
        row_off_ = nullptr;
        block_base_v5_ = nullptr;
        entries_v3_ = nullptr;
        entries_v5_ = nullptr;
        vals_v3_ = nullptr;
        vals_v4_ = nullptr;
    }

    void init_v3(const uint32_t* row_off, uint32_t row_off_count,
                 const BigramEntryV3* entries, uint32_t entry_count,
                 const int16_t* vals, uint32_t val_count) {
        entries_v2_ = nullptr;
        capacity_ = 0;
        mask_ = 0;
        row_off_ = row_off;
        row_off_count_ = row_off_count;
        block_base_v5_ = nullptr;
        entries_v3_ = entries;
        entries_v5_ = nullptr;
        entry_count_ = entry_count;
        vals_v3_ = vals;
        vals_v4_ = nullptr;
        val_count_ = val_count;
    }

    void init_v4(const uint32_t* row_off, uint32_t row_off_count,
                 const BigramEntryV3* entries, uint32_t entry_count,
                 const int8_t* vals, uint32_t val_count) {
        entries_v2_ = nullptr;
        capacity_ = 0;
        mask_ = 0;
        row_off_ = row_off;
        row_off_count_ = row_off_count;
        block_base_v5_ = nullptr;
        entries_v3_ = entries;
        entries_v5_ = nullptr;
        entry_count_ = entry_count;
        vals_v3_ = nullptr;
        vals_v4_ = vals;
        val_count_ = val_count;
    }

    void init_v5(const uint32_t* row_off, uint32_t row_off_count,
                 const uint32_t* block_base, uint32_t block_base_count,
                 const BigramEntryV5* entries, uint32_t entry_count,
                 const int8_t* vals8, const int16_t* vals16, uint32_t val_count) {
        entries_v2_ = nullptr;
        capacity_ = 0;
        mask_ = 0;
        row_off_ = row_off;
        row_off_count_ = row_off_count;
        block_base_v5_ = block_base;
        block_base_count_ = block_base_count;
        entries_v3_ = nullptr;
        entries_v5_ = entries;
        entry_count_ = entry_count;
        vals_v3_ = vals16;
        vals_v4_ = vals8;
        val_count_ = val_count;
    }

    bool empty() const { return entries_v2_ == nullptr && entries_v3_ == nullptr && entries_v5_ == nullptr; }
    bool is_v3() const { return entries_v3_ != nullptr; }
    bool is_v5() const { return entries_v5_ != nullptr; }

    inline const BigramEntry* find(uint32_t prev_id, uint32_t curr_id) const {
        if (!entries_v2_ || capacity_ == 0) return nullptr;
        uint64_t key = (static_cast<uint64_t>(prev_id) << 32) | static_cast<uint64_t>(curr_id);
        uint64_t h = splitmix64(key);
        uint32_t idx = static_cast<uint32_t>(h) & mask_;
        for (uint32_t step = 0; step < 64; ++step) {
            const BigramEntry& e = entries_v2_[(idx + step) & mask_];
            if (e.key == key) return &e;
            if (e.key == UINT64_MAX) return nullptr;
        }
        return nullptr;
    }

    inline const BigramEntryV3* find_v3(uint32_t prev_id, uint32_t curr_id) const {
        if (!row_off_ || !entries_v3_ || prev_id + 1 >= row_off_count_) return nullptr;
        uint32_t lo = row_off_[prev_id];
        uint32_t hi = row_off_[prev_id + 1];
        if (hi > entry_count_) hi = entry_count_;
        if (lo >= hi) return nullptr;
        if (hi - lo <= 8) {
            for (uint32_t i = lo; i < hi; ++i) {
                uint32_t cid = read_u24(entries_v3_[i].curr_id);
                if (cid == curr_id) return &entries_v3_[i];
                if (cid > curr_id) return nullptr;
            }
            return nullptr;
        }
        while (lo < hi) {
            uint32_t mid = lo + ((hi - lo) >> 1);
            uint32_t cid = read_u24(entries_v3_[mid].curr_id);
            if (cid < curr_id) lo = mid + 1;
            else if (cid > curr_id) hi = mid;
            else return &entries_v3_[mid];
        }
        return nullptr;
    }

    inline int32_t find_v5(uint32_t prev_id, uint32_t curr_id) const {
        if (!row_off_ || !entries_v5_ || prev_id + 1 >= row_off_count_) return -1;
        uint32_t lo = row_off_[prev_id];
        uint32_t hi = row_off_[prev_id + 1];
        if (hi > entry_count_) hi = entry_count_;
        if (lo >= hi) return -1;
        if (hi - lo <= 8) {
            for (uint32_t i = lo; i < hi; ++i) {
                uint32_t cid = read_u24(entries_v5_[i].curr_id);
                if (cid == curr_id) return static_cast<int32_t>(i);
                if (cid > curr_id) return -1;
            }
            return -1;
        }
        while (lo < hi) {
            uint32_t mid = lo + ((hi - lo) >> 1);
            uint32_t cid = read_u24(entries_v5_[mid].curr_id);
            if (cid < curr_id) lo = mid + 1;
            else if (cid > curr_id) hi = mid;
            else return static_cast<int32_t>(mid);
        }
        return -1;
    }

    inline int16_t bonus_v3(const BigramEntryV3* e, uint16_t end_mask, uint8_t ce) const {
        if (!e || ce >= NUM_DIACRITICS) return 0;
        const uint16_t bit = static_cast<uint16_t>(1u << ce);
        if ((end_mask & bit) == 0) return 0;
        const uint32_t offset = portable_popcnt32(end_mask & (bit - 1u));
        const uint32_t idx = e->val_offset + offset;
        if (idx >= val_count_) return 0;
        return vals_v4_ ? static_cast<int16_t>(static_cast<int32_t>(vals_v4_[idx]) * 32) : vals_v3_[idx];
    }

    inline int16_t bonus_v5(int32_t entry_idx, uint16_t end_mask, uint8_t ce) const {
        if (entry_idx < 0 || ce >= NUM_DIACRITICS) return 0;
        const uint16_t bit = static_cast<uint16_t>(1u << ce);
        if ((end_mask & bit) == 0) return 0;
        const uint32_t eidx = static_cast<uint32_t>(entry_idx);
        const uint32_t blk = eidx >> BG_BLOCK_SHIFT_V5;
        if (blk >= block_base_count_) return 0;
        const uint32_t offset = portable_popcnt32(end_mask & (bit - 1u));
        const uint32_t idx = block_base_v5_[blk] + entries_v5_[eidx].local_val_off + offset;
        if (idx >= val_count_) return 0;
        return vals_v4_ ? static_cast<int16_t>(static_cast<int32_t>(vals_v4_[idx]) * 32) : vals_v3_[idx];
    }

private:
    static inline uint64_t splitmix64(uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    const BigramEntry* entries_v2_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t mask_ = 0;

    const uint32_t* row_off_ = nullptr;
    uint32_t row_off_count_ = 0;
    const uint32_t* block_base_v5_ = nullptr;
    uint32_t block_base_count_ = 0;
    const BigramEntryV3* entries_v3_ = nullptr;
    const BigramEntryV5* entries_v5_ = nullptr;
    uint32_t entry_count_ = 0;
    const int16_t* vals_v3_ = nullptr;
    const int8_t* vals_v4_ = nullptr;
    uint32_t val_count_ = 0;
};

struct NgramLookupResult {
    int16_t best_bonus = 0;
    int16_t alt_bonus = 0;
    uint8_t best_ending_id = 0;
    uint8_t alt_ending_id = 0;
    uint8_t order = 0;
};

class NgramTableV6 {
public:
    void init(const NgramEntryV6* entries, uint32_t capacity) {
        entries_ = entries;
        capacity_ = capacity;
        mask_ = (capacity > 0 && (capacity & (capacity - 1u)) == 0) ? (capacity - 1u) : 0u;
        compact_buckets_ = nullptr;
        compact_entries_ = nullptr;
        compact_num_buckets_ = 0;
        compact_num_entries_ = 0;
    }

    bool init_compact(const uint8_t* blob, uint32_t byte_size) {
        entries_ = nullptr;
        capacity_ = 0;
        mask_ = 0;
        compact_buckets_ = nullptr;
        compact_entries_ = nullptr;
        compact_num_buckets_ = 0;
        compact_num_entries_ = 0;
        if (!blob || byte_size < sizeof(CompactBucketHeader)) return false;
        const auto* h = reinterpret_cast<const CompactBucketHeader*>(blob);
        if (h->num_buckets != 65536u) return false;
        size_t off = sizeof(CompactBucketHeader);
        size_t b_bytes = static_cast<size_t>(h->num_buckets + 1u) * sizeof(uint32_t);
        size_t e_bytes = static_cast<size_t>(h->num_entries) * sizeof(CompactNgramEntry);
        if (off + b_bytes + e_bytes > byte_size) return false;
        compact_buckets_ = reinterpret_cast<const uint32_t*>(blob + off);
        compact_entries_ = reinterpret_cast<const CompactNgramEntry*>(blob + off + b_bytes);
        compact_num_buckets_ = h->num_buckets;
        compact_num_entries_ = h->num_entries;
        return true;
    }

    bool empty() const {
        return (entries_ == nullptr || mask_ == 0) && (compact_entries_ == nullptr || compact_num_entries_ == 0);
    }

    static inline uint64_t fnv1a_key(const uint32_t* wids, uint8_t order) {
        uint64_t h = 0xcbf29ce484222325ULL;
        h ^= static_cast<uint64_t>(order);
        h *= 0x100000001b3ULL;
        for (uint8_t k = 0; k < order; ++k) {
            uint32_t w = wids[k];
            for (int b = 0; b < 4; ++b) {
                h ^= static_cast<uint64_t>((w >> (b * 8)) & 0xFFu);
                h *= 0x100000001b3ULL;
            }
        }
        return (h == 0 || h == UINT64_MAX) ? (h ^ 0x9e3779b97f4a7c15ULL) : h;
    }

    inline const NgramEntryV6* find(const uint32_t* wids, uint8_t order, uint8_t n_wids = 0) const {
        if (!entries_ || mask_ == 0) return nullptr;
        const uint8_t cnt = (n_wids == 0) ? order : n_wids;
        uint64_t key = fnv1a_key_tagged(wids, cnt, order);
        uint32_t idx = static_cast<uint32_t>(key ^ (key >> 32)) & mask_;
        for (uint32_t step = 0; step < 32; ++step) {
            const NgramEntryV6& e = entries_[(idx + step) & mask_];
            if (e.key_hash == key && e.order == order) return &e;
            if (e.key_hash == 0) return nullptr;
        }
        return nullptr;
    }

    inline bool find_val(const uint32_t* wids, uint8_t order, NgramLookupResult& out, uint8_t n_wids = 0) const {
        if (compact_entries_) {
            const uint8_t cnt = (n_wids == 0) ? order : n_wids;
            const uint64_t key = fnv1a_key_tagged(wids, cnt, order);
            const uint32_t b = static_cast<uint32_t>(key >> 48);
            const uint64_t klo = key & 0xFFFFFFFFFFFFULL;
            uint32_t lo = compact_buckets_[b];
            uint32_t hi = compact_buckets_[b + 1u];
            if (hi > compact_num_entries_) hi = compact_num_entries_;
            if (lo >= hi) return false;
            if (hi - lo <= 8u) {
                for (uint32_t i = lo; i < hi; ++i) {
                    const uint64_t ek = read_u48(compact_entries_[i].key_lo);
                    if (ek == klo) {
                        uint64_t p = 0;
                        std::memcpy(&p, compact_entries_[i].payload, 5);
                        uint8_t ord = decode_compact_ngram_order(static_cast<uint8_t>((p >> 34) & 7u));
                        if (ord == order) {
                            out.best_bonus = static_cast<int16_t>(static_cast<int32_t>(p & 0x1FFFu) - 4096);
                            out.alt_bonus = static_cast<int16_t>(static_cast<int32_t>((p >> 13) & 0x1FFFu) - 4096);
                            out.best_ending_id = static_cast<uint8_t>((p >> 26) & 0xFu);
                            out.alt_ending_id = static_cast<uint8_t>((p >> 30) & 0xFu);
                            out.order = ord;
                            return true;
                        }
                    } else if (ek > klo) {
                        return false;
                    }
                }
                return false;
            }
            uint32_t l = lo, r = hi;
            while (l < r) {
                uint32_t mid = l + ((r - l) >> 1);
                if (read_u48(compact_entries_[mid].key_lo) < klo) l = mid + 1u;
                else r = mid;
            }
            for (uint32_t i = l; i < hi; ++i) {
                const uint64_t ek = read_u48(compact_entries_[i].key_lo);
                if (ek != klo) break;
                uint64_t p = 0;
                std::memcpy(&p, compact_entries_[i].payload, 5);
                uint8_t ord = decode_compact_ngram_order(static_cast<uint8_t>((p >> 34) & 7u));
                if (ord == order) {
                    out.best_bonus = static_cast<int16_t>(static_cast<int32_t>(p & 0x1FFFu) - 4096);
                    out.alt_bonus = static_cast<int16_t>(static_cast<int32_t>((p >> 13) & 0x1FFFu) - 4096);
                    out.best_ending_id = static_cast<uint8_t>((p >> 26) & 0xFu);
                    out.alt_ending_id = static_cast<uint8_t>((p >> 30) & 0xFu);
                    out.order = ord;
                    return true;
                }
            }
            return false;
        }
        const NgramEntryV6* e = find(wids, order, n_wids);
        if (!e) return false;
        out.best_bonus = e->best_bonus;
        out.alt_bonus = e->alt_bonus;
        out.best_ending_id = e->best_ending_id;
        out.alt_ending_id = e->alt_ending_id;
        out.order = e->order;
        return true;
    }

    static inline uint64_t fnv1a_key_tagged(const uint32_t* wids, uint8_t cnt, uint8_t tag) {
        uint64_t h = 0xcbf29ce484222325ULL;
        h ^= static_cast<uint64_t>(tag);
        h *= 0x100000001b3ULL;
        for (uint8_t k = 0; k < cnt; ++k) {
            uint32_t w = wids[k];
            for (int b = 0; b < 4; ++b) {
                h ^= static_cast<uint64_t>((w >> (b * 8)) & 0xFFu);
                h *= 0x100000001b3ULL;
            }
        }
        return (h == 0 || h == UINT64_MAX) ? (h ^ 0x9e3779b97f4a7c15ULL) : h;
    }

private:
    const NgramEntryV6* entries_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t mask_ = 0;
    const uint32_t* compact_buckets_ = nullptr;
    const CompactNgramEntry* compact_entries_ = nullptr;
    uint32_t compact_num_buckets_ = 0;
    uint32_t compact_num_entries_ = 0;
};

struct CoreBgLookupResult {
    uint16_t best_core_h = 0;
    uint16_t alt_core_h = 0;
    int16_t best_bonus = 0;
    int16_t alt_bonus = 0;
};

class CoreBgTableV7 {
public:
    void init(const CoreBgEntryV7* entries, uint32_t capacity) {
        entries_ = entries;
        capacity_ = capacity;
        mask_ = (capacity > 0 && (capacity & (capacity - 1u)) == 0) ? (capacity - 1u) : 0u;
        compact_buckets_ = nullptr;
        compact_entries_ = nullptr;
        compact_num_buckets_ = 0;
        compact_num_entries_ = 0;
    }

    bool init_compact(const uint8_t* blob, uint32_t byte_size) {
        entries_ = nullptr;
        capacity_ = 0;
        mask_ = 0;
        compact_buckets_ = nullptr;
        compact_entries_ = nullptr;
        compact_num_buckets_ = 0;
        compact_num_entries_ = 0;
        if (!blob || byte_size < sizeof(CompactBucketHeader)) return false;
        const auto* h = reinterpret_cast<const CompactBucketHeader*>(blob);
        if (h->num_buckets != 65536u) return false;
        size_t off = sizeof(CompactBucketHeader);
        size_t b_bytes = static_cast<size_t>(h->num_buckets + 1u) * sizeof(uint32_t);
        size_t e_bytes = static_cast<size_t>(h->num_entries) * sizeof(CompactCoreBgEntry);
        if (off + b_bytes + e_bytes > byte_size) return false;
        compact_buckets_ = reinterpret_cast<const uint32_t*>(blob + off);
        compact_entries_ = reinterpret_cast<const CompactCoreBgEntry*>(blob + off + b_bytes);
        compact_num_buckets_ = h->num_buckets;
        compact_num_entries_ = h->num_entries;
        return true;
    }

    bool empty() const {
        return (entries_ == nullptr || mask_ == 0) && (compact_entries_ == nullptr || compact_num_entries_ == 0);
    }

    inline const CoreBgEntryV7* find(uint8_t dir, const uint32_t* wids, uint8_t cnt) const {
        if (!entries_ || mask_ == 0) return nullptr;
        uint64_t key = NgramTableV6::fnv1a_key_tagged(wids, cnt, static_cast<uint8_t>(0x80u | dir));
        uint32_t idx = static_cast<uint32_t>(key ^ (key >> 32)) & mask_;
        for (uint32_t step = 0; step < 32; ++step) {
            const CoreBgEntryV7& e = entries_[(idx + step) & mask_];
            if (e.key_hash == key) return &e;
            if (e.key_hash == 0) return nullptr;
        }
        return nullptr;
    }

    inline bool find_val(uint8_t dir, const uint32_t* wids, uint8_t cnt, CoreBgLookupResult& out) const {
        if (compact_entries_) {
            const uint64_t key = NgramTableV6::fnv1a_key_tagged(wids, cnt, static_cast<uint8_t>(0x80u | dir));
            const uint32_t b = static_cast<uint32_t>(key >> 48);
            const uint64_t klo = key & 0xFFFFFFFFFFFFULL;
            uint32_t lo = compact_buckets_[b];
            uint32_t hi = compact_buckets_[b + 1u];
            if (hi > compact_num_entries_) hi = compact_num_entries_;
            if (lo >= hi) return false;
            if (hi - lo <= 8u) {
                for (uint32_t i = lo; i < hi; ++i) {
                    const uint64_t ek = read_u48(compact_entries_[i].key_lo);
                    if (ek == klo) {
                        out.best_core_h = compact_entries_[i].best_core_h;
                        out.alt_core_h = compact_entries_[i].alt_core_h;
                        out.best_bonus = compact_entries_[i].best_bonus;
                        out.alt_bonus = compact_entries_[i].alt_bonus;
                        return true;
                    }
                    if (ek > klo) return false;
                }
                return false;
            }
            while (lo < hi) {
                uint32_t mid = lo + ((hi - lo) >> 1);
                const uint64_t ek = read_u48(compact_entries_[mid].key_lo);
                if (ek < klo) lo = mid + 1u;
                else if (ek > klo) hi = mid;
                else {
                    out.best_core_h = compact_entries_[mid].best_core_h;
                    out.alt_core_h = compact_entries_[mid].alt_core_h;
                    out.best_bonus = compact_entries_[mid].best_bonus;
                    out.alt_bonus = compact_entries_[mid].alt_bonus;
                    return true;
                }
            }
            return false;
        }
        const CoreBgEntryV7* e = find(dir, wids, cnt);
        if (!e) return false;
        out.best_core_h = e->best_core_h;
        out.alt_core_h = e->alt_core_h;
        out.best_bonus = e->best_bonus;
        out.alt_bonus = e->alt_bonus;
        return true;
    }

private:
    const CoreBgEntryV7* entries_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t mask_ = 0;
    const uint32_t* compact_buckets_ = nullptr;
    const CompactCoreBgEntry* compact_entries_ = nullptr;
    uint32_t compact_num_buckets_ = 0;
    uint32_t compact_num_entries_ = 0;
};

struct ValencyInfo {
    uint8_t v_trans = 0;         // 0=non-verb/unk, 1=intransitive, 2=transitive, 3=prep/clause-verb
    uint8_t next_case_pref = 0;  // 0=neutral, 1=DAMMA, 2=FATHA, 3=KASRA
    uint8_t word_syn_type = 0;   // 0=other, 1=mudaf-head, 2=adj/mod, 3=adverbial-acc
    uint8_t next2_case_pref = 0; // 0=neutral, 1=DAMMA, 2=FATHA, 3=KASRA
    int8_t val_odds = 0;         // -7..+7 (verbs: acc vs nom; nouns: gen_next vs non_gen)
    uint8_t tan_ratio = 0;       // 0..7
    uint8_t is_gram_fem = 0;     // 0..1
    uint8_t next_nom_p = 0;      // 0..15
    uint8_t next_acc_p = 0;      // 0..15
    uint8_t next_gen_p = 0;      // 0..15
    uint8_t verb_prob = 0;       // 0..15
};

inline ValencyInfo unpack_valency(uint32_t v) {
    ValencyInfo o;
    o.v_trans = static_cast<uint8_t>(v & 3u);
    o.next_case_pref = static_cast<uint8_t>((v >> 2) & 3u);
    o.word_syn_type = static_cast<uint8_t>((v >> 4) & 3u);
    o.next2_case_pref = static_cast<uint8_t>((v >> 6) & 3u);
    o.val_odds = static_cast<int8_t>(static_cast<int>((v >> 8) & 15u) - 7);
    o.tan_ratio = static_cast<uint8_t>((v >> 12) & 7u);
    o.is_gram_fem = static_cast<uint8_t>((v >> 15) & 1u);
    o.next_nom_p = static_cast<uint8_t>((v >> 16) & 15u);
    o.next_acc_p = static_cast<uint8_t>((v >> 20) & 15u);
    o.next_gen_p = static_cast<uint8_t>((v >> 24) & 15u);
    o.verb_prob = static_cast<uint8_t>((v >> 28) & 15u);
    return o;
}

class ValencyTableV7 {
public:
    void init_raw(const uint32_t* raw, uint32_t count) {
        raw_ = raw;
        count_ = count;
        bits_ = nullptr;
        super_ = nullptr;
        vals_ = nullptr;
        num_nonzero_ = 0;
    }

    bool init_compact(const uint8_t* blob, uint32_t byte_size) {
        raw_ = nullptr;
        count_ = 0;
        bits_ = nullptr;
        super_ = nullptr;
        vals_ = nullptr;
        num_nonzero_ = 0;
        if (!blob || byte_size < sizeof(CompactValencyHeader)) return false;
        const auto* h = reinterpret_cast<const CompactValencyHeader*>(blob);
        size_t off = (sizeof(CompactValencyHeader) + 7u) & ~7u;
        size_t bw_bytes = static_cast<size_t>(h->bitmap_words) * sizeof(uint64_t);
        size_t sb_bytes = ((static_cast<size_t>(h->super_blocks) * sizeof(uint32_t)) + 7u) & ~7u;
        size_t val_bytes = static_cast<size_t>(h->num_nonzero) * sizeof(uint32_t);
        if (off + bw_bytes + sb_bytes + val_bytes > byte_size) return false;
        if (static_cast<uint64_t>(h->bitmap_words) * 64u < h->num_words) return false;
        if (static_cast<uint64_t>(h->super_blocks) * 256u < h->num_words) return false;
        bits_ = reinterpret_cast<const uint64_t*>(blob + off);
        off += bw_bytes;
        super_ = reinterpret_cast<const uint32_t*>(blob + off);
        off += sb_bytes;
        vals_ = reinterpret_cast<const uint32_t*>(blob + off);
        count_ = h->num_words;
        num_nonzero_ = h->num_nonzero;
        return true;
    }

    bool valid() const { return count_ > 0 && (raw_ != nullptr || bits_ != nullptr); }
    uint32_t count() const { return count_; }

    inline uint32_t get_packed(uint32_t wid) const {
        if (wid >= count_) return 0u;
        if (raw_) return raw_[wid];
        if (!bits_) return 0u;
        if (((bits_[wid >> 6] >> (wid & 63u)) & 1ULL) == 0ULL) return 0u;
        uint32_t sb = wid >> 8;
        uint32_t r = super_[sb];
        uint32_t w_start = sb << 2;
        uint32_t w_end = wid >> 6;
        for (uint32_t w = w_start; w < w_end; ++w) r += portable_popcnt64(bits_[w]);
        uint32_t rem = wid & 63u;
        if (rem) r += portable_popcnt64(bits_[w_end] & ((1ULL << rem) - 1ULL));
        return (r < num_nonzero_) ? vals_[r] : 0u;
    }

    inline ValencyInfo get(uint32_t wid) const {
        if (!valid() || wid >= count_) return ValencyInfo{};
        return unpack_valency(get_packed(wid));
    }

private:
    const uint32_t* raw_ = nullptr;
    uint32_t count_ = 0;
    const uint64_t* bits_ = nullptr;
    const uint32_t* super_ = nullptr;
    const uint32_t* vals_ = nullptr;
    uint32_t num_nonzero_ = 0;
};

inline uint32_t pack_valency(const ValencyInfo& o) {
    uint32_t vo = static_cast<uint32_t>(o.val_odds + 7) & 15u;
    return (static_cast<uint32_t>(o.v_trans & 3u)) |
           (static_cast<uint32_t>(o.next_case_pref & 3u) << 2) |
           (static_cast<uint32_t>(o.word_syn_type & 3u) << 4) |
           (static_cast<uint32_t>(o.next2_case_pref & 3u) << 6) |
           (vo << 8) |
           (static_cast<uint32_t>(o.tan_ratio & 7u) << 12) |
           (static_cast<uint32_t>(o.is_gram_fem & 1u) << 15) |
           (static_cast<uint32_t>(o.next_nom_p & 15u) << 16) |
           (static_cast<uint32_t>(o.next_acc_p & 15u) << 20) |
           (static_cast<uint32_t>(o.next_gen_p & 15u) << 24) |
           (static_cast<uint32_t>(o.verb_prob & 15u) << 28);
}

inline uint16_t compute_core_hash(const uint8_t* syms, const uint8_t* diacs, uint8_t case_pos, uint8_t lam_skip) {
    uint32_t h = 2166136261u;
    for (uint8_t p = 0; p < case_pos; ++p) {
        if (p == lam_skip) continue;
        uint8_t d = diacs[p];
        if (syms[p] == 4u && d == 0u) d = 3u; // normalize إ kasra-vs-none so CoreBg ignores hamza register
        h ^= d;
        h *= 16777619u;
    }
    uint8_t end_sh = (diacs[case_pos] >= 8u) ? 1u : 0u;
    h ^= static_cast<uint8_t>((case_pos << 1) | end_sh);
    h *= 16777619u;
    uint16_t out = static_cast<uint16_t>((h ^ (h >> 16)) & 0xFFFFu);
    return out ? out : 1u;
}

}
