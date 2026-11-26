#pragma once

#include "format.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace tashkeel {

struct NeuralIrabTokInput {
    uint32_t word_id;
    uint16_t hashes[4];
    uint8_t part_id;
    uint8_t pos_id;
    uint8_t hard_boundary; // 1 if start of a new hard sentence (BK_HARD or idx==0)
    uint8_t need_pron;     // 1 iff n>=2, last char is S_KAF/S_HEH, and any candidate has CF_SUFFIX
    float dense[48];
};

struct NeuralIrabWorkspace {
    std::vector<float> h_cur;     // [L, D]
    std::vector<float> h_tmp;     // [L, D]
    std::vector<float> h_norm;    // [L, D]
    std::vector<float> qkv;       // [L, 2*D] (glru_in: [u_fwd, u_bwd, g_fwd, g_bwd])
    std::vector<float> attn_out;  // [L, D]   (glru_rec output / conv_pw out)
    std::vector<float> ffn_mid;   // [L, D_FF]
    std::vector<float> attn_sc;   // [L]      (per-token act scales)
    std::vector<uint8_t> act_u8;  // [L * max(D, D_FF)]
    std::vector<float> end_logp;  // [W, 15]
    std::vector<float> fam_logp;  // [W, 5]
    std::vector<float> tan_logp;  // [W, 2]
    std::vector<float> pron_logp; // [W, 4]
};

// BARQ-Lightning V8 (LightningBiIrab, NRAB version=2):
// Pure O(L) L2-resident Dilated Depthwise-Conv + Bidirectional Gated Linear Recurrence (Bi-GLRU)
// with tiled 4-token AVX2 INT8 GEMM (zero exp/tanh in backbone).
class NeuralIrabModel {
public:
    static constexpr size_t D_WORD = 32;
    static constexpr size_t D_HASH = 32;
    static constexpr size_t D_FEAT = 32;
    static constexpr size_t D_PART = 16;
    static constexpr size_t D_POS  = 16;
    static constexpr size_t D_FEAT_IN = 48 + D_PART + D_POS; // 80
    static constexpr size_t D_MODEL_V2 = D_WORD + D_HASH + D_FEAT; // 96
    static constexpr size_t D_REC_V2 = 48;
    static constexpr size_t MAX_CHUNK = 256;
    static constexpr size_t CHUNK_STRIDE = 224;

    struct QuantLinear {
        size_t in_dim = 0;
        size_t out_dim = 0;
        const int8_t* w = nullptr;        // [out_dim, in_dim], values clamped to [-64, 63]
        const float* scale = nullptr;     // [out_dim]
        const int32_t* zp_sum = nullptr;  // [out_dim] = 128 * sum_k w[r, k]
        const float* bias = nullptr;      // [out_dim]
    };

    struct Layer {
        const float* norm1_g = nullptr;
        const float* dw_conv_w = nullptr; // [CONV_K, D]
        const float* dw_conv_b = nullptr; // [D]
        QuantLinear conv_pw;              // D -> D
        const float* norm2_g = nullptr;
        QuantLinear glru_in;              // D -> 2*D ([u_fwd, u_bwd, g_fwd, g_bwd])
        QuantLinear glru_out;             // D -> D
        const float* norm3_g = nullptr;
        QuantLinear ffn_up;               // D -> D_FF
        QuantLinear ffn_down;             // D_FF -> D
    };

    void clear() {
        valid_ = false;
        layers_.clear();
        wid_to_row_ = nullptr;
        crf_trans_ = nullptr;
    }

    bool valid() const { return valid_; }
    const float* crf_trans() const { return crf_trans_; }

    bool init(const uint8_t* blob, size_t blob_size) {
        clear();
        if (!blob || blob_size < sizeof(NeuralIrabHeader)) return false;
        const auto* hdr = reinterpret_cast<const NeuralIrabHeader*>(blob);
        if (hdr->magic != NEURAL_IRAB_MAGIC || hdr->version != 2) return false;
        d_model_ = hdr->d_model;
        n_layers_ = hdr->n_layers;
        d_rec_ = hdr->n_heads;
        d_ff_ = hdr->d_ff;
        conv_k_ = hdr->conv_kernel;
        vocab_size_ = hdr->vocab_size;
        word_emb_rows_ = hdr->word_emb_rows;
        hash_buckets_ = hdr->hash_buckets;
        n_dense_ = hdr->n_dense_feat;

        if (d_model_ != D_MODEL_V2 || d_rec_ != D_REC_V2 || (d_ff_ % 32) != 0) return false;
        if (conv_k_ != 5 || n_dense_ != 48 || n_layers_ == 0) return false;

        size_t off = 64;
        auto align64 = [](size_t x) -> size_t { return (x + 63u) & ~size_t(63u); };
        auto take = [&](size_t bytes) -> const uint8_t* {
            off = align64(off);
            if (off > blob_size || bytes > blob_size - off) return nullptr;
            const uint8_t* p = blob + off;
            off += bytes;
            return p;
        };
        auto take_f32 = [&](size_t cnt) -> const float* {
            return reinterpret_cast<const float*>(take(cnt * sizeof(float)));
        };
        auto take_ql = [&](QuantLinear& ql, size_t in_d, size_t out_d) -> bool {
            ql.in_dim = in_d;
            ql.out_dim = out_d;
            ql.w = reinterpret_cast<const int8_t*>(take(out_d * in_d));
            ql.scale = take_f32(out_d);
            ql.zp_sum = reinterpret_cast<const int32_t*>(take(out_d * sizeof(int32_t)));
            ql.bias = take_f32(out_d);
            return ql.w && ql.scale && ql.zp_sum && ql.bias;
        };

        wid_to_row_ = reinterpret_cast<const uint16_t*>(take(static_cast<size_t>(vocab_size_) * sizeof(uint16_t)));
        const float* ws = take_f32(1);
        word_emb_ = reinterpret_cast<const int8_t*>(take(static_cast<size_t>(word_emb_rows_) * D_WORD));
        const float* hs = take_f32(1);
        hash_emb_ = reinterpret_cast<const int8_t*>(take(static_cast<size_t>(hash_buckets_) * D_HASH));
        part_emb_ = take_f32(32 * D_PART);
        pos_emb_  = take_f32(16 * D_POS);
        feat_w_   = take_f32(D_FEAT * D_FEAT_IN);
        feat_b_   = take_f32(D_FEAT);
        if (!wid_to_row_ || !ws || !word_emb_ || !hs || !hash_emb_ || !part_emb_ || !pos_emb_ || !feat_w_ || !feat_b_) {
            clear();
            return false;
        }
        word_emb_scale_ = *ws;
        hash_emb_scale_ = *hs;

        layers_.resize(n_layers_);
        for (size_t l = 0; l < n_layers_; ++l) {
            Layer& ly = layers_[l];
            ly.norm1_g = take_f32(d_model_);
            ly.dw_conv_w = take_f32(conv_k_ * d_model_);
            ly.dw_conv_b = take_f32(d_model_);
            if (!ly.norm1_g || !ly.dw_conv_w || !ly.dw_conv_b) { clear(); return false; }
            if (!take_ql(ly.conv_pw, d_model_, d_model_)) { clear(); return false; }
            ly.norm2_g = take_f32(d_model_);
            if (!ly.norm2_g) { clear(); return false; }
            if (!take_ql(ly.glru_in, d_model_, 2 * d_model_)) { clear(); return false; }
            if (!take_ql(ly.glru_out, d_model_, d_model_)) { clear(); return false; }
            ly.norm3_g = take_f32(d_model_);
            if (!ly.norm3_g) { clear(); return false; }
            if (!take_ql(ly.ffn_up, d_model_, d_ff_)) { clear(); return false; }
            if (!take_ql(ly.ffn_down, d_ff_, d_model_)) { clear(); return false; }
        }

        norm_final_g_ = take_f32(d_model_);
        w_end_  = take_f32(NUM_DIACRITICS * d_model_);
        b_end_  = take_f32(NUM_DIACRITICS);
        w_fam_  = take_f32(5 * d_model_);
        b_fam_  = take_f32(5);
        w_tan_  = take_f32(2 * d_model_);
        b_tan_  = take_f32(2);
        w_pron_ = take_f32(4 * d_model_);
        b_pron_ = take_f32(4);
        crf_trans_ = take_f32(NUM_DIACRITICS * NUM_DIACRITICS);

        if (!norm_final_g_ || !w_end_ || !b_end_ || !w_fam_ || !b_fam_ || !w_tan_ || !b_tan_ || !w_pron_ || !b_pron_ || !crf_trans_) {
            clear();
            return false;
        }
        valid_ = true;
        return true;
    }

