#pragma once

#include "bit_fst.hpp"
#include "format.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace tashkeel {

class FlatTreeEnsemble {
public:
    FlatTreeEnsemble() = default;

    struct FastNode {
        uint16_t right_delta; // 0 => leaf; >= 2 => right child offset (left child is +1)
        uint8_t feat;         // internal: feature index 0..127; leaf: byte 0 of int16_t leaf_val
        int8_t thresh;        // internal: threshold -8..119; leaf: byte 1 of int16_t leaf_val
    };
    static_assert(sizeof(FastNode) == 4, "FastNode must be 4 bytes");
    static_assert(offsetof(FastNode, feat) == 2, "FastNode::feat must be at offset 2");
    static_assert(offsetof(FastNode, thresh) == 3, "FastNode::thresh must be at offset 3");

    struct Split2 {
        uint8_t feat;
        int8_t thresh;
    };
    static_assert(sizeof(Split2) == 2, "Split2 must be 2 bytes");

    // 1-based pure complete-BFS trees: sp[1..(2^D-1)] (sp[0] unused), leaf[0..(2^D-1)].
    // Early leaves use dummy split {0, 127}: for any int8_t x in [-128..127], (x > 127) is ALWAYS false!
    struct alignas(64) Bfs6One {
        Split2 sp[64];
        int16_t leaf[64];
    };
    static_assert(sizeof(Bfs6One) == 256, "Bfs6One must be 256 bytes");

    struct alignas(64) Bfs7One {
        Split2 sp[128];
        int16_t leaf[128];
    };
    static_assert(sizeof(Bfs7One) == 512, "Bfs7One must be 512 bytes");

    struct alignas(64) Bfs8One {
        Split2 sp[256];
        int16_t leaf[256];
    };
    static_assert(sizeof(Bfs8One) == 1024, "Bfs8One must be 1024 bytes");

    // Depth-9 Split Top5 (64B = 1 cache line) + 32 x Sub4 (each 64B = 1 cache line):
    // Eliminates 5-cache-line Phase-2 scatter & 2048B power-of-2 L1d cache-set aliasing!
    struct alignas(64) Bfs9Top5 {
        Split2 sp[32];
    };
    static_assert(sizeof(Bfs9Top5) == 64, "Bfs9Top5 must be 64 bytes");

    struct alignas(64) Bfs9Sub4 {
        Split2 sp[16];
        int16_t leaf[16];
    };
    static_assert(sizeof(Bfs9Sub4) == 64, "Bfs9Sub4 must be 64 bytes");

    // Depth > 9 fallback hybrid BFS-7 + FastNode tails
    struct alignas(8) Bfs7Hybrid {
        Split2 sp[127];
        uint16_t pad;
        uint32_t tail_base;
        uint32_t pad2;
        uint64_t lm[2];
        int16_t slot[128];
    };

    bool init(const uint8_t* data, uint32_t byte_size, uint8_t mode = 0) {
        // mode: 0=v2 FlatTreeNode, 1=v3 FlatTreeNodeV3, 2=v5 packed uint32_t
        valid_ = false;
        fast_kind_ = 0;
        used_feats_ = 128;
        bfs6_trees_.clear();
        bfs7_one_trees_.clear();
        bfs8_trees_.clear();
        bfs9_top5_.clear();
        bfs9_sub4_.clear();
        bfs7_trees_.clear();
        fast_nodes_.clear();
        tree_span_.clear();
        for (auto& v : class_tree_off_) v = 0;
        mode_ = mode;
        if (!data || byte_size < sizeof(TreeEnsembleHeader)) return false;
        header_ = reinterpret_cast<const TreeEnsembleHeader*>(data);
        size_t node_sz = (mode == 2) ? sizeof(uint32_t)
                       : ((mode == 1) ? sizeof(FlatTreeNodeV3) : sizeof(FlatTreeNode));
        size_t expected = sizeof(TreeEnsembleHeader) +
                          static_cast<size_t>(header_->num_trees) * sizeof(uint32_t) +
                          static_cast<size_t>(header_->num_nodes) * node_sz;
        if (byte_size < expected || header_->num_classes == 0 || header_->num_classes > NUM_DIACRITICS) return false;
        tree_roots_ = reinterpret_cast<const uint32_t*>(data + sizeof(TreeEnsembleHeader));
        const uint8_t* np = data + sizeof(TreeEnsembleHeader) + static_cast<size_t>(header_->num_trees) * sizeof(uint32_t);
        nodes_v2_ = nullptr;
        nodes_v3_ = nullptr;
        nodes_v5_ = nullptr;
        if (mode == 2) {
            nodes_v5_ = reinterpret_cast<const uint32_t*>(np);
            build_fast_v5();
        } else if (mode == 1) {
            nodes_v3_ = reinterpret_cast<const FlatTreeNodeV3*>(np);
        } else {
            nodes_v2_ = reinterpret_cast<const FlatTreeNode*>(np);
        }
        valid_ = true;
        return true;
    }

    bool valid() const { return valid_; }
    bool has_fast_v5() const { return fast_kind_ != 0; }
    uint32_t num_classes() const { return valid_ ? header_->num_classes : 0; }
    int32_t base_score_q() const { return valid_ ? header_->base_score_q : 0; }
    uint32_t used_feats() const { return used_feats_; }

    static inline void pack_features_i8_128(const int32_t* __restrict features, size_t num_features, int8_t f8[128]) {
        const size_t n = num_features < 128 ? num_features : 128;
        for (size_t i = 0; i < n; ++i) {
            int32_t v = features[i];
            f8[i] = static_cast<int8_t>(v < -8 ? -8 : (v > 120 ? 120 : v));
        }
        if (n < 128) std::memset(f8 + n, 0xFF, 128 - n);
    }

    static inline void pack_features_i8(const int32_t* __restrict features, size_t num_features, int8_t* __restrict f8) {
        const size_t n = num_features < 128 ? num_features : 128;
        for (size_t i = 0; i < n; ++i) {
            int32_t v = features[i];
            f8[i] = static_cast<int8_t>(v < -8 ? -8 : (v > 120 ? 120 : v));
        }
        if (n < 128) std::memset(f8 + n, 0xFF, 128 - n);
    }

    inline void predict_scores_masked_i8(const int8_t* __restrict f8, uint32_t class_mask, int32_t* __restrict out_scores) const {
        if (!valid_ || fast_kind_ == 0) return;
        const uint32_t nc = header_->num_classes;
        const int32_t bsq = header_->base_score_q;
        uint32_t m = class_mask & ((1u << nc) - 1u);
        switch (fast_kind_) {
            case 1:
                while (m) {
                    const uint32_t c = portable_ctz32(m); m &= (m - 1u);
                    out_scores[c] = bsq + eval_bfs6_range(f8, class_tree_off_[c], class_tree_off_[c + 1u]);
                }
                break;
            case 2:
                while (m) {
                    const uint32_t c = portable_ctz32(m); m &= (m - 1u);
                    out_scores[c] = bsq + eval_bfs7_one_range(f8, class_tree_off_[c], class_tree_off_[c + 1u]);
                }
                break;
            case 3:
                while (m) {
                    const uint32_t c = portable_ctz32(m); m &= (m - 1u);
                    out_scores[c] = bsq + eval_bfs8_range(f8, class_tree_off_[c], class_tree_off_[c + 1u]);
                }
                break;
            case 4:
                while (m) {
                    const uint32_t c = portable_ctz32(m); m &= (m - 1u);
                    out_scores[c] = bsq + eval_bfs9_range(f8, class_tree_off_[c], class_tree_off_[c + 1u]);
                }
                break;
            default:
                while (m) {
                    const uint32_t c = portable_ctz32(m); m &= (m - 1u);
                    out_scores[c] = bsq + eval_bfs7_range(f8, class_tree_off_[c], class_tree_off_[c + 1u]);
                }
                break;
        }
    }

    inline void predict_scores_i8(const int8_t* __restrict f8, int32_t* __restrict out_scores) const {
        if (!valid_ || fast_kind_ == 0) return;
        const uint32_t nc = header_->num_classes;
        predict_scores_masked_i8(f8, (1u << nc) - 1u, out_scores);
    }

    // L1-cache-tiled & AVX2/SSE2/NEON sentence-batch binary evaluation (out_logits[v] = sc[1] - sc[0]) for Core2.
    template <size_t F8_STRIDE>
    inline bool predict_batch_binary_i8(const std::array<int8_t, F8_STRIDE>* f8_batch, size_t n_vecs, int32_t* __restrict out_logits) const {
        if (!valid_ || fast_kind_ == 0 || header_->num_classes < 2 || n_vecs == 0) return false;
        for (size_t v = 0; v < n_vecs; ++v) out_logits[v] = 0;
        const uint32_t tb1 = class_tree_off_[1];
        const uint32_t te1 = class_tree_off_[2];
        const uint32_t tb0 = class_tree_off_[0];
        const uint32_t te0 = class_tree_off_[1];
        if (tb1 < te1) eval_batch_range_add<F8_STRIDE, +1>(f8_batch, nullptr, n_vecs, tb1, te1, out_logits, 1, 0);
        if (tb0 < te0) eval_batch_range_add<F8_STRIDE, -1>(f8_batch, nullptr, n_vecs, tb0, te0, out_logits, 1, 0);
        return true;
    }

    // Provably bit-exact branch-and-bound early pruning across 64-tree chunks:
    // Any slice where (best_other - p1_v + rem_span) * w_c2 <= (c2_mar - 1) * 1000 can NEVER flip Pass-1.
    template <size_t F8_STRIDE, typename SliceT>
    inline bool predict_batch_binary_pruned_i8(const std::array<int8_t, F8_STRIDE>* f8_batch, size_t n_vecs,
                                               const SliceT* slices, size_t n_slices,
                                               int32_t w_c2, int32_t c2_mar,
                                               std::vector<uint32_t>& active_vecs,
                                               std::vector<uint32_t>& active_slices,
                                               int32_t* __restrict out_logits) const {
        if (!valid_ || fast_kind_ == 0 || header_->num_classes < 2 || n_vecs == 0) return false;
        if (w_c2 <= 0 || n_slices == 0 || tree_span_.size() != header_->num_trees) {
            return predict_batch_binary_i8(f8_batch, n_vecs, out_logits);
        }
        for (size_t v = 0; v < n_vecs; ++v) out_logits[v] = 0;
        const uint32_t tb0 = class_tree_off_[0], te0 = class_tree_off_[1];
        const uint32_t tb1 = class_tree_off_[1], te1 = class_tree_off_[2];

        int64_t rem_span = 0;
        for (uint32_t t = tb0; t < te0; ++t) rem_span += tree_span_[t];
        for (uint32_t t = tb1; t < te1; ++t) rem_span += tree_span_[t];

        active_slices.resize(n_slices);
        for (size_t s = 0; s < n_slices; ++s) active_slices[s] = static_cast<uint32_t>(s);
        bool any_pruned = false;
        const int64_t prune_rhs = (static_cast<int64_t>(c2_mar) - 1LL) * 1000LL;
        const int64_t w64 = static_cast<int64_t>(w_c2);

        auto run_range = [&](uint32_t tb, uint32_t te, bool sign_pos, bool has_more_ranges) {
            for (uint32_t cs = tb; cs < te && !active_slices.empty(); ) {
                const uint32_t step = (cs == tb && !any_pruned) ? 160u : 64u;
                const uint32_t ce = (cs + step < te) ? (cs + step) : te;
                const uint32_t* idx_ptr = any_pruned ? active_vecs.data() : nullptr;
                const size_t cnt = any_pruned ? active_vecs.size() : n_vecs;
                if (cnt == 0) break;
                if (sign_pos) eval_batch_range_add<F8_STRIDE, +1>(f8_batch, idx_ptr, cnt, cs, ce, out_logits, 1, 0);
                else          eval_batch_range_add<F8_STRIDE, -1>(f8_batch, idx_ptr, cnt, cs, ce, out_logits, 1, 0);

                for (uint32_t t = cs; t < ce; ++t) rem_span -= tree_span_[t];
                cs = ce;
                const bool more_trees = (ce < te) || has_more_ranges;
                if (!more_trees) continue;

                size_t keep = 0;
                for (size_t ai = 0; ai < active_slices.size(); ++ai) {
                    const uint32_t si = active_slices[ai];
                    const SliceT& sl = slices[si];
                    const int32_t p1_v = out_logits[sl.vec_off + static_cast<uint32_t>(sl.p1_g)];
                    int32_t best_other = INT32_MIN / 4;
                    for (int g = 0; g < sl.n_grp; ++g) {
                        if (g == sl.p1_g) continue;
                        const int32_t v = out_logits[sl.vec_off + static_cast<uint32_t>(g)];
                        if (v > best_other) best_other = v;
                    }
                    const int64_t max_final_diff = static_cast<int64_t>(best_other) - static_cast<int64_t>(p1_v) + rem_span;
                    if (max_final_diff * w64 <= prune_rhs) {
                        for (int g = 0; g < sl.n_grp; ++g) {
                            if (g != sl.p1_g) out_logits[sl.vec_off + static_cast<uint32_t>(g)] = INT32_MIN / 4;
                        }
                    } else {
                        active_slices[keep++] = si;
                    }
                }
                if (keep < active_slices.size()) {
                    active_slices.resize(keep);
                    any_pruned = true;
                    active_vecs.clear();
                    for (uint32_t si : active_slices) {
                        const SliceT& sl = slices[si];
                        for (int g = 0; g < sl.n_grp; ++g) {
                            active_vecs.push_back(sl.vec_off + static_cast<uint32_t>(g));
                        }
                    }
                }
            }
        };

        if (tb1 < te1) run_range(tb1, te1, true, tb0 < te0);
        if (tb0 < te0 && !active_slices.empty()) run_range(tb0, te0, false, false);
        return true;
    }

