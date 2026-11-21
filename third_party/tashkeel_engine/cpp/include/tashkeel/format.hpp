#pragma once

#include <cstddef>
#include <cstdint>

namespace tashkeel {

constexpr uint32_t TASH_MAGIC = 0x48534154;
constexpr uint16_t TASH_VERSION_2 = 2;
constexpr uint16_t TASH_VERSION_3 = 3;
constexpr uint16_t TASH_VERSION_4 = 4;
constexpr uint16_t TASH_VERSION_5 = 5;
constexpr uint16_t TASH_VERSION = TASH_VERSION_5;

enum DiacriticId : uint8_t {
    DIAC_NONE = 0,
    DIAC_FATHA = 1,
    DIAC_DAMMA = 2,
    DIAC_KASRA = 3,
    DIAC_SUKUN = 4,
    DIAC_FATHATAN = 5,
    DIAC_DAMMATAN = 6,
    DIAC_KASRATAN = 7,
    DIAC_SHADDA = 8,
    DIAC_SHADDA_FATHA = 9,
    DIAC_SHADDA_DAMMA = 10,
    DIAC_SHADDA_KASRA = 11,
    DIAC_SHADDA_FATHATAN = 12,
    DIAC_SHADDA_DAMMATAN = 13,
    DIAC_SHADDA_KASRATAN = 14,
    NUM_DIACRITICS = 15
};

enum MorphFlags : uint8_t {
    MORPH_NONE = 0,
    MORPH_HAS_AL = 1 << 0,
    MORPH_PREP = 1 << 1,
    MORPH_NASB = 1 << 2,
    MORPH_JAZM = 1 << 3,
    MORPH_INNA = 1 << 4,
    MORPH_KANA = 1 << 5,
    MORPH_CONJ = 1 << 6,
    MORPH_VERB_LIKE = 1 << 7
};

enum WeightId : uint32_t {
    W_LEX = 0,
    W_BIGRAM,
    W_TRANS,
    W_OOV_TREE,
    W_IRAB_TREE,
    W_RULE,
    W_UNARY_RULE,
    W_PROCLITIC_PEN,
    W_SUFFIX_PEN,
    W_RELAXED_PEN,
    W_EXPAND_PEN,
    W_CONSTRAINT,
    W_MORPH_TREE,
    NUM_WEIGHTS
};

constexpr int32_t DEFAULT_WEIGHTS[NUM_WEIGHTS] = {
    450, 950, 800, 1200, 1000, 750, 700, 200, 450, 250, 2400, 1000, 1000
};

constexpr size_t MAX_WORD_LEN = 48;
constexpr size_t OOV_NUM_FEATURES = 22;
constexpr size_t OOV_LEGACY_FEATURES = 16;
constexpr size_t IRAB_NUM_FEATURES = 56;
constexpr size_t IRAB_V5_BASE_FEATURES = 44;
constexpr size_t IRAB_LEGACY_FEATURES = 40;
constexpr size_t MORPH_NUM_FEATURES = 32;
constexpr size_t NUM_MORPH_CLASSES = 12;
constexpr size_t NUM_TRANS_ROLES_V5 = 24;
constexpr size_t BG_BLOCK_SHIFT_V5 = 5;

#pragma pack(push, 1)
struct FileHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;

    uint64_t fst_nodes_offset;
    uint32_t fst_nodes_count;

    uint64_t word_meta_offset;
    uint32_t word_meta_count;

    uint64_t candidates_offset;
    uint32_t candidates_count;

    uint64_t core_pool_offset;
    uint32_t core_pool_size;

    uint64_t bigram_offset;
    uint32_t bigram_capacity;

    uint64_t trans_matrix_offset;
    uint32_t trans_matrix_size;

    uint64_t oov_trees_offset;
    uint32_t oov_trees_size;

    uint64_t irab_trees_offset;
    uint32_t irab_trees_size;

    uint64_t weights_offset;
    uint32_t weights_count;
};

struct FileHeaderV3 {
    uint32_t magic;
    uint16_t version;
    uint16_t flags; // bit 0: compact OOV, bit 1: compact Irab, bit 2: int8 bigrams, bit 3: compact Morph

    uint64_t fst_nodes_offset;
    uint32_t fst_nodes_count;

    uint64_t word_meta_offset;
    uint32_t word_meta_count;

    uint64_t candidates_offset;
    uint32_t candidates_count;

    uint64_t core_pool_offset;
    uint32_t core_pool_size;

    uint64_t bg_row_off_offset;
    uint32_t bg_row_off_count;