    void forward(const NeuralIrabTokInput* toks, size_t W, NeuralIrabWorkspace& ws) const {
        if (!valid_ || W == 0) return;
        ws.end_logp.resize(W * NUM_DIACRITICS);
        ws.fam_logp.resize(W * 5);
        ws.tan_logp.resize(W * 2);
        ws.pron_logp.resize(W * 4);

        size_t s = 0;
        while (s < W) {
            size_t e = s + 1;
            while (e < W && !toks[e].hard_boundary) ++e;
            const size_t seg_len = e - s;
            if (seg_len <= MAX_CHUNK) {
                forward_segment(toks + s, seg_len, 0, seg_len, s, ws);
            } else {
                size_t cur = 0;
                while (cur < seg_len) {
                    size_t wlen = std::min<size_t>(MAX_CHUNK, seg_len - cur);
                    size_t keep_s = (cur == 0) ? 0 : (MAX_CHUNK - CHUNK_STRIDE) / 2;
                    size_t keep_e = (cur + wlen >= seg_len) ? wlen : (wlen - (MAX_CHUNK - CHUNK_STRIDE) / 2);
                    forward_segment(toks + s + cur, wlen, keep_s, keep_e, s + cur, ws);
                    if (cur + wlen >= seg_len) break;
                    cur += CHUNK_STRIDE;
                }
            }
            s = e;
        }
    }

private:
    static inline float relu6_scalar(float x) {
        return x <= 0.0f ? 0.0f : (x >= 6.0f ? 6.0f : x);
    }

    static inline void relu6_inplace(float* x, size_t n) {
#if defined(__AVX2__)
        const __m256 z = _mm256_setzero_ps();
        const __m256 six = _mm256_set1_ps(6.0f);
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            __m256 v = _mm256_loadu_ps(x + i);
            v = _mm256_min_ps(six, _mm256_max_ps(z, v));
            _mm256_storeu_ps(x + i, v);
        }
        for (; i < n; ++i) x[i] = relu6_scalar(x[i]);
#elif defined(__ARM_NEON) && defined(__aarch64__)
        const float32x4_t z = vdupq_n_f32(0.0f);
        const float32x4_t six = vdupq_n_f32(6.0f);
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            float32x4_t v = vld1q_f32(x + i);
            v = vminq_f32(six, vmaxq_f32(z, v));
            vst1q_f32(x + i, v);
        }
        for (; i < n; ++i) x[i] = relu6_scalar(x[i]);
#else
        for (size_t i = 0; i < n; ++i) x[i] = relu6_scalar(x[i]);
#endif
    }

    static inline void rmsnorm_batch(const float* X, const float* g, float* Y, size_t L, size_t D) {
#if defined(__AVX2__)
        for (size_t t = 0; t < L; ++t) {
            const float* x = X + t * D;
            float* y = Y + t * D;
            __m256 acc = _mm256_setzero_ps();
            for (size_t i = 0; i < D; i += 8) {
                __m256 v = _mm256_loadu_ps(x + i);
                acc = _mm256_fmadd_ps(v, v, acc);
            }
            __m128 lo = _mm256_castps256_ps128(acc);
            __m128 hi = _mm256_extractf128_ps(acc, 1);
            __m128 s = _mm_add_ps(lo, hi);
            s = _mm_hadd_ps(s, s);
            s = _mm_hadd_ps(s, s);
            float ss = _mm_cvtss_f32(s);
            __m256 vinv = _mm256_set1_ps(1.0f / std::sqrt(ss / static_cast<float>(D) + 1e-5f));
            for (size_t i = 0; i < D; i += 8) {
                __m256 v = _mm256_loadu_ps(x + i);
                __m256 vg = _mm256_loadu_ps(g + i);
                _mm256_storeu_ps(y + i, _mm256_mul_ps(_mm256_mul_ps(v, vinv), vg));
            }
        }
#elif defined(__ARM_NEON) && defined(__aarch64__)
        for (size_t t = 0; t < L; ++t) {
            const float* x = X + t * D;
            float* y = Y + t * D;
            float32x4_t acc = vdupq_n_f32(0.0f);
            for (size_t i = 0; i < D; i += 4) {
                float32x4_t v = vld1q_f32(x + i);
                acc = vfmaq_f32(acc, v, v);
            }
            float ss = vaddvq_f32(acc);
            float32x4_t vinv = vdupq_n_f32(1.0f / std::sqrt(ss / static_cast<float>(D) + 1e-5f));
            for (size_t i = 0; i < D; i += 4) {
                float32x4_t v = vld1q_f32(x + i);
                float32x4_t vg = vld1q_f32(g + i);
                vst1q_f32(y + i, vmulq_f32(vmulq_f32(v, vinv), vg));
            }
        }
#else
        for (size_t t = 0; t < L; ++t) {
            const float* x = X + t * D;
            float* y = Y + t * D;
            float ss = 0.0f;
            for (size_t i = 0; i < D; ++i) ss += x[i] * x[i];
            float inv = 1.0f / std::sqrt(ss / static_cast<float>(D) + 1e-5f);
            for (size_t i = 0; i < D; ++i) y[i] = x[i] * inv * g[i];
        }
#endif
    }

    static inline float fast_exp_neg(float x) {
        if (x < -18.0f) return 0.0f;
        const float kf = std::floor(x * 1.4426950408889634f + 0.5f);
        const int32_t k = static_cast<int32_t>(kf);
        const float r = x - kf * 0.6931471805599453f;
        float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + r * 0.041666668f)));
        int32_t bits = 0;
        std::memcpy(&bits, &p, sizeof(bits));
        bits += (k << 23);
        std::memcpy(&p, &bits, sizeof(p));
        return p;
    }

    static inline void log_softmax_row(const float* logits, float* out, size_t C) {
        float mx = logits[0];
        for (size_t i = 1; i < C; ++i) if (logits[i] > mx) mx = logits[i];
        float sum = 0.0f;
        for (size_t i = 0; i < C; ++i) sum += fast_exp_neg(logits[i] - mx);
        float lse = mx + std::log(sum);
        for (size_t i = 0; i < C; ++i) out[i] = logits[i] - lse;
    }

    static inline void quantize_batch_u8(const float* X, size_t L, size_t in_d, uint8_t* X_u8, float* act_scale) {
#if defined(__AVX2__)
        const __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
        const __m256i i_min = _mm256_set1_epi32(-128);
        const __m256i i_max = _mm256_set1_epi32(127);
        const __m256i i_off = _mm256_set1_epi32(128);
        const __m256i perm = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);

        for (size_t t = 0; t < L; ++t) {
            const float* x = X + t * in_d;
            uint8_t* xu = X_u8 + t * in_d;

            __m256 vmax = _mm256_setzero_ps();
            for (size_t k = 0; k < in_d; k += 8) {
                __m256 a = _mm256_and_ps(_mm256_loadu_ps(x + k), abs_mask);
                vmax = _mm256_max_ps(vmax, a);
            }
            __m128 hi = _mm256_extractf128_ps(vmax, 1);
            __m128 lo = _mm256_castps256_ps128(vmax);
            __m128 m4 = _mm_max_ps(lo, hi);
            m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, _MM_SHUFFLE(1, 0, 3, 2)));
            m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, _MM_SHUFFLE(2, 3, 0, 1)));
            float max_abs = _mm_cvtss_f32(m4);

            if (max_abs < 1e-8f) {
                act_scale[t] = 0.0f;
                std::memset(xu, 128, in_d);
                continue;
            }
            act_scale[t] = max_abs / 127.0f;
            __m256 vinv = _mm256_set1_ps(127.0f / max_abs);

            for (size_t k = 0; k < in_d; k += 32) {
                __m256i q0 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(x + k + 0),  vinv));
                __m256i q1 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(x + k + 8),  vinv));
                __m256i q2 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(x + k + 16), vinv));
                __m256i q3 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(x + k + 24), vinv));
                q0 = _mm256_add_epi32(_mm256_min_epi32(i_max, _mm256_max_epi32(i_min, q0)), i_off);
                q1 = _mm256_add_epi32(_mm256_min_epi32(i_max, _mm256_max_epi32(i_min, q1)), i_off);
                q2 = _mm256_add_epi32(_mm256_min_epi32(i_max, _mm256_max_epi32(i_min, q2)), i_off);
                q3 = _mm256_add_epi32(_mm256_min_epi32(i_max, _mm256_max_epi32(i_min, q3)), i_off);
                __m256i p01 = _mm256_packs_epi32(q0, q1);
                __m256i p23 = _mm256_packs_epi32(q2, q3);
                __m256i packed = _mm256_packus_epi16(p01, p23);
                __m256i ordered = _mm256_permutevar8x32_epi32(packed, perm);
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(xu + k), ordered);
            }
        }