    // L1-cache-tiled & AVX2/SSE2/NEON sentence-batch single-class evaluation for Irab2 / Irab1 / Morph!
    template <size_t F8_STRIDE>
    inline void eval_class_batch_i8(uint32_t c, const std::array<int8_t, F8_STRIDE>* f8_buf,
                                    const uint32_t* word_indices, size_t count,
                                    int32_t* __restrict out_scores_flat, size_t out_stride) const {
        if (!valid_ || fast_kind_ == 0 || c >= header_->num_classes || count == 0) return;
        const int32_t bsq = header_->base_score_q;
        for (size_t j = 0; j < count; ++j) {
            out_scores_flat[word_indices[j] * out_stride + c] = bsq;
        }
        const uint32_t tb = class_tree_off_[c];
        const uint32_t te = class_tree_off_[c + 1u];
        if (tb < te) {
            eval_batch_range_add<F8_STRIDE, +1>(f8_buf, word_indices, count, tb, te, out_scores_flat, out_stride, c);
        }
    }

    inline void predict_scores_masked(const int32_t* features, size_t num_features,
                                      uint32_t class_mask, int32_t* out_scores) const {
        if (!valid_) return;
        if (fast_kind_ != 0) {
            alignas(64) int8_t f8[128];
            pack_features_i8_128(features, num_features, f8);
            predict_scores_masked_i8(f8, class_mask, out_scores);
            return;
        }
        predict_scores_fallback(features, num_features, out_scores);
    }

    inline void predict_scores(const int32_t* features, size_t num_features, int32_t* out_scores) const {
        if (!valid_) return;
        if (fast_kind_ != 0) {
            alignas(64) int8_t f8[128];
            pack_features_i8_128(features, num_features, f8);
            predict_scores_i8(f8, out_scores);
            return;
        }
        predict_scores_fallback(features, num_features, out_scores);
    }

private:
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__) || defined(__AVX2__))
    static inline bool cpu_has_avx2() {
#if defined(__AVX2__)
        return true;
#else
        static const bool has = []() {
            uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
            __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0u), "c"(0u));
            if (eax < 7u) return false;
            __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u), "c"(0u));
            if ((ecx & (1u << 27)) == 0u || (ecx & (1u << 28)) == 0u) return false; // OSXSAVE + AVX
            uint32_t xlo = 0, xhi = 0;
            __asm__ __volatile__(".byte 0x0f, 0x01, 0xd0" : "=a"(xlo), "=d"(xhi) : "c"(0u)); // xgetbv
            if ((xlo & 0x6u) != 0x6u) return false; // OS saves XMM+YMM state
            __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7u), "c"(0u));
            return (ebx & (1u << 5)) != 0u; // AVX2
        }();
        return has;
#endif
    }
