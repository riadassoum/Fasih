#pragma once

#include "arabic_grammar.hpp"
#include "arabic_utf8.hpp"
#include "bit_fst.hpp"
#include "format.hpp"
#include "tree_eval.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tashkeel {

inline uint8_t morph_to_role(uint8_t flags) {
    if (flags & MORPH_PREP) return 1;
    if (flags & MORPH_NASB) return 2;
    if (flags & MORPH_JAZM) return 3;
    if (flags & MORPH_INNA) return 4;
    if (flags & MORPH_KANA) return 5;
    if (flags & MORPH_CONJ) return 6;
    if (flags & MORPH_HAS_AL) return 7;
    if (flags & MORPH_VERB_LIKE) return 8;
    return 0;
}

enum CandFlags : uint8_t {
    CF_NONE = 0,
    CF_HAS_AL = 1 << 0,
    CF_GEN = 1 << 1,
    CF_FUT = 1 << 2,
    CF_LAM_NASB = 1 << 3,
    CF_SUFFIX = 1 << 4,
    CF_EXPANDED = 1 << 5,
    CF_ILTIQA = 1 << 6,
    CF_OOV = 1 << 7
};

enum ExtFlags : uint8_t {
    EF_NONE = 0,
    EF_PASSIVE = 1 << 0,
    EF_DIPTOTE = 1 << 1,
    EF_FEM_PL_AT = 1 << 2,
    EF_LAM_SUKUN = 1 << 3,
    EF_PAST_TEH_FEM = 1 << 4,
    EF_PLURAL_MEEM = 1 << 5,
    EF_DUAL_NOON = 1 << 6,
    EF_MASC_PL_NOON = 1 << 7
};

enum SurfFlags : uint16_t {
    SF_NONE = 0,
    SF_MAN = 1 << 0,
    SF_IBN = 1 << 1,
    SF_FIVE_NOUN = 1 << 2,
    SF_SHORT_PREP_PRON = 1 << 3,
    SF_BAAD_QABL = 1 << 4,
    SF_ALI = 1 << 5,
    SF_ALA = 1 << 6,
    SF_AM = 1 << 7,
    SF_THEMMA = 1 << 8,
    SF_ASHAR = 1 << 9,
    SF_COMP_NUM = 1 << 10,
    SF_NAME_HEAD = 1 << 11,
    SF_WASL = 1 << 12,
    SF_INNA_PRON = 1 << 13,
    SF_AMS = 1 << 14,
    SF_QUANT_IDAFA = 1 << 15
};

enum PosTag : uint8_t {
    POS_UNK = 0,
    POS_VERB_PAST_3SG = 1,
    POS_VERB_PAST_SUBJ = 2,
    POS_VERB_PAST_FEM = 3,
    POS_VERB_PASS = 4,
    POS_VERB_IMPERF_12 = 5,
    POS_VERB_IMPERF_3 = 6,
    POS_NOUN_AL = 7,
    POS_NOUN_INDEF = 8,
    POS_NOUN_SUFFIX = 9,
    POS_PARTICLE = 10
};

enum MorphClass : uint8_t {
    MC_VERB_PAST_ACT = 0,
    MC_VERB_PAST_PASS = 1,
    MC_VERB_IMPERF_ACT = 2,
    MC_VERB_IMPERF_PASS = 3,
    MC_NOUN_FATHA_START = 4,
    MC_NOUN_KASRA_START = 5,
    MC_NOUN_DAMMA_START = 6,
    MC_NOUN_SHADDA_CORE = 7,
    MC_VERB_SHADDA_ACT = 8,
    MC_PART_SHADDA = 9,
    MC_PART_SUKUN_LIGHT = 10,
    MC_OTHER = 11
};

class Diacritizer {
public:
    Diacritizer();
    ~Diacritizer();

    Diacritizer(const Diacritizer&) = delete;
    Diacritizer& operator=(const Diacritizer&) = delete;

    bool load(const std::string& model_path, std::string* err_msg = nullptr);
    void unload();
    bool is_loaded() const { return mapped_ptr_ != nullptr || !owned_buf_.empty(); }

    std::string diacritize(const char* utf8_text, size_t len) const;
    std::string diacritize(const std::string& text) const { return diacritize(text.data(), text.size()); }

    uint32_t vocab_size() const { return vocab_count_; }
    uint32_t fst_nodes_count() const { return fst_nodes_count_; }
    bool has_oov_trees() const { return oov_trees_.valid(); }
    bool has_irab_trees() const { return irab_trees_.valid(); }
    bool has_irab2_trees() const { return irab2_trees_.valid(); }
    bool has_morph_trees() const { return morph_trees_.valid(); }
    bool has_v7_valency() const { return valency_.valid(); }
    bool has_v7_trans3() const { return trans3_ != nullptr && trans3_size_ == TRANS3_TABLE_SIZE; }
    bool has_v7_core_bg() const { return !core_bg_.empty(); }