#elif defined(__ARM_NEON) && defined(__aarch64__)
        const float32x4_t zero = vdupq_n_f32(0.0f);
        for (size_t t = 0; t < L; ++t) {
            const float* x = X + t * in_d;
            uint8_t* xu = X_u8 + t * in_d;
            float32x4_t vmax = zero;
            for (size_t k = 0; k < in_d; k += 4) {
                vmax = vmaxq_f32(vmax, vabsq_f32(vld1q_f32(x + k)));
            }
            float max_abs = vmaxvq_f32(vmax);
            if (max_abs < 1e-8f) {
                act_scale[t] = 0.0f;
                std::memset(xu, 128, in_d);
                continue;
            }
            act_scale[t] = max_abs / 127.0f;
            float inv = 127.0f / max_abs;
            float32x4_t vinv = vdupq_n_f32(inv);
            int32x4_t i_min = vdupq_n_s32(-128);
            int32x4_t i_max = vdupq_n_s32(127);
            int32x4_t i_off = vdupq_n_s32(128);
            for (size_t k = 0; k < in_d; k += 8) {
                int32x4_t q0 = vcvtnq_s32_f32(vmulq_f32(vld1q_f32(x + k + 0), vinv));
                int32x4_t q1 = vcvtnq_s32_f32(vmulq_f32(vld1q_f32(x + k + 4), vinv));
                q0 = vaddq_s32(vminq_s32(i_max, vmaxq_s32(i_min, q0)), i_off);
                q1 = vaddq_s32(vminq_s32(i_max, vmaxq_s32(i_min, q1)), i_off);
                int16x8_t p16 = vcombine_s16(vqmovn_s32(q0), vqmovn_s32(q1));
                vst1_u8(xu + k, vqmovun_s16(p16));
            }
        }
#else
        for (size_t t = 0; t < L; ++t) {
            const float* x = X + t * in_d;
            uint8_t* xu = X_u8 + t * in_d;
            float max_abs = 0.0f;
            for (size_t k = 0; k < in_d; ++k) {
                float a = std::fabs(x[k]);
                if (a > max_abs) max_abs = a;
            }
            if (max_abs < 1e-8f) {
                act_scale[t] = 0.0f;
                std::memset(xu, 128, in_d);
                continue;
            }
            act_scale[t] = max_abs / 127.0f;
            float inv = 127.0f / max_abs;
            for (size_t k = 0; k < in_d; ++k) {
                int q = static_cast<int>(std::lrintf(x[k] * inv));
                if (q < -128) q = -128;
                else if (q > 127) q = 127;
                xu[k] = static_cast<uint8_t>(q + 128);
            }
        }
#endif
    }

    static inline void rmsnorm_quantize_u8(const float* X, const float* g, uint8_t* X_u8, float* act_scale,
                                           size_t L, size_t D) {
        if (D > 128) {
            for (size_t t = 0; t < L; ++t) {
                const float* x = X + t * D;
                uint8_t* xu = X_u8 + t * D;
                float ss = 0.0f;
                for (size_t k = 0; k < D; ++k) ss += x[k] * x[k];
                const float inv_rms = 1.0f / std::sqrt(ss / static_cast<float>(D) + 1e-5f);
                float max_abs = 0.0f;
                for (size_t k = 0; k < D; ++k) {
                    float a = std::fabs((x[k] * inv_rms) * g[k]);
                    if (a > max_abs) max_abs = a;
                }
                if (max_abs < 1e-8f) {
                    act_scale[t] = 0.0f;
                    std::memset(xu, 128, D);
                    continue;
                }
                act_scale[t] = max_abs / 127.0f;
                const float inv = 127.0f / max_abs;
                for (size_t k = 0; k < D; ++k) {
                    int q = static_cast<int>(std::lrintf(((x[k] * inv_rms) * g[k]) * inv));
                    if (q < -128) q = -128;
                    else if (q > 127) q = 127;
                    xu[k] = static_cast<uint8_t>(q + 128);
                }
            }
            return;
        }
        alignas(32) float yn_buf[128];
#if defined(__AVX2__)
        const __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
        const __m256i i_min = _mm256_set1_epi32(-128);
        const __m256i i_max = _mm256_set1_epi32(127);
        const __m256i i_off = _mm256_set1_epi32(128);
        const __m256i perm = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);

        for (size_t t = 0; t < L; ++t) {
            const float* x = X + t * D;
            uint8_t* xu = X_u8 + t * D;
            __m256 acc = _mm256_setzero_ps();
            for (size_t k = 0; k < D; k += 8) {
                __m256 v = _mm256_loadu_ps(x + k);
                acc = _mm256_fmadd_ps(v, v, acc);
            }
            __m128 lo = _mm256_castps256_ps128(acc);
            __m128 hi = _mm256_extractf128_ps(acc, 1);
            __m128 s = _mm_add_ps(lo, hi);
            s = _mm_hadd_ps(s, s);
            s = _mm_hadd_ps(s, s);
            const float inv_rms = 1.0f / std::sqrt(_mm_cvtss_f32(s) / static_cast<float>(D) + 1e-5f);
            const __m256 vinv_rms = _mm256_set1_ps(inv_rms);

            __m256 vmax = _mm256_setzero_ps();
            for (size_t k = 0; k < D; k += 8) {
                __m256 yn = _mm256_mul_ps(_mm256_mul_ps(_mm256_loadu_ps(x + k), vinv_rms), _mm256_loadu_ps(g + k));
                _mm256_store_ps(yn_buf + k, yn);
                vmax = _mm256_max_ps(vmax, _mm256_and_ps(yn, abs_mask));
            }
            __m128 m4 = _mm_max_ps(_mm256_castps256_ps128(vmax), _mm256_extractf128_ps(vmax, 1));
            m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, _MM_SHUFFLE(1, 0, 3, 2)));
            m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, _MM_SHUFFLE(2, 3, 0, 1)));
            float max_abs = _mm_cvtss_f32(m4);
            if (max_abs < 1e-8f) {
                act_scale[t] = 0.0f;
                std::memset(xu, 128, D);
                continue;
            }
            act_scale[t] = max_abs / 127.0f;
            const __m256 vinv = _mm256_set1_ps(127.0f / max_abs);
            for (size_t k = 0; k < D; k += 32) {
                __m256i q0 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_load_ps(yn_buf + k + 0),  vinv));
                __m256i q1 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_load_ps(yn_buf + k + 8),  vinv));
                __m256i q2 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_load_ps(yn_buf + k + 16), vinv));
                __m256i q3 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_load_ps(yn_buf + k + 24), vinv));
                q0 = _mm256_add_epi32(_mm256_min_epi32(i_max, _mm256_max_epi32(i_min, q0)), i_off);
                q1 = _mm256_add_epi32(_mm256_min_epi32(i_max, _mm256_max_epi32(i_min, q1)), i_off);
                q2 = _mm256_add_epi32(_mm256_min_epi32(i_max, _mm256_max_epi32(i_min, q2)), i_off);
                q3 = _mm256_add_epi32(_mm256_min_epi32(i_max, _mm256_max_epi32(i_min, q3)), i_off);
                __m256i packed = _mm256_packus_epi16(_mm256_packs_epi32(q0, q1), _mm256_packs_epi32(q2, q3));
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(xu + k), _mm256_permutevar8x32_epi32(packed, perm));
            }
        }