#endif

    static inline int32_t read_leaf(const FastNode* p) {
        int16_t v;
        std::memcpy(&v, &p->feat, 2);
        return static_cast<int32_t>(v);
    }

    static inline FastNode make_leaf(int16_t val) {
        FastNode fn{};
        fn.right_delta = 0;
        std::memcpy(&fn.feat, &val, 2);
        return fn;
    }

    // Generic 1-based pure-BFS 8-way unrolled scalar range evaluator for depth D in {6, 7, 8, 9}
    template <typename TreeT, int DEPTH, uint32_t LEAF_OFF>
    static inline int32_t eval_bfs_one_range(const TreeT* __restrict tops, const int8_t* __restrict f8, uint32_t tb, uint32_t te) {
        int32_t acc = 0;
        uint32_t t = tb;
        for (; t + 7u < te; t += 8u) {
            const TreeT& b0 = tops[t];      const TreeT& b1 = tops[t + 1u];
            const TreeT& b2 = tops[t + 2u]; const TreeT& b3 = tops[t + 3u];
            const TreeT& b4 = tops[t + 4u]; const TreeT& b5 = tops[t + 5u];
            const TreeT& b6 = tops[t + 6u]; const TreeT& b7 = tops[t + 7u];
            uint32_t i0 = 2u + static_cast<uint32_t>(f8[b0.sp[1].feat] > b0.sp[1].thresh);
            uint32_t i1 = 2u + static_cast<uint32_t>(f8[b1.sp[1].feat] > b1.sp[1].thresh);
            uint32_t i2 = 2u + static_cast<uint32_t>(f8[b2.sp[1].feat] > b2.sp[1].thresh);
            uint32_t i3 = 2u + static_cast<uint32_t>(f8[b3.sp[1].feat] > b3.sp[1].thresh);
            uint32_t i4 = 2u + static_cast<uint32_t>(f8[b4.sp[1].feat] > b4.sp[1].thresh);
            uint32_t i5 = 2u + static_cast<uint32_t>(f8[b5.sp[1].feat] > b5.sp[1].thresh);
            uint32_t i6 = 2u + static_cast<uint32_t>(f8[b6.sp[1].feat] > b6.sp[1].thresh);
            uint32_t i7 = 2u + static_cast<uint32_t>(f8[b7.sp[1].feat] > b7.sp[1].thresh);
            for (int d = 1; d < DEPTH; ++d) {
                const Split2 s0 = b0.sp[i0], s1 = b1.sp[i1], s2 = b2.sp[i2], s3 = b3.sp[i3];
                const Split2 s4 = b4.sp[i4], s5 = b5.sp[i5], s6 = b6.sp[i6], s7 = b7.sp[i7];
                i0 = (i0 << 1) + static_cast<uint32_t>(f8[s0.feat] > s0.thresh);
                i1 = (i1 << 1) + static_cast<uint32_t>(f8[s1.feat] > s1.thresh);
                i2 = (i2 << 1) + static_cast<uint32_t>(f8[s2.feat] > s2.thresh);
                i3 = (i3 << 1) + static_cast<uint32_t>(f8[s3.feat] > s3.thresh);
                i4 = (i4 << 1) + static_cast<uint32_t>(f8[s4.feat] > s4.thresh);
                i5 = (i5 << 1) + static_cast<uint32_t>(f8[s5.feat] > s5.thresh);
                i6 = (i6 << 1) + static_cast<uint32_t>(f8[s6.feat] > s6.thresh);
                i7 = (i7 << 1) + static_cast<uint32_t>(f8[s7.feat] > s7.thresh);
            }
            acc += b0.leaf[i0 - LEAF_OFF] + b1.leaf[i1 - LEAF_OFF] + b2.leaf[i2 - LEAF_OFF] + b3.leaf[i3 - LEAF_OFF] +
                   b4.leaf[i4 - LEAF_OFF] + b5.leaf[i5 - LEAF_OFF] + b6.leaf[i6 - LEAF_OFF] + b7.leaf[i7 - LEAF_OFF];
        }
        for (; t + 3u < te; t += 4u) {
            const TreeT& b0 = tops[t];      const TreeT& b1 = tops[t + 1u];
            const TreeT& b2 = tops[t + 2u]; const TreeT& b3 = tops[t + 3u];
            uint32_t i0 = 2u + static_cast<uint32_t>(f8[b0.sp[1].feat] > b0.sp[1].thresh);
            uint32_t i1 = 2u + static_cast<uint32_t>(f8[b1.sp[1].feat] > b1.sp[1].thresh);
            uint32_t i2 = 2u + static_cast<uint32_t>(f8[b2.sp[1].feat] > b2.sp[1].thresh);
            uint32_t i3 = 2u + static_cast<uint32_t>(f8[b3.sp[1].feat] > b3.sp[1].thresh);
            for (int d = 1; d < DEPTH; ++d) {
                const Split2 s0 = b0.sp[i0], s1 = b1.sp[i1], s2 = b2.sp[i2], s3 = b3.sp[i3];
                i0 = (i0 << 1) + static_cast<uint32_t>(f8[s0.feat] > s0.thresh);
                i1 = (i1 << 1) + static_cast<uint32_t>(f8[s1.feat] > s1.thresh);
                i2 = (i2 << 1) + static_cast<uint32_t>(f8[s2.feat] > s2.thresh);
                i3 = (i3 << 1) + static_cast<uint32_t>(f8[s3.feat] > s3.thresh);
            }
            acc += b0.leaf[i0 - LEAF_OFF] + b1.leaf[i1 - LEAF_OFF] + b2.leaf[i2 - LEAF_OFF] + b3.leaf[i3 - LEAF_OFF];
        }
        for (; t < te; ++t) {
            const TreeT& bt = tops[t];
            uint32_t idx = 2u + static_cast<uint32_t>(f8[bt.sp[1].feat] > bt.sp[1].thresh);
            for (int d = 1; d < DEPTH; ++d) {
                const Split2 s = bt.sp[idx];
                idx = (idx << 1) + static_cast<uint32_t>(f8[s.feat] > s.thresh);
            }
            acc += bt.leaf[idx - LEAF_OFF];
        }
        return acc;
    }

    inline int32_t eval_bfs6_range(const int8_t* __restrict f8, uint32_t tb, uint32_t te) const {
        return eval_bfs_one_range<Bfs6One, 6, 64u>(bfs6_trees_.data(), f8, tb, te);
    }
    inline int32_t eval_bfs7_one_range(const int8_t* __restrict f8, uint32_t tb, uint32_t te) const {
        return eval_bfs_one_range<Bfs7One, 7, 128u>(bfs7_one_trees_.data(), f8, tb, te);
    }
    inline int32_t eval_bfs8_range(const int8_t* __restrict f8, uint32_t tb, uint32_t te) const {
        return eval_bfs_one_range<Bfs8One, 8, 256u>(bfs8_trees_.data(), f8, tb, te);
    }
    // Split Top5 + Sub4 8-way unrolled scalar evaluator for depth-9 trees (2 cache lines per tree: Top5 + 1 Sub4!)
    static inline int32_t eval_bfs9_sub4_range(const Bfs9Top5* __restrict tps, const Bfs9Sub4* __restrict sbs,
                                               const int8_t* __restrict f8, uint32_t tb, uint32_t te) {
        int32_t acc = 0;
        uint32_t t = tb;
        for (; t + 7u < te; t += 8u) {
            const Split2* sp0 = tps[t].sp;      const Split2* sp1 = tps[t + 1u].sp;
            const Split2* sp2 = tps[t + 2u].sp; const Split2* sp3 = tps[t + 3u].sp;
            const Split2* sp4 = tps[t + 4u].sp; const Split2* sp5 = tps[t + 5u].sp;
            const Split2* sp6 = tps[t + 6u].sp; const Split2* sp7 = tps[t + 7u].sp;
            uint32_t i0 = 2u + static_cast<uint32_t>(f8[sp0[1].feat] > sp0[1].thresh);
            uint32_t i1 = 2u + static_cast<uint32_t>(f8[sp1[1].feat] > sp1[1].thresh);
            uint32_t i2 = 2u + static_cast<uint32_t>(f8[sp2[1].feat] > sp2[1].thresh);
            uint32_t i3 = 2u + static_cast<uint32_t>(f8[sp3[1].feat] > sp3[1].thresh);
            uint32_t i4 = 2u + static_cast<uint32_t>(f8[sp4[1].feat] > sp4[1].thresh);
            uint32_t i5 = 2u + static_cast<uint32_t>(f8[sp5[1].feat] > sp5[1].thresh);
            uint32_t i6 = 2u + static_cast<uint32_t>(f8[sp6[1].feat] > sp6[1].thresh);
            uint32_t i7 = 2u + static_cast<uint32_t>(f8[sp7[1].feat] > sp7[1].thresh);
            for (int d = 1; d < 5; ++d) {
                i0 = (i0 << 1) + static_cast<uint32_t>(f8[sp0[i0].feat] > sp0[i0].thresh);
                i1 = (i1 << 1) + static_cast<uint32_t>(f8[sp1[i1].feat] > sp1[i1].thresh);
                i2 = (i2 << 1) + static_cast<uint32_t>(f8[sp2[i2].feat] > sp2[i2].thresh);
                i3 = (i3 << 1) + static_cast<uint32_t>(f8[sp3[i3].feat] > sp3[i3].thresh);
                i4 = (i4 << 1) + static_cast<uint32_t>(f8[sp4[i4].feat] > sp4[i4].thresh);
                i5 = (i5 << 1) + static_cast<uint32_t>(f8[sp5[i5].feat] > sp5[i5].thresh);
                i6 = (i6 << 1) + static_cast<uint32_t>(f8[sp6[i6].feat] > sp6[i6].thresh);
                i7 = (i7 << 1) + static_cast<uint32_t>(f8[sp7[i7].feat] > sp7[i7].thresh);
            }
            const Bfs9Sub4& b0 = sbs[static_cast<size_t>(t)      * 32u + (i0 - 32u)];
            const Bfs9Sub4& b1 = sbs[static_cast<size_t>(t + 1u) * 32u + (i1 - 32u)];
            const Bfs9Sub4& b2 = sbs[static_cast<size_t>(t + 2u) * 32u + (i2 - 32u)];
            const Bfs9Sub4& b3 = sbs[static_cast<size_t>(t + 3u) * 32u + (i3 - 32u)];
            const Bfs9Sub4& b4 = sbs[static_cast<size_t>(t + 4u) * 32u + (i4 - 32u)];
            const Bfs9Sub4& b5 = sbs[static_cast<size_t>(t + 5u) * 32u + (i5 - 32u)];
            const Bfs9Sub4& b6 = sbs[static_cast<size_t>(t + 6u) * 32u + (i6 - 32u)];
            const Bfs9Sub4& b7 = sbs[static_cast<size_t>(t + 7u) * 32u + (i7 - 32u)];
            uint32_t k0 = 2u + static_cast<uint32_t>(f8[b0.sp[1].feat] > b0.sp[1].thresh);
            uint32_t k1 = 2u + static_cast<uint32_t>(f8[b1.sp[1].feat] > b1.sp[1].thresh);
            uint32_t k2 = 2u + static_cast<uint32_t>(f8[b2.sp[1].feat] > b2.sp[1].thresh);
            uint32_t k3 = 2u + static_cast<uint32_t>(f8[b3.sp[1].feat] > b3.sp[1].thresh);
            uint32_t k4 = 2u + static_cast<uint32_t>(f8[b4.sp[1].feat] > b4.sp[1].thresh);
            uint32_t k5 = 2u + static_cast<uint32_t>(f8[b5.sp[1].feat] > b5.sp[1].thresh);
            uint32_t k6 = 2u + static_cast<uint32_t>(f8[b6.sp[1].feat] > b6.sp[1].thresh);
            uint32_t k7 = 2u + static_cast<uint32_t>(f8[b7.sp[1].feat] > b7.sp[1].thresh);
            for (int d = 1; d < 4; ++d) {
                k0 = (k0 << 1) + static_cast<uint32_t>(f8[b0.sp[k0].feat] > b0.sp[k0].thresh);
                k1 = (k1 << 1) + static_cast<uint32_t>(f8[b1.sp[k1].feat] > b1.sp[k1].thresh);
                k2 = (k2 << 1) + static_cast<uint32_t>(f8[b2.sp[k2].feat] > b2.sp[k2].thresh);
                k3 = (k3 << 1) + static_cast<uint32_t>(f8[b3.sp[k3].feat] > b3.sp[k3].thresh);
                k4 = (k4 << 1) + static_cast<uint32_t>(f8[b4.sp[k4].feat] > b4.sp[k4].thresh);
                k5 = (k5 << 1) + static_cast<uint32_t>(f8[b5.sp[k5].feat] > b5.sp[k5].thresh);
                k6 = (k6 << 1) + static_cast<uint32_t>(f8[b6.sp[k6].feat] > b6.sp[k6].thresh);
                k7 = (k7 << 1) + static_cast<uint32_t>(f8[b7.sp[k7].feat] > b7.sp[k7].thresh);
            }
            acc += b0.leaf[k0 - 16u] + b1.leaf[k1 - 16u] + b2.leaf[k2 - 16u] + b3.leaf[k3 - 16u] +
                   b4.leaf[k4 - 16u] + b5.leaf[k5 - 16u] + b6.leaf[k6 - 16u] + b7.leaf[k7 - 16u];
        }
        for (; t + 3u < te; t += 4u) {
            const Split2* sp0 = tps[t].sp;      const Split2* sp1 = tps[t + 1u].sp;
            const Split2* sp2 = tps[t + 2u].sp; const Split2* sp3 = tps[t + 3u].sp;
            uint32_t i0 = 2u + static_cast<uint32_t>(f8[sp0[1].feat] > sp0[1].thresh);
            uint32_t i1 = 2u + static_cast<uint32_t>(f8[sp1[1].feat] > sp1[1].thresh);
            uint32_t i2 = 2u + static_cast<uint32_t>(f8[sp2[1].feat] > sp2[1].thresh);
            uint32_t i3 = 2u + static_cast<uint32_t>(f8[sp3[1].feat] > sp3[1].thresh);
            for (int d = 1; d < 5; ++d) {
                i0 = (i0 << 1) + static_cast<uint32_t>(f8[sp0[i0].feat] > sp0[i0].thresh);
                i1 = (i1 << 1) + static_cast<uint32_t>(f8[sp1[i1].feat] > sp1[i1].thresh);
                i2 = (i2 << 1) + static_cast<uint32_t>(f8[sp2[i2].feat] > sp2[i2].thresh);
                i3 = (i3 << 1) + static_cast<uint32_t>(f8[sp3[i3].feat] > sp3[i3].thresh);
            }
            const Bfs9Sub4& b0 = sbs[static_cast<size_t>(t)      * 32u + (i0 - 32u)];
            const Bfs9Sub4& b1 = sbs[static_cast<size_t>(t + 1u) * 32u + (i1 - 32u)];
            const Bfs9Sub4& b2 = sbs[static_cast<size_t>(t + 2u) * 32u + (i2 - 32u)];
            const Bfs9Sub4& b3 = sbs[static_cast<size_t>(t + 3u) * 32u + (i3 - 32u)];
            uint32_t k0 = 2u + static_cast<uint32_t>(f8[b0.sp[1].feat] > b0.sp[1].thresh);
            uint32_t k1 = 2u + static_cast<uint32_t>(f8[b1.sp[1].feat] > b1.sp[1].thresh);
            uint32_t k2 = 2u + static_cast<uint32_t>(f8[b2.sp[1].feat] > b2.sp[1].thresh);
            uint32_t k3 = 2u + static_cast<uint32_t>(f8[b3.sp[1].feat] > b3.sp[1].thresh);
            for (int d = 1; d < 4; ++d) {
                k0 = (k0 << 1) + static_cast<uint32_t>(f8[b0.sp[k0].feat] > b0.sp[k0].thresh);
                k1 = (k1 << 1) + static_cast<uint32_t>(f8[b1.sp[k1].feat] > b1.sp[k1].thresh);
                k2 = (k2 << 1) + static_cast<uint32_t>(f8[b2.sp[k2].feat] > b2.sp[k2].thresh);
                k3 = (k3 << 1) + static_cast<uint32_t>(f8[b3.sp[k3].feat] > b3.sp[k3].thresh);
            }
            acc += b0.leaf[k0 - 16u] + b1.leaf[k1 - 16u] + b2.leaf[k2 - 16u] + b3.leaf[k3 - 16u];
        }
        for (; t < te; ++t) {
            const Split2* sp = tps[t].sp;
            uint32_t i = 2u + static_cast<uint32_t>(f8[sp[1].feat] > sp[1].thresh);
            for (int d = 1; d < 5; ++d) i = (i << 1) + static_cast<uint32_t>(f8[sp[i].feat] > sp[i].thresh);
            const Bfs9Sub4& b = sbs[static_cast<size_t>(t) * 32u + (i - 32u)];
            uint32_t k = 2u + static_cast<uint32_t>(f8[b.sp[1].feat] > b.sp[1].thresh);
            for (int d = 1; d < 4; ++d) k = (k << 1) + static_cast<uint32_t>(f8[b.sp[k].feat] > b.sp[k].thresh);
            acc += b.leaf[k - 16u];
        }
        return acc;
    }

    inline int32_t eval_bfs9_range(const int8_t* __restrict f8, uint32_t tb, uint32_t te) const {
        return eval_bfs9_sub4_range(bfs9_top5_.data(), bfs9_sub4_.data(), f8, tb, te);
    }

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__) || defined(__AVX2__))
#if defined(__GNUC__) || defined(__clang__)
#define TASHKEEL_TARGET_AVX2 __attribute__((target("avx2")))
#else
#define TASHKEEL_TARGET_AVX2
#endif
    // AVX2 Phase-1 (32-lane L5 or 16-lane L4) + Scalar Phase-2 for any 1-based complete BFS tree!
    template <typename TreeT, int DEPTH, uint32_t LEAF_OFF, uint32_t TILE, size_t F8_STRIDE, int SIGN>
    TASHKEEL_TARGET_AVX2
    static void eval_bfs_one_avx2(const TreeT* __restrict ts, uint32_t tb_all, uint32_t te_all,
                                  const std::array<int8_t, F8_STRIDE>* f8_buf, const uint32_t* indices, size_t count,
                                  int32_t* __restrict out_flat, size_t out_stride, uint32_t out_col,
                                  uint32_t num_feats) {
        alignas(32) int8_t fT[128][32];
        alignas(32) uint8_t idx_buf[TILE][32];
        const uint32_t nf = (num_feats > 128u) ? 128u : num_feats;

        for (size_t base = 0; base < count; base += 32u) {
            const size_t cnt = (base + 32u <= count) ? 32u : (count - base);
            if (cnt < 6u) {
                for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
                    const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
                    for (size_t j = 0; j < cnt; ++j) {
                        const uint32_t wi = indices ? indices[base + j] : static_cast<uint32_t>(base + j);
                        const int32_t v = eval_bfs_one_range<TreeT, DEPTH, LEAF_OFF>(ts, f8_buf[wi].data(), tb, te);
                        if constexpr (SIGN > 0) out_flat[wi * out_stride + out_col] += v;
                        else out_flat[wi * out_stride + out_col] -= v;
                    }
                }
                continue;
            }

            const int8_t* ptrs[32];
            uint32_t wis[32];
            for (size_t j = 0; j < cnt; ++j) {
                const uint32_t wi = indices ? indices[base + j] : static_cast<uint32_t>(base + j);
                wis[j] = wi;
                ptrs[j] = f8_buf[wi].data();
            }
            for (uint32_t fi = 0; fi < nf; ++fi) {
                int8_t* dst = fT[fi];
                for (size_t j = 0; j < cnt; ++j) dst[j] = ptrs[j][fi];
            }

            if (cnt > 16u) {
                // 32-lane AVX2 Level-5 (levels 0..4 => idx in 32..63) + scalar levels 5..DEPTH-1
                const __m256i v32 = _mm256_set1_epi8(32), v16 = _mm256_set1_epi8(16);
                const __m256i v8  = _mm256_set1_epi8(8),  v4  = _mm256_set1_epi8(4);
                const __m256i v2  = _mm256_set1_epi8(2),  v1  = _mm256_set1_epi8(1);
#define T_CMP32(k) _mm256_cmpgt_epi8(_mm256_load_si256(reinterpret_cast<const __m256i*>(fT[sp[k].feat])), _mm256_set1_epi8(sp[k].thresh))
                for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
                    const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
                    const uint32_t tcnt = te - tb;
                    for (uint32_t ti = 0; ti < tcnt; ++ti) {
                        const Split2* __restrict sp = ts[tb + ti].sp;
                        __m256i m0 = T_CMP32(1);
                        __m256i c2 = T_CMP32(2), c3 = T_CMP32(3);
                        __m256i m1 = _mm256_blendv_epi8(c2, c3, m0);
                        __m256i c4 = T_CMP32(4), c5 = T_CMP32(5), c6 = T_CMP32(6), c7 = T_CMP32(7);
                        __m256i m2 = _mm256_blendv_epi8(_mm256_blendv_epi8(c4, c5, c2), _mm256_blendv_epi8(c6, c7, c3), m0);
                        __m256i c8=T_CMP32(8), c9=T_CMP32(9), c10=T_CMP32(10), c11=T_CMP32(11);
                        __m256i c12=T_CMP32(12), c13=T_CMP32(13), c14=T_CMP32(14), c15=T_CMP32(15);
                        __m256i m3 = _mm256_blendv_epi8(
                            _mm256_blendv_epi8(_mm256_blendv_epi8(c8, c9, c4), _mm256_blendv_epi8(c10, c11, c5), c2),
                            _mm256_blendv_epi8(_mm256_blendv_epi8(c12, c13, c6), _mm256_blendv_epi8(c14, c15, c7), c3), m0);
                        __m256i e0 = _mm256_blendv_epi8(T_CMP32(16), T_CMP32(17), c8);
                        __m256i e1 = _mm256_blendv_epi8(T_CMP32(18), T_CMP32(19), c9);
                        __m256i e2 = _mm256_blendv_epi8(T_CMP32(20), T_CMP32(21), c10);
                        __m256i e3 = _mm256_blendv_epi8(T_CMP32(22), T_CMP32(23), c11);
                        __m256i e4 = _mm256_blendv_epi8(T_CMP32(24), T_CMP32(25), c12);
                        __m256i e5 = _mm256_blendv_epi8(T_CMP32(26), T_CMP32(27), c13);
                        __m256i e6 = _mm256_blendv_epi8(T_CMP32(28), T_CMP32(29), c14);
                        __m256i e7 = _mm256_blendv_epi8(T_CMP32(30), T_CMP32(31), c15);
                        __m256i m4 = _mm256_blendv_epi8(
                            _mm256_blendv_epi8(_mm256_blendv_epi8(e0, e1, c4), _mm256_blendv_epi8(e2, e3, c5), c2),
                            _mm256_blendv_epi8(_mm256_blendv_epi8(e4, e5, c6), _mm256_blendv_epi8(e6, e7, c7), c3), m0);
                        __m256i idx = _mm256_or_si256(
                            _mm256_or_si256(v32, _mm256_and_si256(m0, v16)),
                            _mm256_or_si256(
                                _mm256_or_si256(_mm256_and_si256(m1, v8), _mm256_and_si256(m2, v4)),
                                _mm256_or_si256(_mm256_and_si256(m3, v2), _mm256_and_si256(m4, v1))));
                        _mm256_store_si256(reinterpret_cast<__m256i*>(idx_buf[ti]), idx);
                    }
                    for (size_t j = 0; j < cnt; ++j) {
                        const int8_t* __restrict fv = ptrs[j];
                        int32_t acc = 0;
                        uint32_t ti = 0;
                        for (; ti + 7u < tcnt; ti += 8u) {
                            const TreeT& b0=ts[tb+ti],    &b1=ts[tb+ti+1u], &b2=ts[tb+ti+2u], &b3=ts[tb+ti+3u];
                            const TreeT& b4=ts[tb+ti+4u], &b5=ts[tb+ti+5u], &b6=ts[tb+ti+6u], &b7=ts[tb+ti+7u];
                            uint32_t i0=idx_buf[ti][j],    i1=idx_buf[ti+1u][j], i2=idx_buf[ti+2u][j], i3=idx_buf[ti+3u][j];
                            uint32_t i4=idx_buf[ti+4u][j], i5=idx_buf[ti+5u][j], i6=idx_buf[ti+6u][j], i7=idx_buf[ti+7u][j];
                            for (int d = 5; d < DEPTH; ++d) {
                                const Split2 s0=b0.sp[i0], s1=b1.sp[i1], s2=b2.sp[i2], s3=b3.sp[i3];
                                const Split2 s4=b4.sp[i4], s5=b5.sp[i5], s6=b6.sp[i6], s7=b7.sp[i7];
                                i0 = (i0 << 1) + static_cast<uint32_t>(fv[s0.feat] > s0.thresh);
                                i1 = (i1 << 1) + static_cast<uint32_t>(fv[s1.feat] > s1.thresh);
                                i2 = (i2 << 1) + static_cast<uint32_t>(fv[s2.feat] > s2.thresh);
                                i3 = (i3 << 1) + static_cast<uint32_t>(fv[s3.feat] > s3.thresh);
                                i4 = (i4 << 1) + static_cast<uint32_t>(fv[s4.feat] > s4.thresh);
                                i5 = (i5 << 1) + static_cast<uint32_t>(fv[s5.feat] > s5.thresh);
                                i6 = (i6 << 1) + static_cast<uint32_t>(fv[s6.feat] > s6.thresh);
                                i7 = (i7 << 1) + static_cast<uint32_t>(fv[s7.feat] > s7.thresh);
                            }
                            acc += b0.leaf[i0 - LEAF_OFF] + b1.leaf[i1 - LEAF_OFF] + b2.leaf[i2 - LEAF_OFF] + b3.leaf[i3 - LEAF_OFF] +
                                   b4.leaf[i4 - LEAF_OFF] + b5.leaf[i5 - LEAF_OFF] + b6.leaf[i6 - LEAF_OFF] + b7.leaf[i7 - LEAF_OFF];
                        }
                        for (; ti < tcnt; ++ti) {
                            const TreeT& bt = ts[tb + ti];
                            uint32_t idx = idx_buf[ti][j];
                            for (int d = 5; d < DEPTH; ++d) {
                                const Split2 s = bt.sp[idx];
                                idx = (idx << 1) + static_cast<uint32_t>(fv[s.feat] > s.thresh);
                            }
                            acc += bt.leaf[idx - LEAF_OFF];
                        }
                        if constexpr (SIGN > 0) out_flat[wis[j] * out_stride + out_col] += acc;
                        else out_flat[wis[j] * out_stride + out_col] -= acc;
                    }
                }
#undef T_CMP32
            } else {
                // 16-lane AVX2/SSE4.1 Level-5 (levels 0..4 => idx in 32..63) + scalar levels 5..DEPTH-1
                const __m128i v32 = _mm_set1_epi8(32), v16 = _mm_set1_epi8(16);
                const __m128i v8  = _mm_set1_epi8(8),  v4  = _mm_set1_epi8(4);
                const __m128i v2  = _mm_set1_epi8(2),  v1  = _mm_set1_epi8(1);
#define T_CMP16(k) _mm_cmpgt_epi8(_mm_load_si128(reinterpret_cast<const __m128i*>(fT[sp[k].feat])), _mm_set1_epi8(sp[k].thresh))
                for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
                    const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
                    const uint32_t tcnt = te - tb;
                    for (uint32_t ti = 0; ti < tcnt; ++ti) {
                        const Split2* __restrict sp = ts[tb + ti].sp;
                        __m128i m0 = T_CMP16(1);
                        __m128i c2 = T_CMP16(2), c3 = T_CMP16(3);
                        __m128i m1 = _mm_blendv_epi8(c2, c3, m0);
                        __m128i c4 = T_CMP16(4), c5 = T_CMP16(5), c6 = T_CMP16(6), c7 = T_CMP16(7);
                        __m128i m2 = _mm_blendv_epi8(_mm_blendv_epi8(c4, c5, c2), _mm_blendv_epi8(c6, c7, c3), m0);
                        __m128i c8=T_CMP16(8), c9=T_CMP16(9), c10=T_CMP16(10), c11=T_CMP16(11);
                        __m128i c12=T_CMP16(12), c13=T_CMP16(13), c14=T_CMP16(14), c15=T_CMP16(15);
                        __m128i m3 = _mm_blendv_epi8(
                            _mm_blendv_epi8(_mm_blendv_epi8(c8, c9, c4), _mm_blendv_epi8(c10, c11, c5), c2),
                            _mm_blendv_epi8(_mm_blendv_epi8(c12, c13, c6), _mm_blendv_epi8(c14, c15, c7), c3), m0);
                        __m128i e0 = _mm_blendv_epi8(T_CMP16(16), T_CMP16(17), c8);
                        __m128i e1 = _mm_blendv_epi8(T_CMP16(18), T_CMP16(19), c9);
                        __m128i e2 = _mm_blendv_epi8(T_CMP16(20), T_CMP16(21), c10);
                        __m128i e3 = _mm_blendv_epi8(T_CMP16(22), T_CMP16(23), c11);
                        __m128i e4 = _mm_blendv_epi8(T_CMP16(24), T_CMP16(25), c12);
                        __m128i e5 = _mm_blendv_epi8(T_CMP16(26), T_CMP16(27), c13);
                        __m128i e6 = _mm_blendv_epi8(T_CMP16(28), T_CMP16(29), c14);
                        __m128i e7 = _mm_blendv_epi8(T_CMP16(30), T_CMP16(31), c15);
                        __m128i m4 = _mm_blendv_epi8(
                            _mm_blendv_epi8(_mm_blendv_epi8(e0, e1, c4), _mm_blendv_epi8(e2, e3, c5), c2),
                            _mm_blendv_epi8(_mm_blendv_epi8(e4, e5, c6), _mm_blendv_epi8(e6, e7, c7), c3), m0);
                        __m128i idx = _mm_or_si128(
                            _mm_or_si128(v32, _mm_and_si128(m0, v16)),
                            _mm_or_si128(
                                _mm_or_si128(_mm_and_si128(m1, v8), _mm_and_si128(m2, v4)),
                                _mm_or_si128(_mm_and_si128(m3, v2), _mm_and_si128(m4, v1))));
                        _mm_store_si128(reinterpret_cast<__m128i*>(idx_buf[ti]), idx);
                    }
                    for (size_t j = 0; j < cnt; ++j) {
                        const int8_t* __restrict fv = ptrs[j];
                        int32_t acc = 0;
                        uint32_t ti = 0;
                        for (; ti + 7u < tcnt; ti += 8u) {
                            const TreeT& b0=ts[tb+ti],    &b1=ts[tb+ti+1u], &b2=ts[tb+ti+2u], &b3=ts[tb+ti+3u];
                            const TreeT& b4=ts[tb+ti+4u], &b5=ts[tb+ti+5u], &b6=ts[tb+ti+6u], &b7=ts[tb+ti+7u];
                            uint32_t i0=idx_buf[ti][j],    i1=idx_buf[ti+1u][j], i2=idx_buf[ti+2u][j], i3=idx_buf[ti+3u][j];
                            uint32_t i4=idx_buf[ti+4u][j], i5=idx_buf[ti+5u][j], i6=idx_buf[ti+6u][j], i7=idx_buf[ti+7u][j];
                            for (int d = 5; d < DEPTH; ++d) {
                                const Split2 s0=b0.sp[i0], s1=b1.sp[i1], s2=b2.sp[i2], s3=b3.sp[i3];
                                const Split2 s4=b4.sp[i4], s5=b5.sp[i5], s6=b6.sp[i6], s7=b7.sp[i7];
                                i0 = (i0 << 1) + static_cast<uint32_t>(fv[s0.feat] > s0.thresh);
                                i1 = (i1 << 1) + static_cast<uint32_t>(fv[s1.feat] > s1.thresh);
                                i2 = (i2 << 1) + static_cast<uint32_t>(fv[s2.feat] > s2.thresh);
                                i3 = (i3 << 1) + static_cast<uint32_t>(fv[s3.feat] > s3.thresh);
                                i4 = (i4 << 1) + static_cast<uint32_t>(fv[s4.feat] > s4.thresh);
                                i5 = (i5 << 1) + static_cast<uint32_t>(fv[s5.feat] > s5.thresh);
                                i6 = (i6 << 1) + static_cast<uint32_t>(fv[s6.feat] > s6.thresh);
                                i7 = (i7 << 1) + static_cast<uint32_t>(fv[s7.feat] > s7.thresh);
                            }
                            acc += b0.leaf[i0 - LEAF_OFF] + b1.leaf[i1 - LEAF_OFF] + b2.leaf[i2 - LEAF_OFF] + b3.leaf[i3 - LEAF_OFF] +
                                   b4.leaf[i4 - LEAF_OFF] + b5.leaf[i5 - LEAF_OFF] + b6.leaf[i6 - LEAF_OFF] + b7.leaf[i7 - LEAF_OFF];
                        }
                        for (; ti < tcnt; ++ti) {
                            const TreeT& bt = ts[tb + ti];
                            uint32_t idx = idx_buf[ti][j];
                            for (int d = 5; d < DEPTH; ++d) {
                                const Split2 s = bt.sp[idx];
                                idx = (idx << 1) + static_cast<uint32_t>(fv[s.feat] > s.thresh);
                            }
                            acc += bt.leaf[idx - LEAF_OFF];
                        }
                        if constexpr (SIGN > 0) out_flat[wis[j] * out_stride + out_col] += acc;
                        else out_flat[wis[j] * out_stride + out_col] -= acc;
                    }
                }
