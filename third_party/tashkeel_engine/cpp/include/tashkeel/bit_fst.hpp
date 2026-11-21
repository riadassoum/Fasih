#pragma once

#include "format.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tashkeel {

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
            uint32_t offset = static_cast<uint32_t>(__builtin_popcountll(below));
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
            r += static_cast<uint32_t>(__builtin_popcountll(bits[w]));
        }
        uint32_t rem = idx & 63u;
        if (rem) {
            r += static_cast<uint32_t>(__builtin_popcountll(bits[w_end] & ((1ULL << rem) - 1ULL)));
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
            uint32_t offset = static_cast<uint32_t>(__builtin_popcountll(mask & (bit - 1ULL)));
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
            const uint32_t offset = static_cast<uint32_t>(__builtin_popcountll(below));
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
        const uint32_t offset = static_cast<uint32_t>(__builtin_popcount(end_mask & (bit - 1u)));
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
        const uint32_t offset = static_cast<uint32_t>(__builtin_popcount(end_mask & (bit - 1u)));
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

}