#elif defined(__ARM_NEON) && defined(__aarch64__)
        const float32x4_t zero = vdupq_n_f32(0.0f);
        const int32x4_t i_min = vdupq_n_s32(-128), i_max = vdupq_n_s32(127), i_off = vdupq_n_s32(128);
        for (size_t t = 0; t < L; ++t) {
            const float* x = X + t * D;
            uint8_t* xu = X_u8 + t * D;
            float32x4_t acc = zero;
            for (size_t k = 0; k < D; k += 4) {
                float32x4_t v = vld1q_f32(x + k);
                acc = vfmaq_f32(acc, v, v);
            }
            const float inv_rms = 1.0f / std::sqrt(vaddvq_f32(acc) / static_cast<float>(D) + 1e-5f);
            const float32x4_t vinv_rms = vdupq_n_f32(inv_rms);
            float32x4_t vmax = zero;
            for (size_t k = 0; k < D; k += 4) {
                float32x4_t yn = vmulq_f32(vmulq_f32(vld1q_f32(x + k), vinv_rms), vld1q_f32(g + k));
                vst1q_f32(yn_buf + k, yn);
                vmax = vmaxq_f32(vmax, vabsq_f32(yn));
            }
            float max_abs = vmaxvq_f32(vmax);
            if (max_abs < 1e-8f) {
                act_scale[t] = 0.0f;
                std::memset(xu, 128, D);
                continue;
            }
            act_scale[t] = max_abs / 127.0f;
            const float32x4_t vinv = vdupq_n_f32(127.0f / max_abs);
            for (size_t k = 0; k < D; k += 8) {
                int32x4_t q0 = vcvtnq_s32_f32(vmulq_f32(vld1q_f32(yn_buf + k + 0), vinv));
                int32x4_t q1 = vcvtnq_s32_f32(vmulq_f32(vld1q_f32(yn_buf + k + 4), vinv));
                q0 = vaddq_s32(vminq_s32(i_max, vmaxq_s32(i_min, q0)), i_off);
                q1 = vaddq_s32(vminq_s32(i_max, vmaxq_s32(i_min, q1)), i_off);
                vst1_u8(xu + k, vqmovun_s16(vcombine_s16(vqmovn_s32(q0), vqmovn_s32(q1))));
            }
        }
#else
        for (size_t t = 0; t < L; ++t) {
            const float* x = X + t * D;
            uint8_t* xu = X_u8 + t * D;
            float ss = 0.0f;
            for (size_t k = 0; k < D; ++k) ss += x[k] * x[k];
            const float inv_rms = 1.0f / std::sqrt(ss / static_cast<float>(D) + 1e-5f);
            float max_abs = 0.0f;
            for (size_t k = 0; k < D; ++k) {
                float yn = (x[k] * inv_rms) * g[k];
                yn_buf[k] = yn;
                float a = std::fabs(yn);
                if (a > max_abs) max_abs = a;
            }
            if (max_abs < 1e-8f) {
                act_scale[t] = 0.0f;
                std::memset(xu, 128, D);
                continue;
            }
            act_scale[t] = max_abs / 127.0f;
            const float inv = 127.0f / max_abs;
            for (size_t k = 0; k < D; ++k) {
                int q = static_cast<int>(std::lrintf(yn_buf[k] * inv));
                if (q < -128) q = -128;
                else if (q > 127) q = 127;
                xu[k] = static_cast<uint8_t>(q + 128);
            }
        }
#endif
    }

#if defined(__AVX2__)
    static inline int32_t hsum_epi32(__m256i v) {
        __m128i lo = _mm256_castsi256_si128(v);
        __m128i hi = _mm256_extracti128_si256(v, 1);
        __m128i s = _mm_add_epi32(lo, hi);
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(1, 0, 3, 2)));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(2, 3, 0, 1)));
        return _mm_cvtsi128_si32(s);
    }