#undef T_CMP16
            }
        }
    }

    // AVX2 Split Top5 (L5_32 / L5_16) + 1-cache-line Sub4 evaluator for depth-9 trees (Core2 & Irab2)!
    template <uint32_t TILE, size_t F8_STRIDE, int SIGN>
    TASHKEEL_TARGET_AVX2
    static void eval_bfs9_sub4_avx2(const Bfs9Top5* __restrict tps, const Bfs9Sub4* __restrict sbs,
                                    uint32_t tb_all, uint32_t te_all,
                                    const std::array<int8_t, F8_STRIDE>* f8_buf, const uint32_t* indices, size_t count,
                                    int32_t* __restrict out_flat, size_t out_stride, uint32_t out_col,
                                    uint32_t num_feats) {
        alignas(32) int8_t fT[128][32];
        alignas(32) uint8_t idx_buf[TILE][32];
        const uint32_t nf = (num_feats > 128u) ? 128u : num_feats;

        for (size_t base = 0; base < count; base += 32u) {
            const size_t cnt = (base + 32u <= count) ? 32u : (count - base);
            if (cnt < 6u) {
                for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
                    const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
                    for (size_t j = 0; j < cnt; ++j) {
                        const uint32_t wi = indices ? indices[base + j] : static_cast<uint32_t>(base + j);
                        const int32_t v = eval_bfs9_sub4_range(tps, sbs, f8_buf[wi].data(), tb, te);
                        if constexpr (SIGN > 0) out_flat[wi * out_stride + out_col] += v;
                        else out_flat[wi * out_stride + out_col] -= v;
                    }
                }
                continue;
            }

            const int8_t* ptrs[32];
            uint32_t wis[32];
            for (size_t j = 0; j < cnt; ++j) {
                const uint32_t wi = indices ? indices[base + j] : static_cast<uint32_t>(base + j);
                wis[j] = wi;
                ptrs[j] = f8_buf[wi].data();
            }
            for (uint32_t fi = 0; fi < nf; ++fi) {
                int8_t* dst = fT[fi];
                for (size_t j = 0; j < cnt; ++j) dst[j] = ptrs[j][fi];
            }

            if (cnt > 16u) {
                const __m256i v16 = _mm256_set1_epi8(16), v8 = _mm256_set1_epi8(8);
                const __m256i v4  = _mm256_set1_epi8(4),  v2 = _mm256_set1_epi8(2), v1 = _mm256_set1_epi8(1);
#define S_CMP32(k) _mm256_cmpgt_epi8(_mm256_load_si256(reinterpret_cast<const __m256i*>(fT[sp[k].feat])), _mm256_set1_epi8(sp[k].thresh))
                for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
                    const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
                    const uint32_t tcnt = te - tb;
                    for (uint32_t ti = 0; ti < tcnt; ++ti) {
                        const Split2* __restrict sp = tps[tb + ti].sp;
                        __m256i m0 = S_CMP32(1);
                        __m256i c2 = S_CMP32(2), c3 = S_CMP32(3);
                        __m256i m1 = _mm256_blendv_epi8(c2, c3, m0);
                        __m256i c4 = S_CMP32(4), c5 = S_CMP32(5), c6 = S_CMP32(6), c7 = S_CMP32(7);
                        __m256i m2 = _mm256_blendv_epi8(_mm256_blendv_epi8(c4, c5, c2), _mm256_blendv_epi8(c6, c7, c3), m0);
                        __m256i c8=S_CMP32(8), c9=S_CMP32(9), c10=S_CMP32(10), c11=S_CMP32(11);
                        __m256i c12=S_CMP32(12), c13=S_CMP32(13), c14=S_CMP32(14), c15=S_CMP32(15);
                        __m256i m3 = _mm256_blendv_epi8(
                            _mm256_blendv_epi8(_mm256_blendv_epi8(c8, c9, c4), _mm256_blendv_epi8(c10, c11, c5), c2),
                            _mm256_blendv_epi8(_mm256_blendv_epi8(c12, c13, c6), _mm256_blendv_epi8(c14, c15, c7), c3), m0);
                        __m256i e0 = _mm256_blendv_epi8(S_CMP32(16), S_CMP32(17), c8);
                        __m256i e1 = _mm256_blendv_epi8(S_CMP32(18), S_CMP32(19), c9);
                        __m256i e2 = _mm256_blendv_epi8(S_CMP32(20), S_CMP32(21), c10);
                        __m256i e3 = _mm256_blendv_epi8(S_CMP32(22), S_CMP32(23), c11);
                        __m256i e4 = _mm256_blendv_epi8(S_CMP32(24), S_CMP32(25), c12);
                        __m256i e5 = _mm256_blendv_epi8(S_CMP32(26), S_CMP32(27), c13);
                        __m256i e6 = _mm256_blendv_epi8(S_CMP32(28), S_CMP32(29), c14);
                        __m256i e7 = _mm256_blendv_epi8(S_CMP32(30), S_CMP32(31), c15);
                        __m256i m4 = _mm256_blendv_epi8(
                            _mm256_blendv_epi8(_mm256_blendv_epi8(e0, e1, c4), _mm256_blendv_epi8(e2, e3, c5), c2),
                            _mm256_blendv_epi8(_mm256_blendv_epi8(e4, e5, c6), _mm256_blendv_epi8(e6, e7, c7), c3), m0);
                        __m256i s = _mm256_or_si256(_mm256_and_si256(m0, v16),
                            _mm256_or_si256(_mm256_or_si256(_mm256_and_si256(m1, v8), _mm256_and_si256(m2, v4)),
                                            _mm256_or_si256(_mm256_and_si256(m3, v2), _mm256_and_si256(m4, v1))));
                        _mm256_store_si256(reinterpret_cast<__m256i*>(idx_buf[ti]), s);
                    }
                    for (size_t j = 0; j < cnt; ++j) {
                        const int8_t* __restrict fv = ptrs[j];
                        int32_t acc = 0;
                        uint32_t ti = 0;
                        for (; ti + 7u < tcnt; ti += 8u) {
                            const Bfs9Sub4& b0 = sbs[static_cast<size_t>(tb + ti)      * 32u + idx_buf[ti][j]];
                            const Bfs9Sub4& b1 = sbs[static_cast<size_t>(tb + ti + 1u) * 32u + idx_buf[ti + 1u][j]];
                            const Bfs9Sub4& b2 = sbs[static_cast<size_t>(tb + ti + 2u) * 32u + idx_buf[ti + 2u][j]];
                            const Bfs9Sub4& b3 = sbs[static_cast<size_t>(tb + ti + 3u) * 32u + idx_buf[ti + 3u][j]];
                            const Bfs9Sub4& b4 = sbs[static_cast<size_t>(tb + ti + 4u) * 32u + idx_buf[ti + 4u][j]];
                            const Bfs9Sub4& b5 = sbs[static_cast<size_t>(tb + ti + 5u) * 32u + idx_buf[ti + 5u][j]];
                            const Bfs9Sub4& b6 = sbs[static_cast<size_t>(tb + ti + 6u) * 32u + idx_buf[ti + 6u][j]];
                            const Bfs9Sub4& b7 = sbs[static_cast<size_t>(tb + ti + 7u) * 32u + idx_buf[ti + 7u][j]];
                            uint32_t k0 = 2u + static_cast<uint32_t>(fv[b0.sp[1].feat] > b0.sp[1].thresh);
                            uint32_t k1 = 2u + static_cast<uint32_t>(fv[b1.sp[1].feat] > b1.sp[1].thresh);
                            uint32_t k2 = 2u + static_cast<uint32_t>(fv[b2.sp[1].feat] > b2.sp[1].thresh);
                            uint32_t k3 = 2u + static_cast<uint32_t>(fv[b3.sp[1].feat] > b3.sp[1].thresh);
                            uint32_t k4 = 2u + static_cast<uint32_t>(fv[b4.sp[1].feat] > b4.sp[1].thresh);
                            uint32_t k5 = 2u + static_cast<uint32_t>(fv[b5.sp[1].feat] > b5.sp[1].thresh);
                            uint32_t k6 = 2u + static_cast<uint32_t>(fv[b6.sp[1].feat] > b6.sp[1].thresh);
                            uint32_t k7 = 2u + static_cast<uint32_t>(fv[b7.sp[1].feat] > b7.sp[1].thresh);
                            for (int d = 1; d < 4; ++d) {
                                k0 = (k0 << 1) + static_cast<uint32_t>(fv[b0.sp[k0].feat] > b0.sp[k0].thresh);
                                k1 = (k1 << 1) + static_cast<uint32_t>(fv[b1.sp[k1].feat] > b1.sp[k1].thresh);
                                k2 = (k2 << 1) + static_cast<uint32_t>(fv[b2.sp[k2].feat] > b2.sp[k2].thresh);
                                k3 = (k3 << 1) + static_cast<uint32_t>(fv[b3.sp[k3].feat] > b3.sp[k3].thresh);
                                k4 = (k4 << 1) + static_cast<uint32_t>(fv[b4.sp[k4].feat] > b4.sp[k4].thresh);
                                k5 = (k5 << 1) + static_cast<uint32_t>(fv[b5.sp[k5].feat] > b5.sp[k5].thresh);
                                k6 = (k6 << 1) + static_cast<uint32_t>(fv[b6.sp[k6].feat] > b6.sp[k6].thresh);
                                k7 = (k7 << 1) + static_cast<uint32_t>(fv[b7.sp[k7].feat] > b7.sp[k7].thresh);
                            }
                            acc += b0.leaf[k0 - 16u] + b1.leaf[k1 - 16u] + b2.leaf[k2 - 16u] + b3.leaf[k3 - 16u] +
                                   b4.leaf[k4 - 16u] + b5.leaf[k5 - 16u] + b6.leaf[k6 - 16u] + b7.leaf[k7 - 16u];
                        }
                        for (; ti < tcnt; ++ti) {
                            const Bfs9Sub4& b = sbs[static_cast<size_t>(tb + ti) * 32u + idx_buf[ti][j]];
                            uint32_t k = 2u + static_cast<uint32_t>(fv[b.sp[1].feat] > b.sp[1].thresh);
                            for (int d = 1; d < 4; ++d) k = (k << 1) + static_cast<uint32_t>(fv[b.sp[k].feat] > b.sp[k].thresh);
                            acc += b.leaf[k - 16u];
                        }
                        if constexpr (SIGN > 0) out_flat[wis[j] * out_stride + out_col] += acc;
                        else out_flat[wis[j] * out_stride + out_col] -= acc;
                    }
                }
#undef S_CMP32
            } else {
                const __m128i v16 = _mm_set1_epi8(16), v8 = _mm_set1_epi8(8);
                const __m128i v4  = _mm_set1_epi8(4),  v2 = _mm_set1_epi8(2), v1 = _mm_set1_epi8(1);
#define S_CMP16(k) _mm_cmpgt_epi8(_mm_load_si128(reinterpret_cast<const __m128i*>(fT[sp[k].feat])), _mm_set1_epi8(sp[k].thresh))
                for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
                    const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
                    const uint32_t tcnt = te - tb;
                    for (uint32_t ti = 0; ti < tcnt; ++ti) {
                        const Split2* __restrict sp = tps[tb + ti].sp;
                        __m128i m0 = S_CMP16(1);
                        __m128i c2 = S_CMP16(2), c3 = S_CMP16(3);
                        __m128i m1 = _mm_blendv_epi8(c2, c3, m0);
                        __m128i c4 = S_CMP16(4), c5 = S_CMP16(5), c6 = S_CMP16(6), c7 = S_CMP16(7);
                        __m128i m2 = _mm_blendv_epi8(_mm_blendv_epi8(c4, c5, c2), _mm_blendv_epi8(c6, c7, c3), m0);
                        __m128i c8=S_CMP16(8), c9=S_CMP16(9), c10=S_CMP16(10), c11=S_CMP16(11);
                        __m128i c12=S_CMP16(12), c13=S_CMP16(13), c14=S_CMP16(14), c15=S_CMP16(15);
                        __m128i m3 = _mm_blendv_epi8(
                            _mm_blendv_epi8(_mm_blendv_epi8(c8, c9, c4), _mm_blendv_epi8(c10, c11, c5), c2),
                            _mm_blendv_epi8(_mm_blendv_epi8(c12, c13, c6), _mm_blendv_epi8(c14, c15, c7), c3), m0);
                        __m128i e0 = _mm_blendv_epi8(S_CMP16(16), S_CMP16(17), c8);
                        __m128i e1 = _mm_blendv_epi8(S_CMP16(18), S_CMP16(19), c9);
                        __m128i e2 = _mm_blendv_epi8(S_CMP16(20), S_CMP16(21), c10);
                        __m128i e3 = _mm_blendv_epi8(S_CMP16(22), S_CMP16(23), c11);
                        __m128i e4 = _mm_blendv_epi8(S_CMP16(24), S_CMP16(25), c12);
                        __m128i e5 = _mm_blendv_epi8(S_CMP16(26), S_CMP16(27), c13);
                        __m128i e6 = _mm_blendv_epi8(S_CMP16(28), S_CMP16(29), c14);
                        __m128i e7 = _mm_blendv_epi8(S_CMP16(30), S_CMP16(31), c15);
                        __m128i m4 = _mm_blendv_epi8(
                            _mm_blendv_epi8(_mm_blendv_epi8(e0, e1, c4), _mm_blendv_epi8(e2, e3, c5), c2),
                            _mm_blendv_epi8(_mm_blendv_epi8(e4, e5, c6), _mm_blendv_epi8(e6, e7, c7), c3), m0);
                        __m128i s = _mm_or_si128(_mm_and_si128(m0, v16),
                            _mm_or_si128(_mm_or_si128(_mm_and_si128(m1, v8), _mm_and_si128(m2, v4)),
                                         _mm_or_si128(_mm_and_si128(m3, v2), _mm_and_si128(m4, v1))));
                        _mm_store_si128(reinterpret_cast<__m128i*>(idx_buf[ti]), s);
                    }
                    for (size_t j = 0; j < cnt; ++j) {
                        const int8_t* __restrict fv = ptrs[j];
                        int32_t acc = 0;
                        uint32_t ti = 0;
                        for (; ti + 7u < tcnt; ti += 8u) {
                            const Bfs9Sub4& b0 = sbs[static_cast<size_t>(tb + ti)      * 32u + idx_buf[ti][j]];
                            const Bfs9Sub4& b1 = sbs[static_cast<size_t>(tb + ti + 1u) * 32u + idx_buf[ti + 1u][j]];
                            const Bfs9Sub4& b2 = sbs[static_cast<size_t>(tb + ti + 2u) * 32u + idx_buf[ti + 2u][j]];
                            const Bfs9Sub4& b3 = sbs[static_cast<size_t>(tb + ti + 3u) * 32u + idx_buf[ti + 3u][j]];
                            const Bfs9Sub4& b4 = sbs[static_cast<size_t>(tb + ti + 4u) * 32u + idx_buf[ti + 4u][j]];
                            const Bfs9Sub4& b5 = sbs[static_cast<size_t>(tb + ti + 5u) * 32u + idx_buf[ti + 5u][j]];
                            const Bfs9Sub4& b6 = sbs[static_cast<size_t>(tb + ti + 6u) * 32u + idx_buf[ti + 6u][j]];
                            const Bfs9Sub4& b7 = sbs[static_cast<size_t>(tb + ti + 7u) * 32u + idx_buf[ti + 7u][j]];
                            uint32_t k0 = 2u + static_cast<uint32_t>(fv[b0.sp[1].feat] > b0.sp[1].thresh);
                            uint32_t k1 = 2u + static_cast<uint32_t>(fv[b1.sp[1].feat] > b1.sp[1].thresh);
                            uint32_t k2 = 2u + static_cast<uint32_t>(fv[b2.sp[1].feat] > b2.sp[1].thresh);
                            uint32_t k3 = 2u + static_cast<uint32_t>(fv[b3.sp[1].feat] > b3.sp[1].thresh);
                            uint32_t k4 = 2u + static_cast<uint32_t>(fv[b4.sp[1].feat] > b4.sp[1].thresh);
                            uint32_t k5 = 2u + static_cast<uint32_t>(fv[b5.sp[1].feat] > b5.sp[1].thresh);
                            uint32_t k6 = 2u + static_cast<uint32_t>(fv[b6.sp[1].feat] > b6.sp[1].thresh);
                            uint32_t k7 = 2u + static_cast<uint32_t>(fv[b7.sp[1].feat] > b7.sp[1].thresh);
                            for (int d = 1; d < 4; ++d) {
                                k0 = (k0 << 1) + static_cast<uint32_t>(fv[b0.sp[k0].feat] > b0.sp[k0].thresh);
                                k1 = (k1 << 1) + static_cast<uint32_t>(fv[b1.sp[k1].feat] > b1.sp[k1].thresh);
                                k2 = (k2 << 1) + static_cast<uint32_t>(fv[b2.sp[k2].feat] > b2.sp[k2].thresh);
                                k3 = (k3 << 1) + static_cast<uint32_t>(fv[b3.sp[k3].feat] > b3.sp[k3].thresh);
                                k4 = (k4 << 1) + static_cast<uint32_t>(fv[b4.sp[k4].feat] > b4.sp[k4].thresh);
                                k5 = (k5 << 1) + static_cast<uint32_t>(fv[b5.sp[k5].feat] > b5.sp[k5].thresh);
                                k6 = (k6 << 1) + static_cast<uint32_t>(fv[b6.sp[k6].feat] > b6.sp[k6].thresh);
                                k7 = (k7 << 1) + static_cast<uint32_t>(fv[b7.sp[k7].feat] > b7.sp[k7].thresh);
                            }
                            acc += b0.leaf[k0 - 16u] + b1.leaf[k1 - 16u] + b2.leaf[k2 - 16u] + b3.leaf[k3 - 16u] +
                                   b4.leaf[k4 - 16u] + b5.leaf[k5 - 16u] + b6.leaf[k6 - 16u] + b7.leaf[k7 - 16u];
                        }
                        for (; ti < tcnt; ++ti) {
                            const Bfs9Sub4& b = sbs[static_cast<size_t>(tb + ti) * 32u + idx_buf[ti][j]];
                            uint32_t k = 2u + static_cast<uint32_t>(fv[b.sp[1].feat] > b.sp[1].thresh);
                            for (int d = 1; d < 4; ++d) k = (k << 1) + static_cast<uint32_t>(fv[b.sp[k].feat] > b.sp[k].thresh);
                            acc += b.leaf[k - 16u];
                        }
                        if constexpr (SIGN > 0) out_flat[wis[j] * out_stride + out_col] += acc;
                        else out_flat[wis[j] * out_stride + out_col] -= acc;
                    }
                }