    int32_t weight(uint32_t id) const {
        if (id < NUM_WEIGHTS) return weights_[id];
        if (id >= 100 && id < 100 + NUM_V7_WEIGHTS) return v7_weights_[id - 100];
        return 0;
    }
    bool set_weight(uint32_t id, int32_t v) {
        if (id < NUM_WEIGHTS) {
            weights_[id] = v;
            ++cache_epoch_;
            return true;
        }
        if (id >= 100 && id < 100 + NUM_V7_WEIGHTS) {
            v7_weights_[id - 100] = v;
            ++cache_epoch_;
            return true;
        }
        return false;
    }
    void set_restore_spelling(bool v) { restore_spelling_ = v; ++cache_epoch_; }

    // Fast C++ helper for training Pass-2 Stacked Irab2 GBDT with 100% zero train/inference skew
    struct Irab2DumpSample {
        int32_t feat[IRAB2_NUM_FEATURES];
        uint8_t gold_end;
        uint8_t p1_end;
        uint8_t multi_end;
    };
    void dump_irab2_sentence(const char* gold_utf8, size_t len, std::vector<Irab2DumpSample>& out) const;
    // Fast FST/lexicon read helpers for external C++ V7 table extractor
    uint32_t lookup_word_id(const uint8_t* syms, size_t len) const { return fst_.lookup(syms, len); }
    bool word_has_multi_core(uint32_t wid, const uint8_t* syms, size_t len) const;
    void extract_word_top_info(uint32_t wid, const uint8_t* syms, size_t len,
                               uint8_t& out_case_pos, uint8_t& out_top_end, uint8_t& out_top_verb) const;
    void extract_word_core_priors(uint32_t wid, const uint8_t* syms, size_t len,
                                  uint16_t out_hashes[4], float out_probs[4], int& out_cnt) const;

private:
    struct Cand {
        uint8_t diac[MAX_WORD_LEN];
        int32_t emis;
        uint8_t case_pos;
        uint8_t flags;
        uint8_t ext_flags;
        uint8_t repl_pos;
        uint8_t repl_sym;
        uint8_t verb_pattern;
        uint8_t verb_mood_diac;
        uint8_t pos_tag;
        uint8_t morph_class;
    };

    enum BoundaryMask : uint8_t {
        BK_NONE = 0,
        BK_HARD = 1 << 0,
        BK_COMMA = 1 << 1,
        BK_OPEN = 1 << 2,
        BK_CLOSE = 1 << 3,
        BK_OTHER = 1 << 4
    };

    struct Token {
        uint32_t off = 0;
        uint8_t n = 0;
        uint32_t word_id = UINT32_MAX;
        uint8_t role = 0;
        uint8_t boundary_before = 1;
        uint8_t bkind_mask = BK_HARD;
        uint8_t lex_top_end = 255;
        uint8_t top_verb = 0;
        uint8_t has_noun_lex = 0;
        uint8_t is_fem_at = 0;
        uint8_t is_diptote_lex = 0;
        uint8_t is_ibn = 0;
        uint8_t is_adj_like = 0;
        uint8_t digit_after = 0;
        uint8_t num_after_class = 0;
        uint8_t top_pos = POS_UNK;
        uint16_t surf_flags = 0;
        SurfaceInfo si;
        uint32_t cand_begin = 0;
        uint32_t cand_count = 0;
        bool skip = false;
    };

    struct Ctx {
        std::vector<uint8_t> syms;
        std::vector<uint32_t> cps;
        std::vector<Token> toks;
        std::vector<Cand> cands;
    };

    uint32_t lookup_relaxed(const uint8_t* syms, size_t n, int& repl_pos, uint8_t& repl_sym) const;
    std::string diacritize_internal(const char* utf8_text, size_t len,
                                    std::vector<Irab2DumpSample>* dump_out,
                                    const std::vector<std::array<uint8_t, MAX_WORD_LEN>>* gold_diacs) const;
    bool has_stem_in_fst(const uint8_t* s, size_t stem_len) const;
    void emit_stem(Ctx& ctx, Token& tok, uint32_t stem_id, size_t pre_len, const uint8_t* pre_diac,
                   bool has_al, size_t suf_idx, int32_t penalty, uint8_t extra_flags,
                   int repl_pos, uint8_t repl_sym) const;
    bool analyze_morph(Ctx& ctx, Token& tok) const;
    void build_oov(Ctx& ctx, Token& tok) const;
    void expand_cases(Ctx& ctx, Token& tok) const;
    void build_candidates(Ctx& ctx, Token& tok) const;
    void finalize_cand(Cand& c, const uint8_t* syms, size_t n, const SurfaceInfo& si, uint8_t has_noun_lex) const;