#endif

    // 4-token x 3-row tiled AVX2 INT8 GEMM operating on pre-quantized ws.act_u8 / ws.attn_sc
    static inline void qlinear_batch_preq(const QuantLinear& ql, float* Y, size_t L,
                                          NeuralIrabWorkspace& ws, bool accumulate_res = false) {
        const size_t in_d = ql.in_dim;
        const size_t out_d = ql.out_dim;

#if defined(__AVX2__)
        const __m256i ones16 = _mm256_set1_epi16(1);
        size_t t = 0;
        for (; t + 4 <= L; t += 4) {
            const uint8_t* x0 = ws.act_u8.data() + (t + 0) * in_d;
            const uint8_t* x1 = ws.act_u8.data() + (t + 1) * in_d;
            const uint8_t* x2 = ws.act_u8.data() + (t + 2) * in_d;
            const uint8_t* x3 = ws.act_u8.data() + (t + 3) * in_d;
            const float s0 = ws.attn_sc[t + 0];
            const float s1 = ws.attn_sc[t + 1];
            const float s2 = ws.attn_sc[t + 2];
            const float s3 = ws.attn_sc[t + 3];
            float* y0 = Y + (t + 0) * out_d;
            float* y1 = Y + (t + 1) * out_d;
            float* y2 = Y + (t + 2) * out_d;
            float* y3 = Y + (t + 3) * out_d;

            size_t r = 0;
            for (; r + 3 <= out_d; r += 3) {
                const int8_t* w0 = ql.w + (r + 0) * in_d;
                const int8_t* w1 = ql.w + (r + 1) * in_d;
                const int8_t* w2 = ql.w + (r + 2) * in_d;
                __m256i a00 = _mm256_setzero_si256(), a01 = _mm256_setzero_si256(), a02 = _mm256_setzero_si256();
                __m256i a10 = _mm256_setzero_si256(), a11 = _mm256_setzero_si256(), a12 = _mm256_setzero_si256();
                __m256i a20 = _mm256_setzero_si256(), a21 = _mm256_setzero_si256(), a22 = _mm256_setzero_si256();
                __m256i a30 = _mm256_setzero_si256(), a31 = _mm256_setzero_si256(), a32 = _mm256_setzero_si256();

                for (size_t k = 0; k < in_d; k += 32) {
                    __m256i vx0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x0 + k));
                    __m256i vx1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x1 + k));
                    __m256i vx2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x2 + k));
                    __m256i vx3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x3 + k));

                    __m256i vw0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w0 + k));
                    a00 = _mm256_add_epi32(a00, _mm256_madd_epi16(_mm256_maddubs_epi16(vx0, vw0), ones16));
                    a10 = _mm256_add_epi32(a10, _mm256_madd_epi16(_mm256_maddubs_epi16(vx1, vw0), ones16));
                    a20 = _mm256_add_epi32(a20, _mm256_madd_epi16(_mm256_maddubs_epi16(vx2, vw0), ones16));
                    a30 = _mm256_add_epi32(a30, _mm256_madd_epi16(_mm256_maddubs_epi16(vx3, vw0), ones16));

                    __m256i vw1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w1 + k));
                    a01 = _mm256_add_epi32(a01, _mm256_madd_epi16(_mm256_maddubs_epi16(vx0, vw1), ones16));
                    a11 = _mm256_add_epi32(a11, _mm256_madd_epi16(_mm256_maddubs_epi16(vx1, vw1), ones16));
                    a21 = _mm256_add_epi32(a21, _mm256_madd_epi16(_mm256_maddubs_epi16(vx2, vw1), ones16));
                    a31 = _mm256_add_epi32(a31, _mm256_madd_epi16(_mm256_maddubs_epi16(vx3, vw1), ones16));

                    __m256i vw2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w2 + k));
                    a02 = _mm256_add_epi32(a02, _mm256_madd_epi16(_mm256_maddubs_epi16(vx0, vw2), ones16));
                    a12 = _mm256_add_epi32(a12, _mm256_madd_epi16(_mm256_maddubs_epi16(vx1, vw2), ones16));
                    a22 = _mm256_add_epi32(a22, _mm256_madd_epi16(_mm256_maddubs_epi16(vx2, vw2), ones16));
                    a32 = _mm256_add_epi32(a32, _mm256_madd_epi16(_mm256_maddubs_epi16(vx3, vw2), ones16));
                }

                const float ws0 = ql.scale[r + 0], ws1 = ql.scale[r + 1], ws2 = ql.scale[r + 2];
                const int32_t zp0 = ql.zp_sum[r + 0], zp1 = ql.zp_sum[r + 1], zp2 = ql.zp_sum[r + 2];
                const float b0 = ql.bias[r + 0], b1 = ql.bias[r + 1], b2 = ql.bias[r + 2];

                float v00 = static_cast<float>(hsum_epi32(a00) - zp0) * (s0 * ws0) + b0;
                float v01 = static_cast<float>(hsum_epi32(a01) - zp1) * (s0 * ws1) + b1;
                float v02 = static_cast<float>(hsum_epi32(a02) - zp2) * (s0 * ws2) + b2;
                float v10 = static_cast<float>(hsum_epi32(a10) - zp0) * (s1 * ws0) + b0;
                float v11 = static_cast<float>(hsum_epi32(a11) - zp1) * (s1 * ws1) + b1;
                float v12 = static_cast<float>(hsum_epi32(a12) - zp2) * (s1 * ws2) + b2;
                float v20 = static_cast<float>(hsum_epi32(a20) - zp0) * (s2 * ws0) + b0;
                float v21 = static_cast<float>(hsum_epi32(a21) - zp1) * (s2 * ws1) + b1;
                float v22 = static_cast<float>(hsum_epi32(a22) - zp2) * (s2 * ws2) + b2;
                float v30 = static_cast<float>(hsum_epi32(a30) - zp0) * (s3 * ws0) + b0;
                float v31 = static_cast<float>(hsum_epi32(a31) - zp1) * (s3 * ws1) + b1;
                float v32 = static_cast<float>(hsum_epi32(a32) - zp2) * (s3 * ws2) + b2;

                if (accumulate_res) {
                    y0[r + 0] += v00; y0[r + 1] += v01; y0[r + 2] += v02;
                    y1[r + 0] += v10; y1[r + 1] += v11; y1[r + 2] += v12;
                    y2[r + 0] += v20; y2[r + 1] += v21; y2[r + 2] += v22;
                    y3[r + 0] += v30; y3[r + 1] += v31; y3[r + 2] += v32;
                } else {
                    y0[r + 0] = v00; y0[r + 1] = v01; y0[r + 2] = v02;
                    y1[r + 0] = v10; y1[r + 1] = v11; y1[r + 2] = v12;
                    y2[r + 0] = v20; y2[r + 1] = v21; y2[r + 2] = v22;
                    y3[r + 0] = v30; y3[r + 1] = v31; y3[r + 2] = v32;
                }
            }
            for (; r < out_d; ++r) {
                const int8_t* wr = ql.w + r * in_d;
                __m256i a0 = _mm256_setzero_si256(), a1 = _mm256_setzero_si256();
                __m256i a2 = _mm256_setzero_si256(), a3 = _mm256_setzero_si256();
                for (size_t k = 0; k < in_d; k += 32) {
                    __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(wr + k));
                    a0 = _mm256_add_epi32(a0, _mm256_madd_epi16(_mm256_maddubs_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(x0 + k)), vw), ones16));
                    a1 = _mm256_add_epi32(a1, _mm256_madd_epi16(_mm256_maddubs_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(x1 + k)), vw), ones16));
                    a2 = _mm256_add_epi32(a2, _mm256_madd_epi16(_mm256_maddubs_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(x2 + k)), vw), ones16));
                    a3 = _mm256_add_epi32(a3, _mm256_madd_epi16(_mm256_maddubs_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(x3 + k)), vw), ones16));
                }
                const float ws_r = ql.scale[r], b_r = ql.bias[r];
                const int32_t zp_r = ql.zp_sum[r];
                float v0 = static_cast<float>(hsum_epi32(a0) - zp_r) * (s0 * ws_r) + b_r;
                float v1 = static_cast<float>(hsum_epi32(a1) - zp_r) * (s1 * ws_r) + b_r;
                float v2 = static_cast<float>(hsum_epi32(a2) - zp_r) * (s2 * ws_r) + b_r;
                float v3 = static_cast<float>(hsum_epi32(a3) - zp_r) * (s3 * ws_r) + b_r;
                if (accumulate_res) { y0[r] += v0; y1[r] += v1; y2[r] += v2; y3[r] += v3; }
                else { y0[r] = v0; y1[r] = v1; y2[r] = v2; y3[r] = v3; }
            }
        }
        for (; t < L; ++t) {
            const uint8_t* xt = ws.act_u8.data() + t * in_d;
            const float st = ws.attn_sc[t];
            float* yt = Y + t * out_d;
            size_t r = 0;
            for (; r + 4 <= out_d; r += 4) {
                const int8_t* w0 = ql.w + (r + 0) * in_d;
                const int8_t* w1 = ql.w + (r + 1) * in_d;
                const int8_t* w2 = ql.w + (r + 2) * in_d;
                const int8_t* w3 = ql.w + (r + 3) * in_d;
                __m256i a0 = _mm256_setzero_si256(), a1 = _mm256_setzero_si256();
                __m256i a2 = _mm256_setzero_si256(), a3 = _mm256_setzero_si256();
                for (size_t k = 0; k < in_d; k += 32) {
                    __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(xt + k));
                    a0 = _mm256_add_epi32(a0, _mm256_madd_epi16(_mm256_maddubs_epi16(vx, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w0 + k))), ones16));
                    a1 = _mm256_add_epi32(a1, _mm256_madd_epi16(_mm256_maddubs_epi16(vx, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w1 + k))), ones16));
                    a2 = _mm256_add_epi32(a2, _mm256_madd_epi16(_mm256_maddubs_epi16(vx, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w2 + k))), ones16));
                    a3 = _mm256_add_epi32(a3, _mm256_madd_epi16(_mm256_maddubs_epi16(vx, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w3 + k))), ones16));
                }
                float val0 = static_cast<float>(hsum_epi32(a0) - ql.zp_sum[r + 0]) * (st * ql.scale[r + 0]) + ql.bias[r + 0];
                float val1 = static_cast<float>(hsum_epi32(a1) - ql.zp_sum[r + 1]) * (st * ql.scale[r + 1]) + ql.bias[r + 1];
                float val2 = static_cast<float>(hsum_epi32(a2) - ql.zp_sum[r + 2]) * (st * ql.scale[r + 2]) + ql.bias[r + 2];
                float val3 = static_cast<float>(hsum_epi32(a3) - ql.zp_sum[r + 3]) * (st * ql.scale[r + 3]) + ql.bias[r + 3];
                if (accumulate_res) {
                    yt[r + 0] += val0; yt[r + 1] += val1; yt[r + 2] += val2; yt[r + 3] += val3;
                } else {
                    yt[r + 0] = val0; yt[r + 1] = val1; yt[r + 2] = val2; yt[r + 3] = val3;
                }
            }
            for (; r < out_d; ++r) {
                const int8_t* wr = ql.w + r * in_d;
                __m256i a0 = _mm256_setzero_si256();
                for (size_t k = 0; k < in_d; k += 32) {
                    a0 = _mm256_add_epi32(a0, _mm256_madd_epi16(_mm256_maddubs_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(xt + k)), _mm256_loadu_si256(reinterpret_cast<const __m256i*>(wr + k))), ones16));
                }
                float val = static_cast<float>(hsum_epi32(a0) - ql.zp_sum[r]) * (st * ql.scale[r]) + ql.bias[r];
                if (accumulate_res) yt[r] += val; else yt[r] = val;
            }
        }