#undef S_CMP16
            }
        }
    }
#undef TASHKEEL_TARGET_AVX2
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    template <typename TreeT, int DEPTH, uint32_t LEAF_OFF, uint32_t TILE, size_t F8_STRIDE, int SIGN>
    static void eval_bfs_one_neon(const TreeT* __restrict ts, uint32_t tb_all, uint32_t te_all,
                                  const std::array<int8_t, F8_STRIDE>* f8_buf, const uint32_t* indices, size_t count,
                                  int32_t* __restrict out_flat, size_t out_stride, uint32_t out_col,
                                  uint32_t num_feats) {
        alignas(16) int8_t fT[128][16];
        alignas(16) uint8_t idx_buf[TILE][16];
        const uint32_t nf = (num_feats > 128u) ? 128u : num_feats;

        for (size_t base = 0; base < count; base += 16u) {
            const size_t cnt = (base + 16u <= count) ? 16u : (count - base);
            if (cnt < 6u) {
                for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
                    const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
                    for (size_t j = 0; j < cnt; ++j) {
                        const uint32_t wi = indices ? indices[base + j] : static_cast<uint32_t>(base + j);
                        const int32_t v = eval_bfs_one_range<TreeT, DEPTH, LEAF_OFF>(ts, f8_buf[wi].data(), tb, te);
                        if constexpr (SIGN > 0) out_flat[wi * out_stride + out_col] += v;
                        else out_flat[wi * out_stride + out_col] -= v;
                    }
                }
                continue;
            }
            const int8_t* ptrs[16];
            uint32_t wis[16];
            for (size_t j = 0; j < cnt; ++j) {
                const uint32_t wi = indices ? indices[base + j] : static_cast<uint32_t>(base + j);
                wis[j] = wi;
                ptrs[j] = f8_buf[wi].data();
            }
            for (uint32_t fi = 0; fi < nf; ++fi) {
                int8_t* dst = fT[fi];
                for (size_t j = 0; j < cnt; ++j) dst[j] = ptrs[j][fi];
            }
            const uint8x16_t v32 = vdupq_n_u8(32), v16 = vdupq_n_u8(16), v8 = vdupq_n_u8(8);
            const uint8x16_t v4 = vdupq_n_u8(4), v2 = vdupq_n_u8(2), v1 = vdupq_n_u8(1);
#define T_NCMP(k) vcgtq_s8(vld1q_s8(fT[sp[k].feat]), vdupq_n_s8(sp[k].thresh))
            for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
                const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
                const uint32_t tcnt = te - tb;
                for (uint32_t ti = 0; ti < tcnt; ++ti) {
                    const Split2* __restrict sp = ts[tb + ti].sp;
                    uint8x16_t m0 = T_NCMP(1);
                    uint8x16_t c2 = T_NCMP(2), c3 = T_NCMP(3);
                    uint8x16_t m1 = vbslq_u8(m0, c3, c2);
                    uint8x16_t c4 = T_NCMP(4), c5 = T_NCMP(5), c6 = T_NCMP(6), c7 = T_NCMP(7);
                    uint8x16_t m2 = vbslq_u8(m0, vbslq_u8(c3, c7, c6), vbslq_u8(c2, c5, c4));
                    uint8x16_t c8=T_NCMP(8), c9=T_NCMP(9), c10=T_NCMP(10), c11=T_NCMP(11);
                    uint8x16_t c12=T_NCMP(12), c13=T_NCMP(13), c14=T_NCMP(14), c15=T_NCMP(15);
                    uint8x16_t m3 = vbslq_u8(m0,
                        vbslq_u8(c3, vbslq_u8(c7, c15, c14), vbslq_u8(c6, c13, c12)),
                        vbslq_u8(c2, vbslq_u8(c5, c11, c10), vbslq_u8(c4, c9,  c8)));
                    uint8x16_t e0 = vbslq_u8(c8,  T_NCMP(17), T_NCMP(16));
                    uint8x16_t e1 = vbslq_u8(c9,  T_NCMP(19), T_NCMP(18));
                    uint8x16_t e2 = vbslq_u8(c10, T_NCMP(21), T_NCMP(20));
                    uint8x16_t e3 = vbslq_u8(c11, T_NCMP(23), T_NCMP(22));
                    uint8x16_t e4 = vbslq_u8(c12, T_NCMP(25), T_NCMP(24));
                    uint8x16_t e5 = vbslq_u8(c13, T_NCMP(27), T_NCMP(26));
                    uint8x16_t e6 = vbslq_u8(c14, T_NCMP(29), T_NCMP(28));
                    uint8x16_t e7 = vbslq_u8(c15, T_NCMP(31), T_NCMP(30));
                    uint8x16_t m4 = vbslq_u8(m0,
                        vbslq_u8(c3, vbslq_u8(c7, e7, e6), vbslq_u8(c6, e5, e4)),
                        vbslq_u8(c2, vbslq_u8(c5, e3, e2), vbslq_u8(c4, e1, e0)));
                    uint8x16_t idx = vorrq_u8(
                        vorrq_u8(v32, vandq_u8(m0, v16)),
                        vorrq_u8(vorrq_u8(vandq_u8(m1, v8), vandq_u8(m2, v4)),
                                 vorrq_u8(vandq_u8(m3, v2), vandq_u8(m4, v1))));
                    vst1q_u8(idx_buf[ti], idx);
                }
                for (size_t j = 0; j < cnt; ++j) {
                    const int8_t* __restrict fv = ptrs[j];
                    int32_t acc = 0;
                    uint32_t ti = 0;
                    for (; ti + 7u < tcnt; ti += 8u) {
                        const TreeT& b0=ts[tb+ti],    &b1=ts[tb+ti+1u], &b2=ts[tb+ti+2u], &b3=ts[tb+ti+3u];
                        const TreeT& b4=ts[tb+ti+4u], &b5=ts[tb+ti+5u], &b6=ts[tb+ti+6u], &b7=ts[tb+ti+7u];
                        uint32_t i0=idx_buf[ti][j],    i1=idx_buf[ti+1u][j], i2=idx_buf[ti+2u][j], i3=idx_buf[ti+3u][j];
                        uint32_t i4=idx_buf[ti+4u][j], i5=idx_buf[ti+5u][j], i6=idx_buf[ti+6u][j], i7=idx_buf[ti+7u][j];
                        for (int d = 5; d < DEPTH; ++d) {
                            const Split2 s0=b0.sp[i0], s1=b1.sp[i1], s2=b2.sp[i2], s3=b3.sp[i3];
                            const Split2 s4=b4.sp[i4], s5=b5.sp[i5], s6=b6.sp[i6], s7=b7.sp[i7];
                            i0 = (i0 << 1) + static_cast<uint32_t>(fv[s0.feat] > s0.thresh);
                            i1 = (i1 << 1) + static_cast<uint32_t>(fv[s1.feat] > s1.thresh);
                            i2 = (i2 << 1) + static_cast<uint32_t>(fv[s2.feat] > s2.thresh);
                            i3 = (i3 << 1) + static_cast<uint32_t>(fv[s3.feat] > s3.thresh);
                            i4 = (i4 << 1) + static_cast<uint32_t>(fv[s4.feat] > s4.thresh);
                            i5 = (i5 << 1) + static_cast<uint32_t>(fv[s5.feat] > s5.thresh);
                            i6 = (i6 << 1) + static_cast<uint32_t>(fv[s6.feat] > s6.thresh);
                            i7 = (i7 << 1) + static_cast<uint32_t>(fv[s7.feat] > s7.thresh);
                        }
                        acc += b0.leaf[i0 - LEAF_OFF] + b1.leaf[i1 - LEAF_OFF] + b2.leaf[i2 - LEAF_OFF] + b3.leaf[i3 - LEAF_OFF] +
                               b4.leaf[i4 - LEAF_OFF] + b5.leaf[i5 - LEAF_OFF] + b6.leaf[i6 - LEAF_OFF] + b7.leaf[i7 - LEAF_OFF];
                    }
                    for (; ti < tcnt; ++ti) {
                        const TreeT& bt = ts[tb + ti];
                        uint32_t idx = idx_buf[ti][j];
                        for (int d = 5; d < DEPTH; ++d) {
                            const Split2 s = bt.sp[idx];
                            idx = (idx << 1) + static_cast<uint32_t>(fv[s.feat] > s.thresh);
                        }
                        acc += bt.leaf[idx - LEAF_OFF];
                    }
                    if constexpr (SIGN > 0) out_flat[wis[j] * out_stride + out_col] += acc;
                    else out_flat[wis[j] * out_stride + out_col] -= acc;
                }
            }