    uint64_t bg_entries_offset;
    uint32_t bg_entries_count;

    uint64_t bg_vals_offset;
    uint32_t bg_vals_count;

    uint64_t trans_matrix_offset;
    uint32_t trans_matrix_size;

    uint64_t oov_trees_offset;
    uint32_t oov_trees_size;

    uint64_t irab_trees_offset;
    uint32_t irab_trees_size;

    uint64_t weights_offset;
    uint32_t weights_count;
};

struct FileHeaderV4 {
    FileHeaderV3 base;
    uint64_t morph_trees_offset;
    uint32_t morph_trees_size;
};

struct FileHeaderV5 {
    uint32_t magic;
    uint16_t version;
    uint16_t flags; // bit0: v5_trees_32b, bit1: v5_cands_5b, bit2: int8_bigrams, bit3: v5_24roles_trans, bit4: v5_irab_56f

    uint64_t fst_blob_offset;
    uint32_t fst_blob_size;

    uint64_t word_meta_offset;
    uint32_t word_meta_count;

    uint64_t candidates_offset;
    uint32_t candidates_count;

    uint64_t core_pool_offset;
    uint32_t core_pool_size;

    uint64_t bg_row_off_offset;
    uint32_t bg_row_off_count;

    uint64_t bg_block_base_offset;
    uint32_t bg_block_base_count;

    uint64_t bg_entries_offset;
    uint32_t bg_entries_count;

    uint64_t bg_vals_offset;
    uint32_t bg_vals_count;

    uint64_t trans_matrix_offset;
    uint32_t trans_matrix_size;

    uint64_t oov_trees_offset;
    uint32_t oov_trees_size;

    uint64_t irab_trees_offset;
    uint32_t irab_trees_size;

    uint64_t morph_trees_offset;
    uint32_t morph_trees_size;

    uint64_t weights_offset;
    uint32_t weights_count;
};

struct FstV5Header {
    uint32_t num_nodes;
    uint32_t num_internal;
    uint32_t num_words;
    uint32_t tails_bytes;
    uint32_t bitmap_words;
    uint32_t super_blocks;
};

struct BitTrieNode {
    uint64_t child_mask;
    uint32_t first_child;
    uint32_t value;
};

struct BitTrieNodeV3 {
    uint64_t lo; // [0..37]: 38-bit child_mask, [38..58]: 21-bit first_child, [59..63]: low 5 bits of value
    uint16_t hi; // upper 16 bits of 21-bit value (0x1FFFFF = non-terminal)
};

struct WordMeta {
    uint32_t cand_start;
    uint16_t cand_count;
    uint8_t word_len;
    uint8_t role_flags;
};

struct WordMetaV3 {
    uint8_t cand_start[3];
    uint8_t cand_count;
    uint8_t word_len;
    uint8_t role_flags;
    uint16_t bg_end_mask;
};

struct WordCandidate {
    uint32_t core_offset;
    int16_t log_prob;
    uint8_t ending_id;
    uint8_t flags;
};

struct WordCandidateV3 {
    uint8_t core_offset[3];
    int16_t log_prob;
    uint8_t ending_flags; // low 4 bits = ending_id, high 4 bits = flags
};

struct WordCandidateV5 {
    uint8_t packed[5]; // [0..19]: core_offset, [20..23]: ending_id, [24..27]: flags, [28..39]: log_prob_q (step 3)
};

struct BigramEntry {
    uint64_t key;
    int16_t bonus[NUM_DIACRITICS];
    uint16_t best_prev_mask;
};

struct BigramEntryV3 {
    uint8_t curr_id[3];
    uint32_t val_offset;
};

struct BigramEntryV5 {
    uint8_t curr_id[3];
    uint16_t local_val_off;
};

struct TreeEnsembleHeader {
    uint32_t num_trees;
    uint32_t num_classes;
    uint32_t num_nodes;
    int32_t base_score_q;
};

enum NodeKind : uint8_t {
    NODE_NUM = 0,
    NODE_LEAF = 1,
    NODE_CAT = 2
};

struct FlatTreeNode {
    uint64_t cat_mask;
    int32_t threshold;
    int32_t left_child;
    int32_t right_child;
    uint16_t feature_idx;
    uint8_t kind;
    uint8_t class_id;
};

struct FlatTreeNodeV3 {
    int32_t threshold;
    uint16_t left_child;
    uint16_t right_child;
    uint8_t feature_idx;
    uint8_t kind_class; // (kind << 4) | (class_id & 0x0F)
};
#pragma pack(pop)

inline uint32_t read_u24(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
}

}