#elif defined(__ARM_NEON) && defined(__aarch64__)
        const int16x8_t bias128 = vdupq_n_s16(128);
        for (size_t t = 0; t < L; ++t) {
            const uint8_t* xt = ws.act_u8.data() + t * in_d;
            const float st = ws.attn_sc[t];
            float* yt = Y + t * out_d;
            for (size_t r = 0; r < out_d; ++r) {
                const int8_t* wr = ql.w + r * in_d;
                int32x4_t acc_v = vdupq_n_s32(0);
                for (size_t k = 0; k < in_d; k += 16) {
                    uint8x16_t vu = vld1q_u8(xt + k);
                    int8x16_t vw = vld1q_s8(wr + k);
                    int16x8_t xu_lo = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(vu))), bias128);
                    int16x8_t xu_hi = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(vu))), bias128);
                    int16x8_t w_lo = vmovl_s8(vget_low_s8(vw));
                    int16x8_t w_hi = vmovl_s8(vget_high_s8(vw));
                    acc_v = vmlal_s16(acc_v, vget_low_s16(xu_lo), vget_low_s16(w_lo));
                    acc_v = vmlal_s16(acc_v, vget_high_s16(xu_lo), vget_high_s16(w_lo));
                    acc_v = vmlal_s16(acc_v, vget_low_s16(xu_hi), vget_low_s16(w_hi));
                    acc_v = vmlal_s16(acc_v, vget_high_s16(xu_hi), vget_high_s16(w_hi));
                }
                float v = static_cast<float>(vaddvq_s32(acc_v)) * (st * ql.scale[r]) + ql.bias[r];
                if (accumulate_res) yt[r] += v;
                else yt[r] = v;
            }
        }
#else
        for (size_t t = 0; t < L; ++t) {
            const uint8_t* xt = ws.act_u8.data() + t * in_d;
            const float st = ws.attn_sc[t];
            float* yt = Y + t * out_d;
            for (size_t r = 0; r < out_d; ++r) {
                const int8_t* wr = ql.w + r * in_d;
                int32_t acc = 0;
                for (size_t k = 0; k < in_d; ++k) {
                    acc += static_cast<int32_t>(xt[k]) * static_cast<int32_t>(wr[k]);
                }
                float v = static_cast<float>(acc - ql.zp_sum[r]) * (st * ql.scale[r]) + ql.bias[r];
                if (accumulate_res) yt[r] += v;
                else yt[r] = v;
            }
        }
#endif
    }

    static inline void qlinear_batch(const QuantLinear& ql, const float* X, float* Y, size_t L,
                                     NeuralIrabWorkspace& ws, bool accumulate_res = false) {
        quantize_batch_u8(X, L, ql.in_dim, ws.act_u8.data(), ws.attn_sc.data());
        qlinear_batch_preq(ql, Y, L, ws, accumulate_res);
    }

    static inline void fp32_linear_row(const float* W, const float* b, const float* x, float* y, size_t in_d, size_t out_d) {
#if defined(__AVX2__)
        for (size_t r = 0; r < out_d; ++r) {
            const float* wr = W + r * in_d;
            __m256 vacc = _mm256_setzero_ps();
            size_t k = 0;
            for (; k + 8 <= in_d; k += 8) {
                vacc = _mm256_fmadd_ps(_mm256_loadu_ps(wr + k), _mm256_loadu_ps(x + k), vacc);
            }
            __m128 hi = _mm256_extractf128_ps(vacc, 1);
            __m128 lo = _mm256_castps256_ps128(vacc);
            __m128 s = _mm_add_ps(lo, hi);
            s = _mm_hadd_ps(s, s);
            s = _mm_hadd_ps(s, s);
            float acc = b[r] + _mm_cvtss_f32(s);
            for (; k < in_d; ++k) acc += wr[k] * x[k];
            y[r] = acc;
        }
#elif defined(__ARM_NEON) && defined(__aarch64__)
        for (size_t r = 0; r < out_d; ++r) {
            const float* wr = W + r * in_d;
            float32x4_t vacc = vdupq_n_f32(0.0f);
            size_t k = 0;
            for (; k + 4 <= in_d; k += 4) {
                vacc = vfmaq_f32(vacc, vld1q_f32(wr + k), vld1q_f32(x + k));
            }
            float acc = b[r] + vaddvq_f32(vacc);
            for (; k < in_d; ++k) acc += wr[k] * x[k];
            y[r] = acc;
        }
#else
        for (size_t r = 0; r < out_d; ++r) {
            const float* wr = W + r * in_d;
            float acc = b[r];
            for (size_t k = 0; k < in_d; ++k) acc += wr[k] * x[k];
            y[r] = acc;
        }
#endif
    }

    static inline void dw_conv_dilated(const float* X, const float* W_kd, const float* b, float* Y,
                                       size_t L, size_t D, int dil) {
#if defined(__AVX2__)
        const __m256 z = _mm256_setzero_ps();
        const __m256 six = _mm256_set1_ps(6.0f);
        for (size_t t = 0; t < L; ++t) {
            float* dst = Y + t * D;
            const int i0 = static_cast<int>(t) - 2 * dil;
            const int i1 = static_cast<int>(t) - dil;
            const int i2 = static_cast<int>(t);
            const int i3 = static_cast<int>(t) + dil;
            const int i4 = static_cast<int>(t) + 2 * dil;
            const float* p0 = (i0 >= 0) ? (X + static_cast<size_t>(i0) * D) : nullptr;
            const float* p1 = (i1 >= 0) ? (X + static_cast<size_t>(i1) * D) : nullptr;
            const float* p2 = X + static_cast<size_t>(i2) * D;
            const float* p3 = (i3 < static_cast<int>(L)) ? (X + static_cast<size_t>(i3) * D) : nullptr;
            const float* p4 = (i4 < static_cast<int>(L)) ? (X + static_cast<size_t>(i4) * D) : nullptr;

            for (size_t c = 0; c < D; c += 8) {
                __m256 sum = _mm256_loadu_ps(b + c);
                if (p0) sum = _mm256_fmadd_ps(_mm256_loadu_ps(p0 + c), _mm256_loadu_ps(W_kd + 0 * D + c), sum);
                if (p1) sum = _mm256_fmadd_ps(_mm256_loadu_ps(p1 + c), _mm256_loadu_ps(W_kd + 1 * D + c), sum);
                sum = _mm256_fmadd_ps(_mm256_loadu_ps(p2 + c), _mm256_loadu_ps(W_kd + 2 * D + c), sum);
                if (p3) sum = _mm256_fmadd_ps(_mm256_loadu_ps(p3 + c), _mm256_loadu_ps(W_kd + 3 * D + c), sum);
                if (p4) sum = _mm256_fmadd_ps(_mm256_loadu_ps(p4 + c), _mm256_loadu_ps(W_kd + 4 * D + c), sum);
                sum = _mm256_min_ps(six, _mm256_max_ps(z, sum));
                _mm256_storeu_ps(dst + c, sum);
            }
        }
#elif defined(__ARM_NEON) && defined(__aarch64__)
        const float32x4_t z = vdupq_n_f32(0.0f);
        const float32x4_t six = vdupq_n_f32(6.0f);
        for (size_t t = 0; t < L; ++t) {
            float* dst = Y + t * D;
            const int i0 = static_cast<int>(t) - 2 * dil;
            const int i1 = static_cast<int>(t) - dil;
            const int i2 = static_cast<int>(t);
            const int i3 = static_cast<int>(t) + dil;
            const int i4 = static_cast<int>(t) + 2 * dil;
            const float* p0 = (i0 >= 0) ? (X + static_cast<size_t>(i0) * D) : nullptr;
            const float* p1 = (i1 >= 0) ? (X + static_cast<size_t>(i1) * D) : nullptr;
            const float* p2 = X + static_cast<size_t>(i2) * D;
            const float* p3 = (i3 < static_cast<int>(L)) ? (X + static_cast<size_t>(i3) * D) : nullptr;
            const float* p4 = (i4 < static_cast<int>(L)) ? (X + static_cast<size_t>(i4) * D) : nullptr;

            for (size_t c = 0; c < D; c += 4) {
                float32x4_t sum = vld1q_f32(b + c);
                if (p0) sum = vfmaq_f32(sum, vld1q_f32(p0 + c), vld1q_f32(W_kd + 0 * D + c));
                if (p1) sum = vfmaq_f32(sum, vld1q_f32(p1 + c), vld1q_f32(W_kd + 1 * D + c));
                sum = vfmaq_f32(sum, vld1q_f32(p2 + c), vld1q_f32(W_kd + 2 * D + c));
                if (p3) sum = vfmaq_f32(sum, vld1q_f32(p3 + c), vld1q_f32(W_kd + 3 * D + c));
                if (p4) sum = vfmaq_f32(sum, vld1q_f32(p4 + c), vld1q_f32(W_kd + 4 * D + c));
                sum = vminq_f32(six, vmaxq_f32(z, sum));
                vst1q_f32(dst + c, sum);
            }
        }
#else
        for (size_t t = 0; t < L; ++t) {
            float* dst = Y + t * D;
            for (size_t c = 0; c < D; ++c) dst[c] = b[c];
            for (int k = 0; k < 5; ++k) {
                int src_t = static_cast<int>(t) + (k - 2) * dil;
                if (src_t >= 0 && src_t < static_cast<int>(L)) {
                    const float* xs = X + static_cast<size_t>(src_t) * D;
                    const float* wk = W_kd + static_cast<size_t>(k) * D;
                    for (size_t c = 0; c < D; ++c) dst[c] += xs[c] * wk[c];
                }
            }
            for (size_t c = 0; c < D; ++c) dst[c] = relu6_scalar(dst[c]);
        }
#endif
    }

    // Bidirectional Gated Linear Recurrence (48 fwd + 48 bwd = 96 channels) + ReLU6
    // Gate formula: a = 0.96 * (0.5 + 0.5 * g / (1.0 + |g|)), h[t] = a * h[t-1] + (1.0 - a) * u
    static inline void bi_glru_sweep(const float* in_2d, float* out_d, size_t L, size_t D) {
        const size_t R = D_REC_V2; // 48
        const size_t D2 = 2 * D;   // 192
#if defined(__AVX2__)
        const __m256 c_096 = _mm256_set1_ps(0.96f);
        const __m256 c_05  = _mm256_set1_ps(0.5f);
        const __m256 c_10  = _mm256_set1_ps(1.0f);
        const __m256 z     = _mm256_setzero_ps();
        const __m256 six   = _mm256_set1_ps(6.0f);
        const __m256 abs_m = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));

        // Forward sweep (48 = 6 x 8 floats kept in YMM registers)
        __m256 hf0 = z, hf1 = z, hf2 = z, hf3 = z, hf4 = z, hf5 = z;
        for (size_t t = 0; t < L; ++t) {
            const float* uf = in_2d + t * D2;
            const float* gf = uf + D;
            float* dst = out_d + t * D;

            auto step_fwd = [&](size_t off, __m256& h) {
                __m256 g = _mm256_loadu_ps(gf + off);
                __m256 ag = _mm256_and_ps(g, abs_m);
                __m256 sig = _mm256_add_ps(c_05, _mm256_div_ps(_mm256_mul_ps(c_05, g), _mm256_add_ps(c_10, ag)));
                __m256 a = _mm256_mul_ps(c_096, sig);
                __m256 u = _mm256_loadu_ps(uf + off);
                h = _mm256_fmadd_ps(a, _mm256_sub_ps(h, u), u);
                __m256 r = _mm256_min_ps(six, _mm256_max_ps(z, h));
                _mm256_storeu_ps(dst + off, r);
            };
            step_fwd(0,  hf0);
            step_fwd(8,  hf1);
            step_fwd(16, hf2);
            step_fwd(24, hf3);
            step_fwd(32, hf4);
            step_fwd(40, hf5);
        }

        // Backward sweep (48 channels)
        __m256 hb0 = z, hb1 = z, hb2 = z, hb3 = z, hb4 = z, hb5 = z;
        for (size_t ti = L; ti > 0; --ti) {
            const size_t t = ti - 1;
            const float* ub = in_2d + t * D2 + R;
            const float* gb = in_2d + t * D2 + D + R;
            float* dst = out_d + t * D + R;

            auto step_bwd = [&](size_t off, __m256& h) {
                __m256 g = _mm256_loadu_ps(gb + off);
                __m256 ag = _mm256_and_ps(g, abs_m);
                __m256 sig = _mm256_add_ps(c_05, _mm256_div_ps(_mm256_mul_ps(c_05, g), _mm256_add_ps(c_10, ag)));
                __m256 a = _mm256_mul_ps(c_096, sig);
                __m256 u = _mm256_loadu_ps(ub + off);
                h = _mm256_fmadd_ps(a, _mm256_sub_ps(h, u), u);
                __m256 r = _mm256_min_ps(six, _mm256_max_ps(z, h));
                _mm256_storeu_ps(dst + off, r);
            };
            step_bwd(0,  hb0);
            step_bwd(8,  hb1);
            step_bwd(16, hb2);
            step_bwd(24, hb3);
            step_bwd(32, hb4);
            step_bwd(40, hb5);
        }