#undef T_NCMP
        }
    }

    template <uint32_t TILE, size_t F8_STRIDE, int SIGN>
    static void eval_bfs9_sub4_neon(const Bfs9Top5* __restrict tps, const Bfs9Sub4* __restrict sbs,
                                    uint32_t tb_all, uint32_t te_all,
                                    const std::array<int8_t, F8_STRIDE>* f8_buf, const uint32_t* indices, size_t count,
                                    int32_t* __restrict out_flat, size_t out_stride, uint32_t out_col,
                                    uint32_t num_feats) {
        alignas(16) int8_t fT[128][16];
        alignas(16) uint8_t idx_buf[TILE][16];
        const uint32_t nf = (num_feats > 128u) ? 128u : num_feats;

        for (size_t base = 0; base < count; base += 16u) {
            const size_t cnt = (base + 16u <= count) ? 16u : (count - base);
            if (cnt < 6u) {
                for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
                    const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
                    for (size_t j = 0; j < cnt; ++j) {
                        const uint32_t wi = indices ? indices[base + j] : static_cast<uint32_t>(base + j);
                        const int32_t v = eval_bfs9_sub4_range(tps, sbs, f8_buf[wi].data(), tb, te);
                        if constexpr (SIGN > 0) out_flat[wi * out_stride + out_col] += v;
                        else out_flat[wi * out_stride + out_col] -= v;
                    }
                }
                continue;
            }
            const int8_t* ptrs[16];
            uint32_t wis[16];
            for (size_t j = 0; j < cnt; ++j) {
                const uint32_t wi = indices ? indices[base + j] : static_cast<uint32_t>(base + j);
                wis[j] = wi;
                ptrs[j] = f8_buf[wi].data();
            }
            for (uint32_t fi = 0; fi < nf; ++fi) {
                int8_t* dst = fT[fi];
                for (size_t j = 0; j < cnt; ++j) dst[j] = ptrs[j][fi];
            }
            const uint8x16_t v16 = vdupq_n_u8(16), v8 = vdupq_n_u8(8), v4 = vdupq_n_u8(4), v2 = vdupq_n_u8(2), v1 = vdupq_n_u8(1);
#define S_NCMP(k) vcgtq_s8(vld1q_s8(fT[sp[k].feat]), vdupq_n_s8(sp[k].thresh))
            for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
                const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
                const uint32_t tcnt = te - tb;
                for (uint32_t ti = 0; ti < tcnt; ++ti) {
                    const Split2* __restrict sp = tps[tb + ti].sp;
                    uint8x16_t m0 = S_NCMP(1);
                    uint8x16_t c2 = S_NCMP(2), c3 = S_NCMP(3);
                    uint8x16_t m1 = vbslq_u8(m0, c3, c2);
                    uint8x16_t c4 = S_NCMP(4), c5 = S_NCMP(5), c6 = S_NCMP(6), c7 = S_NCMP(7);
                    uint8x16_t m2 = vbslq_u8(m0, vbslq_u8(c3, c7, c6), vbslq_u8(c2, c5, c4));
                    uint8x16_t c8=S_NCMP(8), c9=S_NCMP(9), c10=S_NCMP(10), c11=S_NCMP(11);
                    uint8x16_t c12=S_NCMP(12), c13=S_NCMP(13), c14=S_NCMP(14), c15=S_NCMP(15);
                    uint8x16_t m3 = vbslq_u8(m0,
                        vbslq_u8(c3, vbslq_u8(c7, c15, c14), vbslq_u8(c6, c13, c12)),
                        vbslq_u8(c2, vbslq_u8(c5, c11, c10), vbslq_u8(c4, c9,  c8)));
                    uint8x16_t e0 = vbslq_u8(c8,  S_NCMP(17), S_NCMP(16));
                    uint8x16_t e1 = vbslq_u8(c9,  S_NCMP(19), S_NCMP(18));
                    uint8x16_t e2 = vbslq_u8(c10, S_NCMP(21), S_NCMP(20));
                    uint8x16_t e3 = vbslq_u8(c11, S_NCMP(23), S_NCMP(22));
                    uint8x16_t e4 = vbslq_u8(c12, S_NCMP(25), S_NCMP(24));
                    uint8x16_t e5 = vbslq_u8(c13, S_NCMP(27), S_NCMP(26));
                    uint8x16_t e6 = vbslq_u8(c14, S_NCMP(29), S_NCMP(28));
                    uint8x16_t e7 = vbslq_u8(c15, S_NCMP(31), S_NCMP(30));
                    uint8x16_t m4 = vbslq_u8(m0,
                        vbslq_u8(c3, vbslq_u8(c7, e7, e6), vbslq_u8(c6, e5, e4)),
                        vbslq_u8(c2, vbslq_u8(c5, e3, e2), vbslq_u8(c4, e1, e0)));
                    uint8x16_t s = vorrq_u8(vandq_u8(m0, v16),
                        vorrq_u8(vorrq_u8(vandq_u8(m1, v8), vandq_u8(m2, v4)),
                                 vorrq_u8(vandq_u8(m3, v2), vandq_u8(m4, v1))));
                    vst1q_u8(idx_buf[ti], s);
                }
                for (size_t j = 0; j < cnt; ++j) {
                    const int8_t* __restrict fv = ptrs[j];
                    int32_t acc = 0;
                    uint32_t ti = 0;
                    for (; ti + 7u < tcnt; ti += 8u) {
                        const Bfs9Sub4& b0 = sbs[static_cast<size_t>(tb + ti)      * 32u + idx_buf[ti][j]];
                        const Bfs9Sub4& b1 = sbs[static_cast<size_t>(tb + ti + 1u) * 32u + idx_buf[ti + 1u][j]];
                        const Bfs9Sub4& b2 = sbs[static_cast<size_t>(tb + ti + 2u) * 32u + idx_buf[ti + 2u][j]];
                        const Bfs9Sub4& b3 = sbs[static_cast<size_t>(tb + ti + 3u) * 32u + idx_buf[ti + 3u][j]];
                        const Bfs9Sub4& b4 = sbs[static_cast<size_t>(tb + ti + 4u) * 32u + idx_buf[ti + 4u][j]];
                        const Bfs9Sub4& b5 = sbs[static_cast<size_t>(tb + ti + 5u) * 32u + idx_buf[ti + 5u][j]];
                        const Bfs9Sub4& b6 = sbs[static_cast<size_t>(tb + ti + 6u) * 32u + idx_buf[ti + 6u][j]];
                        const Bfs9Sub4& b7 = sbs[static_cast<size_t>(tb + ti + 7u) * 32u + idx_buf[ti + 7u][j]];
                        uint32_t k0 = 2u + static_cast<uint32_t>(fv[b0.sp[1].feat] > b0.sp[1].thresh);
                        uint32_t k1 = 2u + static_cast<uint32_t>(fv[b1.sp[1].feat] > b1.sp[1].thresh);
                        uint32_t k2 = 2u + static_cast<uint32_t>(fv[b2.sp[1].feat] > b2.sp[1].thresh);
                        uint32_t k3 = 2u + static_cast<uint32_t>(fv[b3.sp[1].feat] > b3.sp[1].thresh);
                        uint32_t k4 = 2u + static_cast<uint32_t>(fv[b4.sp[1].feat] > b4.sp[1].thresh);
                        uint32_t k5 = 2u + static_cast<uint32_t>(fv[b5.sp[1].feat] > b5.sp[1].thresh);
                        uint32_t k6 = 2u + static_cast<uint32_t>(fv[b6.sp[1].feat] > b6.sp[1].thresh);
                        uint32_t k7 = 2u + static_cast<uint32_t>(fv[b7.sp[1].feat] > b7.sp[1].thresh);
                        for (int d = 1; d < 4; ++d) {
                            k0 = (k0 << 1) + static_cast<uint32_t>(fv[b0.sp[k0].feat] > b0.sp[k0].thresh);
                            k1 = (k1 << 1) + static_cast<uint32_t>(fv[b1.sp[k1].feat] > b1.sp[k1].thresh);
                            k2 = (k2 << 1) + static_cast<uint32_t>(fv[b2.sp[k2].feat] > b2.sp[k2].thresh);
                            k3 = (k3 << 1) + static_cast<uint32_t>(fv[b3.sp[k3].feat] > b3.sp[k3].thresh);
                            k4 = (k4 << 1) + static_cast<uint32_t>(fv[b4.sp[k4].feat] > b4.sp[k4].thresh);
                            k5 = (k5 << 1) + static_cast<uint32_t>(fv[b5.sp[k5].feat] > b5.sp[k5].thresh);
                            k6 = (k6 << 1) + static_cast<uint32_t>(fv[b6.sp[k6].feat] > b6.sp[k6].thresh);
                            k7 = (k7 << 1) + static_cast<uint32_t>(fv[b7.sp[k7].feat] > b7.sp[k7].thresh);
                        }
                        acc += b0.leaf[k0 - 16u] + b1.leaf[k1 - 16u] + b2.leaf[k2 - 16u] + b3.leaf[k3 - 16u] +
                               b4.leaf[k4 - 16u] + b5.leaf[k5 - 16u] + b6.leaf[k6 - 16u] + b7.leaf[k7 - 16u];
                    }
                    for (; ti < tcnt; ++ti) {
                        const Bfs9Sub4& b = sbs[static_cast<size_t>(tb + ti) * 32u + idx_buf[ti][j]];
                        uint32_t k = 2u + static_cast<uint32_t>(fv[b.sp[1].feat] > b.sp[1].thresh);
                        for (int d = 1; d < 4; ++d) k = (k << 1) + static_cast<uint32_t>(fv[b.sp[k].feat] > b.sp[k].thresh);
                        acc += b.leaf[k - 16u];
                    }
                    if constexpr (SIGN > 0) out_flat[wis[j] * out_stride + out_col] += acc;
                    else out_flat[wis[j] * out_stride + out_col] -= acc;
                }
            }