    int32_t unary_rules(const Ctx& ctx, size_t idx, const Cand& c) const;
    int32_t binary_rules(const Ctx& ctx, const Token* p3, const Token* p2, const Token& pt, const Cand& pc,
                         const Token& ct, const Cand& cc, const Token* nx) const;
    uint8_t cand_role_v5(const Token& tok, const Cand& c) const;
    uint8_t cand_state3_v7(const Token& tok, const Cand& c, const uint8_t* ts) const;
    int32_t trans_score(uint8_t prev_role, uint8_t prev_end, uint8_t curr_role, uint8_t curr_end) const;
    inline int32_t trans3_score(uint8_t s2, uint8_t s1, uint8_t s0) const {
        if (!trans3_ || trans3_size_ != TRANS3_TABLE_SIZE ||
            s2 >= NUM_TRANS3_STATES || s1 >= NUM_TRANS3_STATES || s0 >= NUM_TRANS3_STATES) {
            return 0;
        }
        return static_cast<int32_t>(
            trans3_[(static_cast<size_t>(s2) * NUM_TRANS3_STATES + s1) * NUM_TRANS3_STATES + s0]) * 16;
    }
    inline ValencyInfo get_valency(uint32_t wid) const {
        return valency_.get(wid);
    }

    inline uint8_t meta_word_len(uint32_t wid) const {
        return (is_v3_ || is_v5_) ? word_meta_v3_[wid].word_len : word_meta_[wid].word_len;
    }
    inline uint8_t meta_role_flags(uint32_t wid) const {
        return (is_v3_ || is_v5_) ? word_meta_v3_[wid].role_flags : word_meta_[wid].role_flags;
    }
    inline uint32_t meta_cand_start(uint32_t wid) const {
        return (is_v3_ || is_v5_) ? read_u24(word_meta_v3_[wid].cand_start) : word_meta_[wid].cand_start;
    }
    inline uint16_t meta_cand_count(uint32_t wid) const {
        return (is_v3_ || is_v5_) ? word_meta_v3_[wid].cand_count : word_meta_[wid].cand_count;
    }
    inline uint16_t meta_bg_mask(uint32_t wid) const {
        return (is_v3_ || is_v5_) ? word_meta_v3_[wid].bg_end_mask : 0x7FFF;
    }
    inline WordCandidate read_cand(uint32_t idx) const {
        if (is_v5_ && candidates_v5_) {
            uint64_t v = 0;
            std::memcpy(&v, candidates_v5_[idx].packed, 5);
            uint32_t co = static_cast<uint32_t>(v & 0xFFFFFu);
            uint8_t eid = static_cast<uint8_t>((v >> 20) & 0xFu);
            uint8_t fl = static_cast<uint8_t>((v >> 24) & 0xFu);
            uint32_t lpq = static_cast<uint32_t>((v >> 28) & 0xFFFu);
            int16_t lp = static_cast<int16_t>(-static_cast<int32_t>(lpq) * 3);
            return {co, lp, eid, fl};
        }
        if (is_v3_ || is_v5_) {
            const WordCandidateV3& c = candidates_v3_[idx];
            return {read_u24(c.core_offset), c.log_prob,
                    static_cast<uint8_t>(c.ending_flags & 0x0Fu),
                    static_cast<uint8_t>(c.ending_flags >> 4)};
        }
        return candidates_[idx];
    }
    inline uint8_t read_core_diac(uint32_t core_off, size_t k) const {
        if (!is_v3_ && !is_v5_) return core_pool_[core_off + k];
        uint32_t nib = core_off + static_cast<uint32_t>(k);
        uint8_t b = core_pool_[nib >> 1];
        return (nib & 1u) ? static_cast<uint8_t>(b >> 4) : static_cast<uint8_t>(b & 0x0Fu);
    }

    void* mapped_ptr_ = nullptr;
    size_t mapped_size_ = 0;
    std::vector<uint8_t> owned_buf_;
#ifdef _WIN32
    void* win_file_ = nullptr;
    void* win_map_ = nullptr;
#endif
    bool is_v3_ = false;
    bool is_v5_ = false;
    bool is_v7_ = false;
    bool has_v5_24roles_ = false;
    uint64_t cache_epoch_ = 1;
    uint32_t vocab_count_ = 0;
    uint32_t fst_nodes_count_ = 0;
    BitmappedFST fst_;
    const WordMeta* word_meta_ = nullptr;
    const WordMetaV3* word_meta_v3_ = nullptr;
    const WordCandidate* candidates_ = nullptr;
    const WordCandidateV3* candidates_v3_ = nullptr;
    const WordCandidateV5* candidates_v5_ = nullptr;
    const uint8_t* core_pool_ = nullptr;
    BigramTable bigrams_;
    NgramTableV6 ngrams_;
    CoreBgTableV7 core_bg_;
    ValencyTableV7 valency_;
    const int8_t* trans3_ = nullptr;
    uint32_t trans3_size_ = 0;
    const int16_t* trans_matrix_ = nullptr;
    uint32_t trans_matrix_size_ = 0;
    FlatTreeEnsemble oov_trees_;
    FlatTreeEnsemble irab_trees_;
    FlatTreeEnsemble irab2_trees_;
    FlatTreeEnsemble morph_trees_;
    uint8_t oov_feat_count_ = 16;
    uint8_t irab_feat_count_ = 40;
    uint8_t irab2_feat_count_ = IRAB2_NUM_FEATURES;
    int32_t weights_[NUM_WEIGHTS];
    int32_t v7_weights_[NUM_V7_WEIGHTS];
    bool restore_spelling_ = true;
};

}