#elif defined(__ARM_NEON) && defined(__aarch64__)
        const float32x4_t c_096 = vdupq_n_f32(0.96f);
        const float32x4_t c_05  = vdupq_n_f32(0.5f);
        const float32x4_t c_10  = vdupq_n_f32(1.0f);
        const float32x4_t z     = vdupq_n_f32(0.0f);
        const float32x4_t six   = vdupq_n_f32(6.0f);
        auto sweep_dir = [&](size_t offset, bool backward) {
            float32x4_t h[12];
            for (int b = 0; b < 12; ++b) h[b] = z;
            for (size_t ti = 0; ti < L; ++ti) {
                const size_t t = backward ? (L - 1 - ti) : ti;
                const float* ut = in_2d + t * D2 + offset;
                const float* gt = in_2d + t * D2 + D + offset;
                float* dst = out_d + t * D + offset;
                for (int b = 0; b < 12; ++b) {
                    const size_t ch = static_cast<size_t>(b) * 4u;
                    float32x4_t g = vld1q_f32(gt + ch);
                    float32x4_t u = vld1q_f32(ut + ch);
                    float32x4_t den = vaddq_f32(c_10, vabsq_f32(g));
                    float32x4_t sig = vaddq_f32(c_05, vdivq_f32(vmulq_f32(c_05, g), den));
                    float32x4_t a = vmulq_f32(c_096, sig);
                    h[b] = vfmaq_f32(u, a, vsubq_f32(h[b], u));
                    vst1q_f32(dst + ch, vminq_f32(six, vmaxq_f32(z, h[b])));
                }
            }
        };
        sweep_dir(0, false);
        sweep_dir(R, true);
#else
        float hf[48] = {};
        for (size_t t = 0; t < L; ++t) {
            const float* uf = in_2d + t * D2;
            const float* gf = uf + D;
            float* dst = out_d + t * D;
            for (size_t c = 0; c < R; ++c) {
                float g = gf[c];
                float a = 0.96f * (0.5f + 0.5f * g / (1.0f + std::fabs(g)));
                hf[c] = a * hf[c] + (1.0f - a) * uf[c];
                dst[c] = relu6_scalar(hf[c]);
            }
        }
        float hb[48] = {};
        for (size_t ti = L; ti > 0; --ti) {
            const size_t t = ti - 1;
            const float* ub = in_2d + t * D2 + R;
            const float* gb = in_2d + t * D2 + D + R;
            float* dst = out_d + t * D + R;
            for (size_t c = 0; c < R; ++c) {
                float g = gb[c];
                float a = 0.96f * (0.5f + 0.5f * g / (1.0f + std::fabs(g)));
                hb[c] = a * hb[c] + (1.0f - a) * ub[c];
                dst[c] = relu6_scalar(hb[c]);
            }
        }