#undef S_NCMP
        }
    }
#endif

    template <uint32_t TILE, size_t F8_STRIDE, int SIGN>
    static void eval_bfs9_sub4_scalar_tiled(const Bfs9Top5* __restrict tps, const Bfs9Sub4* __restrict sbs,
                                            uint32_t tb_all, uint32_t te_all,
                                            const std::array<int8_t, F8_STRIDE>* f8_buf, const uint32_t* indices, size_t count,
                                            int32_t* __restrict out_flat, size_t out_stride, uint32_t out_col) {
        for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
            const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
            for (size_t j = 0; j < count; ++j) {
                const uint32_t wi = indices ? indices[j] : static_cast<uint32_t>(j);
                const int32_t v = eval_bfs9_sub4_range(tps, sbs, f8_buf[wi].data(), tb, te);
                if constexpr (SIGN > 0) out_flat[wi * out_stride + out_col] += v;
                else out_flat[wi * out_stride + out_col] -= v;
            }
        }
    }

    template <typename TreeT, int DEPTH, uint32_t LEAF_OFF, uint32_t TILE, size_t F8_STRIDE, int SIGN>
    static void eval_bfs_one_scalar_tiled(const TreeT* __restrict ts, uint32_t tb_all, uint32_t te_all,
                                          const std::array<int8_t, F8_STRIDE>* f8_buf, const uint32_t* indices, size_t count,
                                          int32_t* __restrict out_flat, size_t out_stride, uint32_t out_col) {
        for (uint32_t tb = tb_all; tb < te_all; tb += TILE) {
            const uint32_t te = (tb + TILE < te_all) ? (tb + TILE) : te_all;
            for (size_t j = 0; j < count; ++j) {
                const uint32_t wi = indices ? indices[j] : static_cast<uint32_t>(j);
                const int32_t v = eval_bfs_one_range<TreeT, DEPTH, LEAF_OFF>(ts, f8_buf[wi].data(), tb, te);
                if constexpr (SIGN > 0) out_flat[wi * out_stride + out_col] += v;
                else out_flat[wi * out_stride + out_col] -= v;
            }
        }
    }

    template <size_t F8_STRIDE, int SIGN>
    inline void eval_batch_range_add(const std::array<int8_t, F8_STRIDE>* f8_buf, const uint32_t* indices, size_t count,
                                     uint32_t tb, uint32_t te, int32_t* __restrict out_flat, size_t out_stride, uint32_t out_col) const {
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__) || defined(__AVX2__))
        if (count >= 6u && cpu_has_avx2()) {
            switch (fast_kind_) {
                case 1: eval_bfs_one_avx2<Bfs6One, 6, 64u, 64u, F8_STRIDE, SIGN>(bfs6_trees_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col, used_feats_); return;
                case 2: eval_bfs_one_avx2<Bfs7One, 7, 128u, 32u, F8_STRIDE, SIGN>(bfs7_one_trees_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col, used_feats_); return;
                case 3: eval_bfs_one_avx2<Bfs8One, 8, 256u, 32u, F8_STRIDE, SIGN>(bfs8_trees_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col, used_feats_); return;
                case 4: eval_bfs9_sub4_avx2<32u, F8_STRIDE, SIGN>(bfs9_top5_.data(), bfs9_sub4_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col, used_feats_); return;
                default: break;
            }
        }
#elif defined(__aarch64__) || defined(_M_ARM64)
        if (count >= 6u) {
            switch (fast_kind_) {
                case 1: eval_bfs_one_neon<Bfs6One, 6, 64u, 64u, F8_STRIDE, SIGN>(bfs6_trees_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col, used_feats_); return;
                case 2: eval_bfs_one_neon<Bfs7One, 7, 128u, 32u, F8_STRIDE, SIGN>(bfs7_one_trees_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col, used_feats_); return;
                case 3: eval_bfs_one_neon<Bfs8One, 8, 256u, 32u, F8_STRIDE, SIGN>(bfs8_trees_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col, used_feats_); return;
                case 4: eval_bfs9_sub4_neon<32u, F8_STRIDE, SIGN>(bfs9_top5_.data(), bfs9_sub4_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col, used_feats_); return;
                default: break;
            }
        }