#endif
    }

    void forward_segment(const NeuralIrabTokInput* toks, size_t L, size_t keep_s, size_t keep_e,
                         size_t global_off, NeuralIrabWorkspace& ws) const {
        const size_t D = d_model_;
        const size_t DF = d_ff_;

        ws.h_cur.resize(L * D);
        ws.h_tmp.resize(L * D);
        ws.h_norm.resize(L * D);
        ws.qkv.resize(L * 2 * D);
        ws.attn_out.resize(L * D);
        ws.ffn_mid.resize(L * DF);
        ws.attn_sc.resize(L);
        ws.act_u8.resize(L * std::max(D, DF));

        float feat_in[D_FEAT_IN];
        for (size_t t = 0; t < L; ++t) {
            const NeuralIrabTokInput& ti = toks[t];
            float* out = ws.h_cur.data() + t * D;

            uint16_t row = (ti.word_id < vocab_size_) ? wid_to_row_[ti.word_id] : 0;
            if (row >= word_emb_rows_) row = 0;
            const int8_t* we = word_emb_ + static_cast<size_t>(row) * D_WORD;
            const int8_t* h0 = hash_emb_ + static_cast<size_t>(ti.hashes[0] % hash_buckets_) * D_HASH;
            const int8_t* h1 = hash_emb_ + static_cast<size_t>(ti.hashes[1] % hash_buckets_) * D_HASH;
            const int8_t* h2 = hash_emb_ + static_cast<size_t>(ti.hashes[2] % hash_buckets_) * D_HASH;
            const int8_t* h3 = hash_emb_ + static_cast<size_t>(ti.hashes[3] % hash_buckets_) * D_HASH;
            float* ho = out + D_WORD;
#if defined(__AVX2__)
            const __m256 vws = _mm256_set1_ps(word_emb_scale_);
            const __m256 vhs = _mm256_set1_ps(hash_emb_scale_);
            for (size_t k = 0; k < 32; k += 8) {
                __m256i vw = _mm256_cvtepi8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(we + k)));
                _mm256_storeu_ps(out + k, _mm256_mul_ps(_mm256_cvtepi32_ps(vw), vws));
                __m256i s0 = _mm256_cvtepi8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(h0 + k)));
                __m256i s1 = _mm256_cvtepi8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(h1 + k)));
                __m256i s2 = _mm256_cvtepi8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(h2 + k)));
                __m256i s3 = _mm256_cvtepi8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(h3 + k)));
                __m256i hs = _mm256_add_epi32(_mm256_add_epi32(s0, s1), _mm256_add_epi32(s2, s3));
                _mm256_storeu_ps(ho + k, _mm256_mul_ps(_mm256_cvtepi32_ps(hs), vhs));
            }
#else
            for (size_t k = 0; k < D_WORD; ++k) out[k] = static_cast<float>(we[k]) * word_emb_scale_;
            for (size_t k = 0; k < D_HASH; ++k) {
                int32_t s = static_cast<int32_t>(h0[k]) + static_cast<int32_t>(h1[k]) +
                            static_cast<int32_t>(h2[k]) + static_cast<int32_t>(h3[k]);
                ho[k] = static_cast<float>(s) * hash_emb_scale_;
            }
#endif

            std::memcpy(feat_in, ti.dense, 48 * sizeof(float));
            uint8_t pid = ti.part_id < 32 ? ti.part_id : 0;
            uint8_t psid = ti.pos_id < 16 ? ti.pos_id : 0;
            std::memcpy(feat_in + 48, part_emb_ + pid * D_PART, D_PART * sizeof(float));
            std::memcpy(feat_in + 64, pos_emb_ + psid * D_POS, D_POS * sizeof(float));
            float* fo = out + D_WORD + D_HASH;
            fp32_linear_row(feat_w_, feat_b_, feat_in, fo, D_FEAT_IN, D_FEAT);
            relu6_inplace(fo, D_FEAT);
        }

        for (size_t l = 0; l < n_layers_; ++l) {
            const Layer& ly = layers_[l];
            const int dil = 1 << static_cast<int>(l);

            // 1. RMSNorm -> Dilated Depthwise Conv1D + ReLU6 -> Pointwise Conv + Residual
            rmsnorm_batch(ws.h_cur.data(), ly.norm1_g, ws.h_norm.data(), L, D);
            dw_conv_dilated(ws.h_norm.data(), ly.dw_conv_w, ly.dw_conv_b, ws.h_tmp.data(), L, D, dil);
            qlinear_batch(ly.conv_pw, ws.h_tmp.data(), ws.h_cur.data(), L, ws, true);

            // 2. RMSNorm + Quantize -> glru_in (D -> 2*D) -> Bi-GLRU + ReLU6 -> glru_out (D -> D) + Residual
            rmsnorm_quantize_u8(ws.h_cur.data(), ly.norm2_g, ws.act_u8.data(), ws.attn_sc.data(), L, D);
            qlinear_batch_preq(ly.glru_in, ws.qkv.data(), L, ws, false);
            bi_glru_sweep(ws.qkv.data(), ws.attn_out.data(), L, D);
            qlinear_batch(ly.glru_out, ws.attn_out.data(), ws.h_cur.data(), L, ws, true);

            // 3. RMSNorm + Quantize -> ffn_up (D -> D_FF) + ReLU6 -> ffn_down (D_FF -> D) + Residual
            rmsnorm_quantize_u8(ws.h_cur.data(), ly.norm3_g, ws.act_u8.data(), ws.attn_sc.data(), L, D);
            qlinear_batch_preq(ly.ffn_up, ws.ffn_mid.data(), L, ws, false);
            relu6_inplace(ws.ffn_mid.data(), L * DF);
            qlinear_batch(ly.ffn_down, ws.ffn_mid.data(), ws.h_cur.data(), L, ws, true);
        }

        float logits[16];
        rmsnorm_batch(ws.h_cur.data() + keep_s * D, norm_final_g_, ws.h_norm.data() + keep_s * D, keep_e - keep_s, D);
        for (size_t t = keep_s; t < keep_e; ++t) {
            const float* hn = ws.h_norm.data() + t * D;
            const size_t gi = global_off + t;

            fp32_linear_row(w_end_, b_end_, hn, logits, D, NUM_DIACRITICS);
            log_softmax_row(logits, ws.end_logp.data() + gi * NUM_DIACRITICS, NUM_DIACRITICS);

            fp32_linear_row(w_fam_, b_fam_, hn, logits, D, 5);
            log_softmax_row(logits, ws.fam_logp.data() + gi * 5, 5);

            fp32_linear_row(w_tan_, b_tan_, hn, logits, D, 2);
            log_softmax_row(logits, ws.tan_logp.data() + gi * 2, 2);

            float* p_out = ws.pron_logp.data() + gi * 4;
            if (toks[t].need_pron != 0) {
                fp32_linear_row(w_pron_, b_pron_, hn, logits, D, 4);
                log_softmax_row(logits, p_out, 4);
            } else {
                p_out[0] = p_out[1] = p_out[2] = p_out[3] = 0.0f;
            }
        }
    }

    bool valid_ = false;
    size_t d_model_ = 0;
    size_t n_layers_ = 0;
    size_t d_rec_ = 0;
    size_t d_ff_ = 0;
    size_t conv_k_ = 0;
    uint32_t vocab_size_ = 0;
    uint16_t word_emb_rows_ = 0;
    uint16_t hash_buckets_ = 0;
    uint16_t n_dense_ = 0;

    const uint16_t* wid_to_row_ = nullptr;
    float word_emb_scale_ = 0.0f;
    const int8_t* word_emb_ = nullptr;
    float hash_emb_scale_ = 0.0f;
    const int8_t* hash_emb_ = nullptr;
    const float* part_emb_ = nullptr;
    const float* pos_emb_ = nullptr;
    const float* feat_w_ = nullptr;
    const float* feat_b_ = nullptr;

    std::vector<Layer> layers_;
    const float* norm_final_g_ = nullptr;
    const float* w_end_ = nullptr;
    const float* b_end_ = nullptr;
    const float* w_fam_ = nullptr;
    const float* b_fam_ = nullptr;
    const float* w_tan_ = nullptr;
    const float* b_tan_ = nullptr;
    const float* w_pron_ = nullptr;
    const float* b_pron_ = nullptr;
    const float* crf_trans_ = nullptr;
};

} // namespace tashkeel