#endif
        switch (fast_kind_) {
            case 1: eval_bfs_one_scalar_tiled<Bfs6One, 6, 64u, 64u, F8_STRIDE, SIGN>(bfs6_trees_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col); break;
            case 2: eval_bfs_one_scalar_tiled<Bfs7One, 7, 128u, 32u, F8_STRIDE, SIGN>(bfs7_one_trees_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col); break;
            case 3: eval_bfs_one_scalar_tiled<Bfs8One, 8, 256u, 32u, F8_STRIDE, SIGN>(bfs8_trees_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col); break;
            case 4: eval_bfs9_sub4_scalar_tiled<32u, F8_STRIDE, SIGN>(bfs9_top5_.data(), bfs9_sub4_.data(), tb, te, f8_buf, indices, count, out_flat, out_stride, out_col); break;
            default: {
                constexpr uint32_t TILE = 32u;
                for (uint32_t tile = tb; tile < te; tile += TILE) {
                    const uint32_t tend = (tile + TILE < te) ? (tile + TILE) : te;
                    for (size_t j = 0; j < count; ++j) {
                        const uint32_t wi = indices ? indices[j] : static_cast<uint32_t>(j);
                        const int32_t v = eval_bfs7_range(f8_buf[wi].data(), tile, tend);
                        if constexpr (SIGN > 0) out_flat[wi * out_stride + out_col] += v;
                        else out_flat[wi * out_stride + out_col] -= v;
                    }
                }
                break;
            }
        }
    }

    static inline int32_t finish_bfs7(const Bfs7Hybrid& bt, const FastNode* __restrict tails,
                                      uint32_t sl, const int8_t* __restrict f8) {
        if ((bt.lm[sl >> 6] >> (sl & 63u)) & 1ULL) return bt.slot[sl];
        const FastNode* p = tails + bt.tail_base + static_cast<uint16_t>(bt.slot[sl]);
        while (const uint32_t rd = p->right_delta) {
            p += (f8[p->feat] <= p->thresh) ? 1u : rd;
        }
        return read_leaf(p);
    }

    inline int32_t eval_bfs7_range(const int8_t* __restrict f8, uint32_t tb, uint32_t te) const {
        const Bfs7Hybrid* __restrict tops = bfs7_trees_.data();
        const FastNode* __restrict tails = fast_nodes_.data();
        int32_t acc = 0;
        uint32_t t = tb;
        for (; t + 7u < te; t += 8u) {
            const Bfs7Hybrid& b0 = tops[t];      const Bfs7Hybrid& b1 = tops[t + 1u];
            const Bfs7Hybrid& b2 = tops[t + 2u]; const Bfs7Hybrid& b3 = tops[t + 3u];
            const Bfs7Hybrid& b4 = tops[t + 4u]; const Bfs7Hybrid& b5 = tops[t + 5u];
            const Bfs7Hybrid& b6 = tops[t + 6u]; const Bfs7Hybrid& b7 = tops[t + 7u];
            uint32_t i0 = 0, i1 = 0, i2 = 0, i3 = 0, i4 = 0, i5 = 0, i6 = 0, i7 = 0;
            for (int d = 0; d < 7; ++d) {
                const Split2 s0 = b0.sp[i0], s1 = b1.sp[i1], s2 = b2.sp[i2], s3 = b3.sp[i3];
                const Split2 s4 = b4.sp[i4], s5 = b5.sp[i5], s6 = b6.sp[i6], s7 = b7.sp[i7];
                i0 = (i0 << 1) + 1u + static_cast<uint32_t>(f8[s0.feat] > s0.thresh);
                i1 = (i1 << 1) + 1u + static_cast<uint32_t>(f8[s1.feat] > s1.thresh);
                i2 = (i2 << 1) + 1u + static_cast<uint32_t>(f8[s2.feat] > s2.thresh);
                i3 = (i3 << 1) + 1u + static_cast<uint32_t>(f8[s3.feat] > s3.thresh);
                i4 = (i4 << 1) + 1u + static_cast<uint32_t>(f8[s4.feat] > s4.thresh);
                i5 = (i5 << 1) + 1u + static_cast<uint32_t>(f8[s5.feat] > s5.thresh);
                i6 = (i6 << 1) + 1u + static_cast<uint32_t>(f8[s6.feat] > s6.thresh);
                i7 = (i7 << 1) + 1u + static_cast<uint32_t>(f8[s7.feat] > s7.thresh);
            }
            acc += finish_bfs7(b0, tails, i0 - 127u, f8) + finish_bfs7(b1, tails, i1 - 127u, f8) +
                   finish_bfs7(b2, tails, i2 - 127u, f8) + finish_bfs7(b3, tails, i3 - 127u, f8) +
                   finish_bfs7(b4, tails, i4 - 127u, f8) + finish_bfs7(b5, tails, i5 - 127u, f8) +
                   finish_bfs7(b6, tails, i6 - 127u, f8) + finish_bfs7(b7, tails, i7 - 127u, f8);
        }
        for (; t < te; ++t) {
            const Bfs7Hybrid& bt = tops[t];
            uint32_t idx = 0;
            for (int d = 0; d < 7; ++d) {
                const Split2 s = bt.sp[idx];
                idx = (idx << 1) + 1u + static_cast<uint32_t>(f8[s.feat] > s.thresh);
            }
            acc += finish_bfs7(bt, tails, idx - 127u, f8);
        }
        return acc;
    }

    template <typename TreeT, int DEPTH>
    void fill_bfs_one_ensemble(std::vector<TreeT>& dst, const std::vector<std::vector<uint32_t>>& per_c_trees, uint32_t nt, uint32_t nc) {
        constexpr int LEAF_BASE = 1 << DEPTH;
        dst.resize(nt);
        uint32_t out_t = 0;
        for (uint32_t c = 0; c < nc; ++c) {
            class_tree_off_[c] = out_t;
            for (uint32_t orig_t : per_c_trees[c]) {
                TreeT& bt = dst[out_t++];
                bt.sp[0] = {0, 127};
                auto fill = [&](auto&& self, uint32_t oi, int bi, int d) -> void {
                    const uint32_t nd = nodes_v5_[oi];
                    if (d == DEPTH) {
                        bt.leaf[bi - LEAF_BASE] = static_cast<int16_t>(nd & 0xFFFFu);
                        return;
                    }
                    if (nd >> 31) {
                        // Early leaf: dummy split {0, 127} guarantees (f8[0] > 127) == false -> always left!
                        bt.sp[bi] = {0, 127};
                        self(self, oi, bi * 2, d + 1);
                        self(self, oi, bi * 2 + 1, d + 1);
                        return;
                    }
                    const uint32_t rd = nd & 0x1FFFFu;
                    bt.sp[bi] = {
                        static_cast<uint8_t>((nd >> 17) & 0x7Fu),
                        static_cast<int8_t>(static_cast<int32_t>((nd >> 24) & 0x7Fu) - 8)
                    };
                    self(self, oi + 1u, bi * 2, d + 1);
                    self(self, oi + rd, bi * 2 + 1, d + 1);
                };
                fill(fill, tree_roots_[orig_t], 1, 0);
            }
        }
        class_tree_off_[nc] = out_t;
    }

    void fill_bfs9_sub4_ensemble(const std::vector<std::vector<uint32_t>>& per_c_trees, uint32_t nt, uint32_t nc) {
        bfs9_top5_.resize(nt);
        bfs9_sub4_.resize(static_cast<size_t>(nt) * 32u);
        uint32_t out_t = 0;
        for (uint32_t c = 0; c < nc; ++c) {
            class_tree_off_[c] = out_t;
            for (uint32_t orig_t : per_c_trees[c]) {
                Bfs9Top5& tp = bfs9_top5_[out_t];
                Bfs9Sub4* sb_base = &bfs9_sub4_[static_cast<size_t>(out_t) * 32u];
                ++out_t;
                tp.sp[0] = {0, 127};

                auto fill_sub4 = [&](auto&& self, uint32_t oi, Bfs9Sub4& sb, int bi, int d) -> void {
                    const uint32_t nd = nodes_v5_[oi];
                    if (d == 4) {
                        sb.leaf[bi - 16] = static_cast<int16_t>(nd & 0xFFFFu);
                        return;
                    }
                    if (nd >> 31) {
                        sb.sp[bi] = {0, 127};
                        self(self, oi, sb, bi * 2, d + 1);
                        self(self, oi, sb, bi * 2 + 1, d + 1);
                        return;
                    }
                    const uint32_t rd = nd & 0x1FFFFu;
                    sb.sp[bi] = {
                        static_cast<uint8_t>((nd >> 17) & 0x7Fu),
                        static_cast<int8_t>(static_cast<int32_t>((nd >> 24) & 0x7Fu) - 8)
                    };
                    self(self, oi + 1u, sb, bi * 2, d + 1);
                    self(self, oi + rd, sb, bi * 2 + 1, d + 1);
                };

                auto fill_top = [&](auto&& self, uint32_t oi, int bi, int d) -> void {
                    const uint32_t nd = nodes_v5_[oi];
                    if (d == 5) {
                        Bfs9Sub4& sb = sb_base[bi - 32];
                        sb.sp[0] = {0, 127};
                        fill_sub4(fill_sub4, oi, sb, 1, 0);
                        return;
                    }
                    if (nd >> 31) {
                        tp.sp[bi] = {0, 127};
                        self(self, oi, bi * 2, d + 1);
                        self(self, oi, bi * 2 + 1, d + 1);
                        return;
                    }
                    const uint32_t rd = nd & 0x1FFFFu;
                    tp.sp[bi] = {
                        static_cast<uint8_t>((nd >> 17) & 0x7Fu),
                        static_cast<int8_t>(static_cast<int32_t>((nd >> 24) & 0x7Fu) - 8)
                    };
                    self(self, oi + 1u, bi * 2, d + 1);
                    self(self, oi + rd, bi * 2 + 1, d + 1);
                };
                fill_top(fill_top, tree_roots_[orig_t], 1, 0);
            }
        }
        class_tree_off_[nc] = out_t;
    }

    void build_fast_v5() {
        const uint32_t nt = header_->num_trees;
        const uint32_t nc = header_->num_classes;
        const uint32_t total = header_->num_nodes;
        if (nt == 0 || total == 0 || nc == 0 || nc > NUM_DIACRITICS) return;

        uint32_t max_depth_all = 0;
        uint32_t max_feat_all = 0;
        struct ValCtx {
            const uint32_t* src;
            uint32_t total;
            uint32_t nc;
            int32_t cid;
            uint32_t max_d;
            uint32_t max_f;
            int32_t mn_leaf;
            int32_t mx_leaf;
            bool ok;
            void walk(uint32_t idx, uint32_t d) {
                if (!ok || idx >= total || d > 64u) { ok = false; return; }
                if (d > max_d) max_d = d;
                const uint32_t nd = src[idx];
                if (nd >> 31) {
                    const uint32_t c = (nd >> 26) & 0x1Fu;
                    if (c >= nc) { ok = false; return; }
                    if (cid < 0) cid = static_cast<int32_t>(c);
                    else if (cid != static_cast<int32_t>(c)) { ok = false; return; }
                    const int32_t lv = static_cast<int16_t>(nd & 0xFFFFu);
                    if (lv < mn_leaf) mn_leaf = lv;
                    if (lv > mx_leaf) mx_leaf = lv;
                    return;
                }
                const uint32_t feat = (nd >> 17) & 0x7Fu;
                const int32_t thresh = static_cast<int32_t>((nd >> 24) & 0x7Fu) - 8;
                const uint32_t rd = nd & 0x1FFFFu;
                if (feat >= 128u || thresh < -8 || thresh > 119 || rd == 0u ||
                    idx + 1u >= total || idx + rd >= total) {
                    ok = false;
                    return;
                }
                if (feat > max_f) max_f = feat;
                walk(idx + 1u, d + 1u);
                walk(idx + rd, d + 1u);
            }
        };

        std::vector<std::vector<uint32_t>> per_c_trees(nc);
        std::vector<int32_t> orig_span(nt, 0);
        ValCtx vc{nodes_v5_, total, nc, -1, 0, 0, INT32_MAX, INT32_MIN, true};
        for (uint32_t t = 0; t < nt; ++t) {
            vc.cid = -1;
            vc.max_d = 0;
            vc.mn_leaf = INT32_MAX;
            vc.mx_leaf = INT32_MIN;
            vc.walk(tree_roots_[t], 0u);
            if (!vc.ok || vc.cid < 0) return;
            per_c_trees[static_cast<size_t>(vc.cid)].push_back(t);
            orig_span[t] = (vc.mx_leaf >= vc.mn_leaf) ? (vc.mx_leaf - vc.mn_leaf) : 0;
            if (vc.max_d > max_depth_all) max_depth_all = vc.max_d;
            if (vc.max_f > max_feat_all) max_feat_all = vc.max_f;
        }
        used_feats_ = std::min<uint32_t>(128u, max_feat_all + 1u);

        tree_span_.resize(nt);
        {
            uint32_t sp_i = 0;
            for (uint32_t c = 0; c < nc; ++c) {
                for (uint32_t orig_t : per_c_trees[c]) {
                    tree_span_[sp_i++] = orig_span[orig_t];
                }
            }
        }

        if (max_depth_all <= 6u) {
            fill_bfs_one_ensemble<Bfs6One, 6>(bfs6_trees_, per_c_trees, nt, nc);
            fast_kind_ = 1;
            return;
        }
        if (max_depth_all == 7u) {
            fill_bfs_one_ensemble<Bfs7One, 7>(bfs7_one_trees_, per_c_trees, nt, nc);
            fast_kind_ = 2;
            return;
        }
        if (max_depth_all == 8u) {
            fill_bfs_one_ensemble<Bfs8One, 8>(bfs8_trees_, per_c_trees, nt, nc);
            fast_kind_ = 3;
            return;
        }
        if (max_depth_all == 9u) {
            fill_bfs9_sub4_ensemble(per_c_trees, nt, nc);
            fast_kind_ = 4;
            return;
        }

        // Depth > 9 fallback: Bfs7Hybrid + contiguous per-class FastNode tails
        bfs7_trees_.resize(nt);
        fast_nodes_.reserve(total / 2 + 64);
        bool ok_tails = true;
        auto emit_tail = [&](auto&& self, uint32_t oi) -> uint32_t {
            const uint32_t nd = nodes_v5_[oi];
            const uint32_t pos = static_cast<uint32_t>(fast_nodes_.size());
            if (nd >> 31) {
                fast_nodes_.push_back(make_leaf(static_cast<int16_t>(nd & 0xFFFFu)));
                return pos;
            }
            const uint32_t orig_rd = nd & 0x1FFFFu;
            FastNode fn{};
            fn.right_delta = 0;
            fn.feat = static_cast<uint8_t>((nd >> 17) & 0x7Fu);
            fn.thresh = static_cast<int8_t>(static_cast<int32_t>((nd >> 24) & 0x7Fu) - 8);
            fast_nodes_.push_back(fn);
            self(self, oi + 1u);
            const uint32_t rpos = static_cast<uint32_t>(fast_nodes_.size());
            const uint32_t new_rd = rpos - pos;
            if (new_rd < 2u || new_rd > 65535u) { ok_tails = false; return pos; }
            fast_nodes_[pos].right_delta = static_cast<uint16_t>(new_rd);
            self(self, oi + orig_rd);
            return pos;
        };

        uint32_t out_t = 0;
        for (uint32_t c = 0; c < nc && ok_tails; ++c) {
            class_tree_off_[c] = out_t;
            for (uint32_t orig_t : per_c_trees[c]) {
                Bfs7Hybrid& bt = bfs7_trees_[out_t++];
                bt.tail_base = static_cast<uint32_t>(fast_nodes_.size());
                bt.lm[0] = bt.lm[1] = 0;
                auto fill = [&](auto&& self, uint32_t oi, int bi, int d) -> void {
                    if (!ok_tails) return;
                    const uint32_t nd = nodes_v5_[oi];
                    if (d == 7) {
                        const int sl = bi - 128;
                        if (nd >> 31) {
                            bt.lm[sl >> 6] |= (1ULL << (sl & 63));
                            bt.slot[sl] = static_cast<int16_t>(nd & 0xFFFFu);
                        } else {
                            const uint32_t pos = emit_tail(emit_tail, oi);
                            const uint32_t off = pos - bt.tail_base;
                            if (off > 65535u) { ok_tails = false; return; }
                            bt.slot[sl] = static_cast<int16_t>(static_cast<uint16_t>(off));
                        }
                        return;
                    }
                    if (nd >> 31) {
                        bt.sp[bi - 1] = {0, 127};
                        self(self, oi, bi * 2, d + 1);
                        self(self, oi, bi * 2 + 1, d + 1);
                        return;
                    }
                    const uint32_t rd = nd & 0x1FFFFu;
                    bt.sp[bi - 1] = {
                        static_cast<uint8_t>((nd >> 17) & 0x7Fu),
                        static_cast<int8_t>(static_cast<int32_t>((nd >> 24) & 0x7Fu) - 8)
                    };
                    self(self, oi + 1u, bi * 2, d + 1);
                    self(self, oi + rd, bi * 2 + 1, d + 1);
                };
                fill(fill, tree_roots_[orig_t], 1, 0);
                if (!ok_tails) break;
            }
        }
        if (!ok_tails) {
            bfs7_trees_.clear();
            fast_nodes_.clear();
            tree_span_.clear();
            return;
        }
        class_tree_off_[nc] = out_t;
        fast_kind_ = 5;
    }

    inline void predict_scores_fallback(const int32_t* features, size_t num_features, int32_t* out_scores) const {
        const uint32_t nc = header_->num_classes;
        for (uint32_t c = 0; c < nc; ++c) out_scores[c] = header_->base_score_q;
        const uint32_t nt = header_->num_trees;
        const uint32_t total = header_->num_nodes;
        if (mode_ == 2) {
            for (uint32_t t = 0; t < nt; ++t) {
                uint32_t idx = tree_roots_[t];
                while (idx < total) {
                    const uint32_t node = nodes_v5_[idx];
                    if (node >> 31) {
                        uint32_t cid = (node >> 26) & 0x1Fu;
                        if (cid < nc) out_scores[cid] += static_cast<int16_t>(node & 0xFFFFu);
                        break;
                    }
                    uint32_t feat = (node >> 17) & 0x7Fu;
                    int32_t thresh = static_cast<int32_t>((node >> 24) & 0x7Fu) - 8;
                    int32_t f = (feat < num_features) ? features[feat] : -1;
                    idx += (f <= thresh) ? 1u : (node & 0x1FFFFu);
                }
            }
            return;
        }
        if (mode_ == 1) {
            for (uint32_t t = 0; t < nt; ++t) {
                uint32_t idx = tree_roots_[t];
                uint32_t guard = 0;
                while (idx < total && guard++ < 256) {
                    const FlatTreeNodeV3& node = nodes_v3_[idx];
                    uint8_t kind = node.kind_class >> 4;
                    uint8_t cid = node.kind_class & 0x0F;
                    if (kind == NODE_LEAF) {
                        if (cid < nc) out_scores[cid] += node.threshold;
                        break;
                    }
                    int32_t f = (node.feature_idx < num_features) ? features[node.feature_idx] : -1;
                    idx = static_cast<uint32_t>(f <= node.threshold ? node.left_child : node.right_child);
                }
            }
            return;
        }
        for (uint32_t t = 0; t < nt; ++t) {
            uint32_t idx = tree_roots_[t];
            uint32_t guard = 0;
            while (idx < total && guard++ < 256) {
                const FlatTreeNode& node = nodes_v2_[idx];
                if (node.kind == NODE_LEAF) {
                    if (node.class_id < nc) out_scores[node.class_id] += node.threshold;
                    break;
                }
                int32_t f = (node.feature_idx < num_features) ? features[node.feature_idx] : -1;
                bool left;
                if (node.kind == NODE_CAT) {
                    left = (f >= 0 && f < 64) && ((node.cat_mask >> f) & 1ULL);
                } else {
                    left = f <= node.threshold;
                }
                idx = static_cast<uint32_t>(left ? node.left_child : node.right_child);
            }
        }
    }

    const TreeEnsembleHeader* header_ = nullptr;
    const uint32_t* tree_roots_ = nullptr;
    const FlatTreeNode* nodes_v2_ = nullptr;
    const FlatTreeNodeV3* nodes_v3_ = nullptr;
    const uint32_t* nodes_v5_ = nullptr;
    std::vector<Bfs6One> bfs6_trees_;
    std::vector<Bfs7One> bfs7_one_trees_;
    std::vector<Bfs8One> bfs8_trees_;
    std::vector<Bfs9Top5> bfs9_top5_;
    std::vector<Bfs9Sub4> bfs9_sub4_;
    std::vector<Bfs7Hybrid> bfs7_trees_;
    std::vector<FastNode> fast_nodes_;
    std::vector<int32_t> tree_span_;
    uint32_t class_tree_off_[NUM_DIACRITICS + 1] = {};
    uint32_t used_feats_ = 128;
    uint8_t mode_ = 0;
    uint8_t fast_kind_ = 0; // 0=fallback, 1=Bfs6One, 2=Bfs7One, 3=Bfs8One, 4=Bfs9Sub4, 5=Bfs7Hybrid
    bool valid_ = false;
};

} // namespace tashkeel
