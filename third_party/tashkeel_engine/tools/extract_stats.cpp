#include "tashkeel/diacritizer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace tashkeel;

namespace {

struct ParsedWord {
    uint8_t syms[MAX_WORD_LEN];
    uint8_t diacs[MAX_WORD_LEN];
    uint8_t n = 0;
    uint8_t boundary_before = 1;
    uint32_t wid = UINT32_MAX;
    uint8_t cpos = 0;
    uint8_t end_id = 0;
    uint8_t fam = 0;
    uint8_t tan = 0;
    uint8_t top_verb = 0;
    uint8_t is_fem_at = 0;
    uint8_t is_past_fem = 0;
    uint8_t is_verb = 0;
    uint8_t is_pass = 0;
    uint8_t is_subj12 = 0;
    uint8_t is_kana = 0;
    uint8_t is_inna = 0;
    uint8_t has_suf = 0;
    uint16_t core_h = 0;
    SurfaceInfo si;
};

static inline bool is_boundary_cp_ext(uint32_t cp) {
    switch (cp) {
        case '.': case '!': case '?': case ';': case ':': case ',': case '\n': case '(': case ')':
        case '[': case ']': case '{': case '}': case '"':
        case 0x061F: case 0x061B: case 0x060C: case 0x00AB: case 0x00BB: case 0x06D4:
            return true;
        default:
            return false;
    }
}

static inline bool is_man_or_an_prep(const uint8_t* s, uint8_t n) {
    size_t st = (n == 3 && (s[0] == S_WAW || s[0] == S_FEH)) ? 1 : 0;
    if (n - st != 2) return false;
    return (s[st] == S_MEEM && s[st + 1] == S_NOON) || (s[st] == S_AIN && s[st + 1] == S_NOON);
}

static inline bool is_plural_meem_ext(const uint8_t* s, uint8_t n) {
    return n >= 3 && s[n - 1] == S_MEEM && (s[n - 2] == S_HEH || s[n - 2] == S_KAF || s[n - 2] == S_TEH);
}

static inline bool is_weak_final_sym(uint8_t s) {
    return s == S_ALEF || s == S_ALEF_MAKSURA || s == S_YEH || s == S_WAW;
}

struct LexCorePrior {
    uint16_t h[4] = {0, 0, 0, 0};
    float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int cnt = 0;
};

static inline bool is_kana_sym(const uint8_t* s, uint8_t n) {
    if (n < 3) return false;
    size_t st = 0;
    if (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH || s[0] == S_LAM)) st = 1;
    if (n - st >= 5 && s[st] == S_SEEN) ++st;
    size_t sn = n - st;
    const uint8_t* w = s + st;
    if (sn == 3 && w[0] == S_KAF && w[1] == S_ALEF && w[2] == S_NOON) return true;
    if (sn == 4 && w[0] == S_KAF && w[1] == S_ALEF && w[2] == S_NOON && (w[3] == S_TEH || w[3] == S_WAW)) return true;
    if (sn == 5 && w[0] == S_KAF && w[1] == S_ALEF && w[2] == S_NOON && w[3] == S_WAW && w[4] == S_ALEF) return true;
    if (sn == 4 && (w[0] == S_YEH || w[0] == S_TEH || w[0] == S_NOON || w[0] == S_ALEF_HAMZA_A) &&
        w[1] == S_KAF && w[2] == S_WAW && w[3] == S_NOON) return true;
    return false;
}

static inline bool is_inna_pron_sym(const uint8_t* s, uint8_t n) {
    size_t st = 0;
    if (n >= 4 && (s[0] == S_WAW || s[0] == S_FEH)) st = 1;
    if (n - st >= 4 && (s[st] == S_LAM || s[st] == S_BEH || s[st] == S_KAF)) ++st;
    size_t len = n - st;
    const uint8_t* w = s + st;
    if (len == 3 && (w[0] == S_ALEF_HAMZA_A || w[0] == S_ALEF_HAMZA_B) && w[1] == S_NOON && (w[2] == S_HEH || w[2] == S_KAF)) return true;
    if (len == 4 && (w[0] == S_ALEF_HAMZA_A || w[0] == S_ALEF_HAMZA_B) && w[1] == S_NOON &&
        ((w[2] == S_HEH && (w[3] == S_ALEF || w[3] == S_MEEM)) ||
         (w[2] == S_NOON && w[3] == S_ALEF) || (w[2] == S_KAF && w[3] == S_MEEM))) return true;
    return false;
}

static void parse_gold_line(
    const Diacritizer& eng,
    const std::vector<uint8_t>& wid_cpos,
    const std::vector<uint8_t>& wid_top_verb,
    const std::string& line,
    std::vector<ParsedWord>& out) {
    out.clear();
    const char* s = line.data();
    const size_t len = line.size();
    size_t pos = 0;
    uint8_t pending_b = 1;
    while (pos < len) {
        uint32_t cp = utf8_next(s, len, pos);
        if (is_arabic_letter(cp)) {
            ParsedWord pw{};
            pw.boundary_before = pending_b;
            pending_b = 0;
            size_t cnt = 0;
            auto read_d = [&](size_t idx) {
                bool sh = false;
                uint8_t base = DIAC_NONE;
                while (pos < len) {
                    size_t ps = pos;
                    uint32_t dcp = utf8_next(s, len, pos);
                    if (is_tatweel(dcp)) continue;
                    if (!is_arabic_diacritic(dcp)) { pos = ps; break; }
                    if (dcp == 0x0651u) sh = true;
                    else if (dcp == 0x064Eu) base = DIAC_FATHA;
                    else if (dcp == 0x064Fu) base = DIAC_DAMMA;
                    else if (dcp == 0x0650u) base = DIAC_KASRA;
                    else if (dcp == 0x0652u) base = DIAC_SUKUN;
                    else if (dcp == 0x064Bu) base = DIAC_FATHATAN;
                    else if (dcp == 0x064Cu) base = DIAC_DAMMATAN;
                    else if (dcp == 0x064Du) base = DIAC_KASRATAN;
                }
                if (idx < MAX_WORD_LEN) {
                    uint8_t d = base;
                    if (sh) {
                        if (base == DIAC_FATHA) d = DIAC_SHADDA_FATHA;
                        else if (base == DIAC_DAMMA) d = DIAC_SHADDA_DAMMA;
                        else if (base == DIAC_KASRA) d = DIAC_SHADDA_KASRA;
                        else if (base == DIAC_FATHATAN) d = DIAC_SHADDA_FATHATAN;
                        else if (base == DIAC_DAMMATAN) d = DIAC_SHADDA_DAMMATAN;
                        else if (base == DIAC_KASRATAN) d = DIAC_SHADDA_KASRATAN;
                        else d = DIAC_SHADDA;
                    }
                    pw.diacs[idx] = d;
                }
            };
            pw.syms[0] = char_to_sym(cp);
            read_d(0);
            cnt = 1;
            while (pos < len) {
                size_t ns = pos;
                uint32_t ncp = utf8_next(s, len, pos);
                if (is_tatweel(ncp) || is_arabic_diacritic(ncp)) continue;
                if (is_arabic_letter(ncp)) {
                    if (cnt < MAX_WORD_LEN) pw.syms[cnt] = char_to_sym(ncp);
                    read_d(cnt);
                    ++cnt;
                } else {
                    pos = ns;
                    break;
                }
            }
            if (cnt > MAX_WORD_LEN) {
                pending_b = 1;
                continue;
            }
            pw.n = static_cast<uint8_t>(cnt);
            pw.si = analyze_surface(pw.syms, pw.n);
            pw.wid = eng.lookup_word_id(pw.syms, pw.n);
            if (pw.wid != UINT32_MAX && pw.wid < wid_cpos.size()) {
                pw.cpos = std::min<uint8_t>(wid_cpos[pw.wid], static_cast<uint8_t>(pw.n - 1));
                pw.top_verb = wid_top_verb[pw.wid];
            } else {
                pw.cpos = static_cast<uint8_t>(pw.n - 1);
                pw.top_verb = (pw.si.verb_like == 2) ? 1 : 0;
            }
            pw.has_suf = (pw.cpos + 1 < pw.n) ? 1 : 0;
            pw.end_id = pw.diacs[pw.cpos];
            pw.fam = diac_family(pw.end_id);
            pw.tan = diac_is_tanween(pw.end_id) ? 1 : 0;
            pw.is_fem_at = (pw.n >= pw.si.al_len + 5 && pw.syms[pw.n - 1] == S_TEH && pw.syms[pw.n - 2] == S_ALEF &&
                            pw.syms[pw.si.al_len] != S_ALEF_HAMZA_A && pw.syms[pw.si.al_len] != S_ALEF_HAMZA_B &&
                            pw.syms[pw.si.al_len] != S_ALEF) ? 1 : 0;
            pw.is_kana = (pw.si.particle == PART_KANA || is_kana_sym(pw.syms, pw.n)) ? 1 : 0;
            pw.is_inna = (pw.si.particle == PART_INNA ||
                          ((pw.si.particle == PART_AN || pw.si.particle == PART_LI_AN ||
                            pw.si.particle == PART_LAKIN || pw.si.particle == PART_IN) && diac_has_shadda(pw.end_id)) ||
                          is_inna_pron_sym(pw.syms, pw.n)) ? 1 : 0;
            // Lightweight accurate verb/voice/subject classification from gold vowels
            bool yeh_sukun_noun = (pw.n == 3 && pw.syms[0] == S_YEH && pw.diacs[1] == DIAC_SUKUN);
            if (!pw.is_kana && !yeh_sukun_noun && pw.si.particle == PART_NONE && pw.si.al_len == 0 && !pw.si.has_prep &&
                pw.syms[pw.n - 1] != S_TEH_MARBUTA && !pw.tan && pw.fam != FAM_KASRA) {
                if (pw.top_verb == 1 || (pw.si.verb_like == 2 && pw.wid == UINT32_MAX)) {
                    pw.is_verb = 1;
                    size_t st = (pw.n >= 4 && (pw.syms[0] == S_WAW || pw.syms[0] == S_FEH)) ? 1 : 0;
                    if (pw.n - st >= 4 && pw.syms[st] == S_SEEN) ++st;
                    bool imp = (pw.syms[st] == S_YEH || pw.syms[st] == S_TEH || pw.syms[st] == S_NOON || pw.syms[st] == S_ALEF_HAMZA_A);
                    if (imp) {
                        if (pw.diacs[st] == DIAC_DAMMA && pw.cpos >= 2 && pw.diacs[pw.cpos - 1] == DIAC_FATHA) pw.is_pass = 1;
                        if (pw.syms[st] == S_NOON || pw.syms[st] == S_ALEF_HAMZA_A) pw.is_subj12 = 1;
                    } else {
                        if (pw.diacs[st] == DIAC_DAMMA && pw.cpos >= 2 && diac_family(pw.diacs[pw.cpos - 1]) == FAM_KASRA) pw.is_pass = 1;
                        if (pw.n >= st + 4 && pw.syms[pw.cpos] == S_TEH && pw.diacs[pw.cpos] == DIAC_SUKUN) pw.is_past_fem = 1;
                        if (pw.n >= st + 4 && pw.syms[pw.cpos] == S_TEH && pw.cpos >= 1 && pw.diacs[pw.cpos - 1] == DIAC_SUKUN &&
                            (pw.diacs[pw.cpos] == DIAC_DAMMA || pw.diacs[pw.cpos] == DIAC_FATHA || pw.diacs[pw.cpos] == DIAC_KASRA)) {
                            pw.is_subj12 = 1;
                        }
                    }
                    if (pw.has_suf) pw.is_subj12 = 1;
                }
            }
            uint8_t lskip = (pw.si.al_len >= 2) ? pw.si.al_lam_pos : 255;
            pw.core_h = compute_core_hash(pw.syms, pw.diacs, pw.cpos, lskip);
            out.push_back(pw);
        } else if (is_boundary_cp_ext(cp)) {
            pending_b = 1;
        }
    }
}

struct ValAccum {
    uint32_t self_tot = 0;
    uint32_t self_verb = 0;
    uint32_t self_fem_agree = 0;
    uint32_t self_indef = 0;
    uint32_t self_tan = 0;
    uint32_t self_mudaf = 0;
    uint32_t self_adj = 0;
    uint32_t self_adv_acc = 0;
    uint32_t nx1_nom = 0;
    uint32_t nx1_acc = 0;
    uint32_t nx1_gen = 0;
    uint32_t nx1_prep = 0;
    uint32_t v_subj_acc = 0;
    uint32_t v_subj_nonacc = 0;
    uint32_t nx2_nom = 0;
    uint32_t nx2_acc = 0;
    uint32_t nx2_gen = 0;
    uint32_t end_cnt[NUM_DIACRITICS] = {};
};

struct SmallTop4 {
    uint32_t tot = 0;
    uint32_t target_wid = 0;
    uint8_t order = 0;
    uint16_t keys[4] = {0, 0, 0, 0};
    uint32_t cnts[4] = {0, 0, 0, 0};

    inline void add(uint16_t k, uint32_t c = 1) {
        tot += c;
        for (int i = 0; i < 4; ++i) {
            if (cnts[i] == 0) { keys[i] = k; cnts[i] = c; return; }
            if (keys[i] == k) { cnts[i] += c; return; }
        }
        int min_i = 0;
        for (int i = 1; i < 4; ++i) if (cnts[i] < cnts[min_i]) min_i = i;
        if (c > cnts[min_i]) { keys[min_i] = k; cnts[min_i] = c; }
    }

    inline void merge(const SmallTop4& o) {
        tot += o.tot;
        for (int i = 0; i < 4; ++i) {
            if (o.cnts[i] == 0) continue;
            bool found = false;
            for (int j = 0; j < 4; ++j) {
                if (cnts[j] == 0) { keys[j] = o.keys[i]; cnts[j] = o.cnts[i]; found = true; break; }
                if (keys[j] == o.keys[i]) { cnts[j] += o.cnts[i]; found = true; break; }
            }
            if (!found) {
                int min_j = 0;
                for (int j = 1; j < 4; ++j) if (cnts[j] < cnts[min_j]) min_j = j;
                if (o.cnts[i] > cnts[min_j]) { keys[min_j] = o.keys[i]; cnts[min_j] = o.cnts[i]; }
            }
        }
    }
};

static std::vector<std::pair<uint64_t, uint64_t>> get_chunks(const std::string& path, int n_threads) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    uint64_t fsize = static_cast<uint64_t>(f.tellg());
    std::vector<uint64_t> bounds;
    bounds.push_back(0);
    for (int i = 1; i < n_threads; ++i) {
        uint64_t pos = (fsize * i) / n_threads;
        f.seekg(static_cast<std::streamoff>(pos));
        std::string dummy;
        std::getline(f, dummy);
        uint64_t tell = static_cast<uint64_t>(f.tellg());
        if (tell > bounds.back() && tell < fsize) bounds.push_back(tell);
    }
    bounds.push_back(fsize);
    std::vector<std::pair<uint64_t, uint64_t>> res;
    for (size_t i = 0; i + 1 < bounds.size(); ++i) res.push_back({bounds[i], bounds[i + 1]});
    return res;
}

static inline uint8_t word_state3(const ParsedWord& pw, const std::vector<uint32_t>& valency) {
    uint8_t sr = 11;
    if (pw.si.particle == PART_GEN || pw.si.has_prep) {
        sr = 1;
    } else if (pw.is_kana) {
        sr = 7;
    } else if (pw.is_inna) {
        sr = 6;
    } else if (pw.is_verb) {
        if (pw.is_pass) sr = 4;
        else if (pw.is_subj12) sr = 5;
        else {
            uint8_t vt = (pw.wid < valency.size()) ? static_cast<uint8_t>(valency[pw.wid] & 3u) : 0;
            sr = (vt == 1) ? 2 : 3;
        }
    } else if (pw.si.particle == PART_IDAFA_NOUN || pw.si.particle == PART_NUM_IDAFA) {
        sr = 10;
    } else if (pw.si.particle == PART_NONE) {
        if (pw.si.al_len > 0 || pw.has_suf) sr = 8;
        else if (pw.tan) sr = 9;
        else sr = 10;
    } else {
        sr = 11;
    }
    uint8_t fm = pw.fam < NUM_TRANS3_FAMS ? pw.fam : 0;
    return static_cast<uint8_t>(sr * NUM_TRANS3_FAMS + fm);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <baseline.tash> <corpus_strict_train.txt> <out_dir> [threads=4]\n";
        return 1;
    }
    std::string model_path = argv[1];
    std::string corpus_path = argv[2];
    std::string out_dir = argv[3];
    int n_threads = (argc >= 5) ? std::max(1, std::atoi(argv[4])) : 4;

    Diacritizer eng;
    std::string err;
    if (!eng.load(model_path, &err)) {
        std::cerr << "Failed to load model: " << err << "\n";
        return 1;
    }
    const uint32_t V = eng.vocab_size();
    std::cout << "Loaded model vocab=" << V << " threads=" << n_threads << "\n";

    // Pre-scan corpus sample? No, extract wid metadata directly as we see each word!
    // Actually we can lazily populate wid_cpos / wid_top_verb / wid_multi_core during a quick pre-scan OR inline with atomics.
    // Even faster: pre-scan first 100k lines? Better: in Pass A, each thread resolves on first encounter into a read-only cache, OR pre-compute for all seen words in Pass A!
    std::vector<uint8_t> wid_cpos(V, 255);
    std::vector<uint8_t> wid_top_verb(V, 0);
    std::vector<uint8_t> wid_multi_core(V, 0);

    auto chunks = get_chunks(corpus_path, n_threads);
    std::cout << "[Pass A] Extracting Valency, Word Priors, and Core Ambiguity over " << chunks.size() << " chunks...\n";

    // Thread-local accumulators
    std::vector<std::vector<ValAccum>> tl_val(chunks.size(), std::vector<ValAccum>(V));
    std::vector<std::unordered_map<uint32_t, SmallTop4>> tl_wcore(chunks.size());
    std::vector<std::unordered_map<uint32_t, LexCorePrior>> tl_lexcore(chunks.size());
    std::vector<std::vector<uint8_t>> tl_cpos(chunks.size(), std::vector<uint8_t>(V, 255));
    std::vector<std::vector<uint8_t>> tl_tverb(chunks.size(), std::vector<uint8_t>(V, 0));
    std::vector<std::vector<uint8_t>> tl_mcore(chunks.size(), std::vector<uint8_t>(V, 0));
    std::vector<std::vector<uint8_t>> tl_force_hom(chunks.size(), std::vector<uint8_t>(V, 0));
    std::vector<uint64_t> tl_lines(chunks.size(), 0);

    auto passA_worker = [&](size_t tid) {
        std::ifstream f(corpus_path, std::ios::binary);
        f.seekg(static_cast<std::streamoff>(chunks[tid].first));
        const uint64_t end_pos = chunks[tid].second;
        auto& va = tl_val[tid];
        auto& wc = tl_wcore[tid];
        auto& lcp = tl_lexcore[tid];
        auto& cp_arr = tl_cpos[tid];
        auto& tv_arr = tl_tverb[tid];
        auto& mc_arr = tl_mcore[tid];
        auto& fh_arr = tl_force_hom[tid];
        std::string line;
        std::vector<ParsedWord> toks;
        toks.reserve(64);

        while (static_cast<uint64_t>(f.tellg()) < end_pos && std::getline(f, line)) {
            if (line.empty()) continue;
            ++tl_lines[tid];
            // Quick first parse: resolve wid_cpos/wid_top_verb on demand
            const char* s = line.data();
            size_t len = line.size(), pos = 0;
            uint8_t pending_b = 1;
            toks.clear();
            while (pos < len) {
                uint32_t cp = utf8_next(s, len, pos);
                if (is_arabic_letter(cp)) {
                    ParsedWord pw{};
                    pw.boundary_before = pending_b;
                    pending_b = 0;
                    size_t cnt = 0;
                    auto read_d = [&](size_t idx) {
                        bool sh = false; uint8_t base = DIAC_NONE;
                        while (pos < len) {
                            size_t ps = pos;
                            uint32_t dcp = utf8_next(s, len, pos);
                            if (is_tatweel(dcp)) continue;
                            if (!is_arabic_diacritic(dcp)) { pos = ps; break; }
                            if (dcp == 0x0651u) sh = true;
                            else if (dcp == 0x064Eu) base = DIAC_FATHA;
                            else if (dcp == 0x064Fu) base = DIAC_DAMMA;
                            else if (dcp == 0x0650u) base = DIAC_KASRA;
                            else if (dcp == 0x0652u) base = DIAC_SUKUN;
                            else if (dcp == 0x064Bu) base = DIAC_FATHATAN;
                            else if (dcp == 0x064Cu) base = DIAC_DAMMATAN;
                            else if (dcp == 0x064Du) base = DIAC_KASRATAN;
                        }
                        if (idx < MAX_WORD_LEN) {
                            uint8_t d = base;
                            if (sh) {
                                if (base == DIAC_FATHA) d = DIAC_SHADDA_FATHA;
                                else if (base == DIAC_DAMMA) d = DIAC_SHADDA_DAMMA;
                                else if (base == DIAC_KASRA) d = DIAC_SHADDA_KASRA;
                                else if (base == DIAC_FATHATAN) d = DIAC_SHADDA_FATHATAN;
                                else if (base == DIAC_DAMMATAN) d = DIAC_SHADDA_DAMMATAN;
                                else if (base == DIAC_KASRATAN) d = DIAC_SHADDA_KASRATAN;
                                else d = DIAC_SHADDA;
                            }
                            pw.diacs[idx] = d;
                        }
                    };
                    pw.syms[0] = char_to_sym(cp);
                    read_d(0);
                    cnt = 1;
                    while (pos < len) {
                        size_t ns = pos;
                        uint32_t ncp = utf8_next(s, len, pos);
                        if (is_tatweel(ncp) || is_arabic_diacritic(ncp)) continue;
                        if (is_arabic_letter(ncp)) {
                            if (cnt < MAX_WORD_LEN) pw.syms[cnt] = char_to_sym(ncp);
                            read_d(cnt);
                            ++cnt;
                        } else { pos = ns; break; }
                    }
                    if (cnt > MAX_WORD_LEN) { pending_b = 1; continue; }
                    pw.n = static_cast<uint8_t>(cnt);
                    pw.si = analyze_surface(pw.syms, pw.n);
                    pw.wid = eng.lookup_word_id(pw.syms, pw.n);
                    if (pw.wid != UINT32_MAX && pw.wid < V) {
                        if (cp_arr[pw.wid] == 255) {
                            uint8_t oc = 0, oe = 0, ov = 0;
                            eng.extract_word_top_info(pw.wid, pw.syms, pw.n, oc, oe, ov);
                            cp_arr[pw.wid] = oc;
                            tv_arr[pw.wid] = ov;
                            bool is_mc = eng.word_has_multi_core(pw.wid, pw.syms, pw.n);
                            mc_arr[pw.wid] = is_mc ? 1 : 0;
                            if (is_mc) {
                                LexCorePrior lp{};
                                eng.extract_word_core_priors(pw.wid, pw.syms, pw.n, lp.h, lp.p, lp.cnt);
                                lcp[pw.wid] = lp;
                                if (pw.si.particle == PART_IN || pw.si.particle == PART_AN || pw.si.particle == PART_LI_AN ||
                                    pw.si.particle == PART_LAKIN || pw.si.particle == PART_AY || pw.si.particle == PART_LAMA ||
                                    is_man_or_an_prep(pw.syms, pw.n)) {
                                    fh_arr[pw.wid] = 1;
                                }
                            }
                        }
                        pw.cpos = std::min<uint8_t>(cp_arr[pw.wid], static_cast<uint8_t>(pw.n - 1));
                        pw.top_verb = tv_arr[pw.wid];
                    } else {
                        pw.cpos = static_cast<uint8_t>(pw.n - 1);
                        pw.top_verb = (pw.si.verb_like == 2) ? 1 : 0;
                    }
                    pw.has_suf = (pw.cpos + 1 < pw.n) ? 1 : 0;
                    pw.end_id = pw.diacs[pw.cpos];
                    pw.fam = diac_family(pw.end_id);
                    pw.tan = diac_is_tanween(pw.end_id) ? 1 : 0;
                    pw.is_fem_at = (pw.n >= pw.si.al_len + 5 && pw.syms[pw.n - 1] == S_TEH && pw.syms[pw.n - 2] == S_ALEF &&
                                    pw.syms[pw.si.al_len] != S_ALEF_HAMZA_A && pw.syms[pw.si.al_len] != S_ALEF_HAMZA_B &&
                                    pw.syms[pw.si.al_len] != S_ALEF) ? 1 : 0;
                    pw.is_kana = (pw.si.particle == PART_KANA || is_kana_sym(pw.syms, pw.n)) ? 1 : 0;
                    pw.is_inna = (pw.si.particle == PART_INNA ||
                                  ((pw.si.particle == PART_AN || pw.si.particle == PART_LI_AN ||
                                    pw.si.particle == PART_LAKIN || pw.si.particle == PART_IN) && diac_has_shadda(pw.end_id)) ||
                                  is_inna_pron_sym(pw.syms, pw.n)) ? 1 : 0;
                    bool yeh_suk_n = (pw.n == 3 && pw.syms[0] == S_YEH && pw.diacs[1] == DIAC_SUKUN);
                    if (!pw.is_kana && !yeh_suk_n && pw.si.particle == PART_NONE && pw.si.al_len == 0 && !pw.si.has_prep &&
                        pw.syms[pw.n - 1] != S_TEH_MARBUTA && !pw.tan && pw.fam != FAM_KASRA &&
                        (pw.top_verb == 1 || (pw.si.verb_like == 2 && pw.wid == UINT32_MAX))) {
                        pw.is_verb = 1;
                        size_t st = (pw.n >= 4 && (pw.syms[0] == S_WAW || pw.syms[0] == S_FEH)) ? 1 : 0;
                        if (pw.n - st >= 4 && pw.syms[st] == S_SEEN) ++st;
                        bool imp = (pw.syms[st] == S_YEH || pw.syms[st] == S_TEH || pw.syms[st] == S_NOON || pw.syms[st] == S_ALEF_HAMZA_A);
                        if (!imp && pw.n >= st + 4 && pw.syms[pw.cpos] == S_TEH && pw.diacs[pw.cpos] == DIAC_SUKUN) pw.is_past_fem = 1;
                    }
                    uint8_t lskip = (pw.si.al_len >= 2) ? pw.si.al_lam_pos : 255;
                    pw.core_h = compute_core_hash(pw.syms, pw.diacs, pw.cpos, lskip);
                    toks.push_back(pw);
                } else if (is_boundary_cp_ext(cp)) {
                    pending_b = 1;
                }
            }

            const size_t W = toks.size();
            for (size_t i = 0; i < W; ++i) {
                const ParsedWord& t = toks[i];
                if (t.wid >= V) continue;
                ValAccum& a = va[t.wid];
                ++a.self_tot;
                if (t.end_id < NUM_DIACRITICS) ++a.end_cnt[t.end_id];
                if (t.is_verb) ++a.self_verb;
                if (mc_arr[t.wid]) wc[t.wid].add(t.core_h);

                bool is_bare_noun = (!t.is_verb && !t.is_kana && !t.is_inna && t.si.particle == PART_NONE && !t.si.has_prep);
                if (is_bare_noun && t.si.al_len == 0 && !t.has_suf) {
                    ++a.self_indef;
                    if (t.tan) ++a.self_tan;
                }
                const ParsedWord* p = (i >= 1 && !t.boundary_before) ? &toks[i - 1] : nullptr;
                const ParsedWord* nx = (i + 1 < W && !toks[i + 1].boundary_before) ? &toks[i + 1] : nullptr;
                const ParsedWord* nx2 = (nx && i + 2 < W && !toks[i + 2].boundary_before) ? &toks[i + 2] : nullptr;

                // Feminine agreement detection (bare feminine nouns without ة!)
                if (is_bare_noun && t.syms[t.n - 1] != S_TEH_MARBUTA) {
                    if (p && p->is_past_fem && t.fam == FAM_DAMMA) ++a.self_fem_agree;
                    if (nx && nx->si.particle == PART_NONE && !nx->is_verb && !nx->si.has_prep &&
                        nx->syms[nx->n - 1] == S_TEH_MARBUTA && nx->fam == t.fam && (nx->si.al_len > 0) == (t.si.al_len > 0)) {
                        ++a.self_fem_agree;
                    }
                }
                // Mudaf / Adj / Adverbial behavior of t itself
                if (is_bare_noun) {
                    if (t.si.al_len == 0 && !t.has_suf && !t.tan && nx && nx->si.particle == PART_NONE && !nx->is_verb && nx->fam == FAM_KASRA) {
                        ++a.self_mudaf;
                    }
                    if (p && p->si.particle == PART_NONE && !p->is_verb && !p->si.has_prep &&
                        p->fam == t.fam && p->fam != FAM_NONE && p->fam != FAM_SUKUN &&
                        ((t.si.al_len > 0 && p->si.al_len > 0) || (t.si.al_len == 0 && p->si.al_len == 0 && t.tan && p->tan))) {
                        ++a.self_adj;
                    }
                    bool prev_gov_acc = p && (p->is_verb || p->is_inna || p->is_kana || p->si.particle == PART_GEN || p->si.has_prep);
                    if (!prev_gov_acc && t.fam == FAM_FATHA) ++a.self_adv_acc;
                }
                // Right-1 case preference
                if (nx) {
                    if (nx->si.particle == PART_GEN || nx->si.has_prep) {
                        ++a.nx1_prep;
                    } else if (!nx->is_verb && !nx->is_kana && !nx->is_inna && nx->si.particle == PART_NONE) {
                        bool nx_acc = (!nx->is_fem_at && nx->fam == FAM_FATHA) || (nx->is_fem_at && nx->fam == FAM_KASRA && (t.is_verb || t.is_inna || t.is_kana));
                        if (nx->fam == FAM_DAMMA) ++a.nx1_nom;
                        else if (nx_acc) ++a.nx1_acc;
                        else if (nx->fam == FAM_KASRA) ++a.nx1_gen;
                    }
                }
                // Right-2 case preference
                if (nx2 && !nx2->is_verb && !nx2->is_kana && !nx2->is_inna && nx2->si.particle == PART_NONE && !nx2->si.has_prep) {
                    if (nx2->fam == FAM_DAMMA) ++a.nx2_nom;
                    else if (nx2->fam == FAM_FATHA) ++a.nx2_acc;
                    else if (nx2->fam == FAM_KASRA) ++a.nx2_gen;
                }
                // Verb transitivity scan: after verb + nominative subject (skipping mudaf/adj up to 4 words), check if ACC object appears
                if (t.is_verb && nx && !nx->is_verb && nx->si.particle == PART_NONE && !nx->si.has_prep && nx->fam == FAM_DAMMA) {
                    bool saw_acc = false;
                    bool saw_other = false;
                    for (size_t j = i + 2; j < W && j <= i + 5; ++j) {
                        const ParsedWord& wj = toks[j];
                        if (wj.boundary_before || wj.is_verb || wj.is_kana || wj.is_inna ||
                            wj.si.particle == PART_REL || wj.si.particle == PART_COND) break;
                        if (wj.si.particle == PART_GEN || wj.si.has_prep) { saw_other = true; break; }
                        if (wj.si.particle == PART_NONE && !wj.is_verb) {
                            if (wj.fam == FAM_KASRA && !wj.is_fem_at) continue; // skip mudaf ilayh
                            if (wj.fam == FAM_DAMMA) continue; // skip adj of subject
                            if (wj.fam == FAM_FATHA || (wj.is_fem_at && wj.fam == FAM_KASRA)) { saw_acc = true; break; }
                        }
                    }
                    if (saw_acc) ++a.v_subj_acc;
                    else if (saw_other || i + 2 == W || (i + 2 < W && toks[i + 2].boundary_before)) ++a.v_subj_nonacc;
                }
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t t = 0; t < chunks.size(); ++t) threads.emplace_back(passA_worker, t);
    for (auto& th : threads) th.join();

    // Merge Pass A
    uint64_t total_lines = 0;
    std::vector<ValAccum> g_val(V);
    std::unordered_map<uint32_t, SmallTop4> g_wcore;
    std::unordered_map<uint32_t, LexCorePrior> g_lexcore;
    std::vector<uint8_t> wid_force_hom(V, 0);
    for (size_t t = 0; t < chunks.size(); ++t) {
        total_lines += tl_lines[t];
        for (uint32_t w = 0; w < V; ++w) {
            if (wid_cpos[w] == 255 && tl_cpos[t][w] != 255) {
                wid_cpos[w] = tl_cpos[t][w];
                wid_top_verb[w] = tl_tverb[t][w];
                wid_multi_core[w] = tl_mcore[t][w];
                wid_force_hom[w] = tl_force_hom[t][w];
            }
            const ValAccum& src = tl_val[t][w];
            if (src.self_tot == 0) continue;
            ValAccum& dst = g_val[w];
            dst.self_tot += src.self_tot;
            dst.self_verb += src.self_verb;
            dst.self_fem_agree += src.self_fem_agree;
            dst.self_indef += src.self_indef;
            dst.self_tan += src.self_tan;
            dst.self_mudaf += src.self_mudaf;
            dst.self_adj += src.self_adj;
            dst.self_adv_acc += src.self_adv_acc;
            dst.nx1_nom += src.nx1_nom;
            dst.nx1_acc += src.nx1_acc;
            dst.nx1_gen += src.nx1_gen;
            dst.nx1_prep += src.nx1_prep;
            dst.v_subj_acc += src.v_subj_acc;
            dst.v_subj_nonacc += src.v_subj_nonacc;
            dst.nx2_nom += src.nx2_nom;
            dst.nx2_acc += src.nx2_acc;
            dst.nx2_gen += src.nx2_gen;
            for (int d = 0; d < NUM_DIACRITICS; ++d) dst.end_cnt[d] += src.end_cnt[d];
        }
        for (auto& kv : tl_wcore[t]) g_wcore[kv.first].merge(kv.second);
        for (auto& kv : tl_lexcore[t]) g_lexcore.emplace(kv.first, kv.second);
        tl_val[t].clear(); tl_val[t].shrink_to_fit();
        tl_wcore[t].clear();
        tl_lexcore[t].clear();
    }
    std::cout << "  Pass A merged " << total_lines << " lines | multi-core words tracked=" << g_wcore.size() << "\n";

    // Pack Valency table (uint32_t[V]) & compute word base ending log-probs and multi-end flags
    std::vector<uint32_t> valency_packed(V, 0);
    std::vector<uint32_t> word_freq(V, 0);
    std::vector<uint8_t> word_multi_end(V, 0);
    std::vector<std::array<float, NUM_DIACRITICS>> word_end_logp(V);
    uint32_t n_intr = 0, n_trans = 0, n_prep_v = 0, n_fem = 0, n_mudaf = 0, n_adj = 0, n_adv = 0;

    for (uint32_t w = 0; w < V; ++w) {
        const ValAccum& a = g_val[w];
        word_freq[w] = a.self_tot;
        double e_tot = 15.0 * 0.1;
        int n_dist_e = 0;
        for (int d = 0; d < NUM_DIACRITICS; ++d) {
            e_tot += a.end_cnt[d];
            if (a.end_cnt[d] >= 2) ++n_dist_e;
        }
        word_multi_end[w] = (n_dist_e >= 2) ? 1 : 0;
        for (int d = 0; d < NUM_DIACRITICS; ++d) {
            word_end_logp[w][d] = static_cast<float>(std::log((a.end_cnt[d] + 0.1) / e_tot));
        }
        if (a.self_tot < 5) continue;

        ValencyInfo vi{};
        double v_ratio = static_cast<double>(a.self_verb) / a.self_tot;
        vi.verb_prob = static_cast<uint8_t>(std::min<int>(15, static_cast<int>(std::round(v_ratio * 15.0))));

        uint32_t nx1_tot = a.nx1_nom + a.nx1_acc + a.nx1_gen;
        if (nx1_tot >= 6) {
            double pn = (a.nx1_nom + 1.0) / (nx1_tot + 3.0);
            double pa = (a.nx1_acc + 1.0) / (nx1_tot + 3.0);
            double pg = (a.nx1_gen + 1.0) / (nx1_tot + 3.0);
            vi.next_nom_p = static_cast<uint8_t>(std::min<int>(15, static_cast<int>(std::round(pn * 15.0))));
            vi.next_acc_p = static_cast<uint8_t>(std::min<int>(15, static_cast<int>(std::round(pa * 15.0))));
            vi.next_gen_p = static_cast<uint8_t>(std::min<int>(15, static_cast<int>(std::round(pg * 15.0))));
            if (pn >= 0.52) vi.next_case_pref = FAM_DAMMA;
            else if (pa >= 0.52) vi.next_case_pref = FAM_FATHA;
            else if (pg >= 0.58) vi.next_case_pref = FAM_KASRA;
        }
        uint32_t nx2_tot = a.nx2_nom + a.nx2_acc + a.nx2_gen;
        if (nx2_tot >= 8) {
            double pn2 = static_cast<double>(a.nx2_nom) / nx2_tot;
            double pa2 = static_cast<double>(a.nx2_acc) / nx2_tot;
            double pg2 = static_cast<double>(a.nx2_gen) / nx2_tot;
            if (pn2 >= 0.52) vi.next2_case_pref = FAM_DAMMA;
            else if (pa2 >= 0.52) vi.next2_case_pref = FAM_FATHA;
            else if (pg2 >= 0.58) vi.next2_case_pref = FAM_KASRA;
        }

        if (v_ratio >= 0.45 && a.self_verb >= 10) {
            double acc_ev = static_cast<double>(a.nx1_acc + a.v_subj_acc);
            double nonacc_ev = static_cast<double>(a.nx1_nom + a.v_subj_nonacc + a.nx1_prep);
            double prep_ratio = static_cast<double>(a.nx1_prep) / (nx1_tot + a.nx1_prep + 1.0);
            double acc_share = acc_ev / (acc_ev + a.v_subj_nonacc + a.nx1_nom * 0.5 + a.nx1_prep * 0.5 + 3.0);
            if (acc_ev + nonacc_ev >= 8) {
                double lo = 2.2 * std::log((acc_ev + 2.0) / (a.nx1_nom + a.v_subj_nonacc + 2.0));
                vi.val_odds = static_cast<int8_t>(std::max(-7, std::min(7, static_cast<int>(std::round(lo)))));
                if (acc_share >= 0.36 && acc_ev >= 5) { vi.v_trans = 2; ++n_trans; }
                else if (prep_ratio >= 0.52 && acc_share < 0.16) { vi.v_trans = 3; ++n_prep_v; }
                else if (acc_share < 0.14 && nonacc_ev >= 12) { vi.v_trans = 1; ++n_intr; }
            }
        } else {
            if (nx1_tot >= 6) {
                double lo = 2.0 * std::log((a.nx1_gen + 2.0) / (a.nx1_nom + a.nx1_acc + 2.0));
                vi.val_odds = static_cast<int8_t>(std::max(-7, std::min(7, static_cast<int>(std::round(lo)))));
            }
            double mud_r = static_cast<double>(a.self_mudaf) / a.self_tot;
            double adj_r = static_cast<double>(a.self_adj) / a.self_tot;
            double adv_r = static_cast<double>(a.self_adv_acc) / a.self_tot;
            if (a.self_tot >= 15 && adv_r >= 0.55 && mud_r < 0.35) { vi.word_syn_type = 3; ++n_adv; }
            else if (a.self_tot >= 12 && mud_r >= 0.48) { vi.word_syn_type = 1; ++n_mudaf; }
            else if (a.self_tot >= 12 && adj_r >= 0.42) { vi.word_syn_type = 2; ++n_adj; }
        }
        if (a.self_indef >= 5) {
            double tr = static_cast<double>(a.self_tan) / a.self_indef;
            vi.tan_ratio = static_cast<uint8_t>(std::min<int>(7, static_cast<int>(std::round(tr * 7.0))));
        }
        if (a.self_tot >= 12 && a.self_fem_agree >= 5 && (static_cast<double>(a.self_fem_agree) / a.self_tot) >= 0.18) {
            vi.is_gram_fem = 1;
            ++n_fem;
        }
        valency_packed[w] = pack_valency(vi);
    }
    std::cout << "  Valency summary: intrans=" << n_intr << " trans=" << n_trans << " prep_v=" << n_prep_v
              << " mudaf=" << n_mudaf << " adj=" << n_adj << " adv=" << n_adv << " bare_fem=" << n_fem << "\n";
    g_val.clear(); g_val.shrink_to_fit();

    // Write valency.bin
    {
        std::ofstream ov(out_dir + "/valency.bin", std::ios::binary);
        ov.write(reinterpret_cast<const char*>(valency_packed.data()), valency_packed.size() * sizeof(uint32_t));
    }

    // Pass B: 2nd-Order Trans3 + Multi-Context NgramV7 + CoreBgV7
    std::cout << "[Pass B] Extracting Trans3, Multi-Context NgramV7 & CoreBgV7...\n";
    std::vector<std::vector<uint32_t>> tl_tr3(chunks.size(), std::vector<uint32_t>(TRANS3_TABLE_SIZE, 0));
    std::vector<std::unordered_map<uint64_t, SmallTop4>> tl_ng(chunks.size());
    std::vector<std::unordered_map<uint64_t, SmallTop4>> tl_cb(chunks.size());

    auto passB_worker = [&](size_t tid) {
        std::ifstream f(corpus_path, std::ios::binary);
        f.seekg(static_cast<std::streamoff>(chunks[tid].first));
        const uint64_t end_pos = chunks[tid].second;
        auto& tr3 = tl_tr3[tid];
        auto& ngm = tl_ng[tid];
        auto& cbm = tl_cb[tid];
        ngm.reserve(800000);
        cbm.reserve(400000);
        std::string line;
        std::vector<ParsedWord> toks;
        toks.reserve(64);
        constexpr size_t MAX_NG_LOCAL = 4500000;
        constexpr size_t MAX_CB_LOCAL = 2500000;

        while (static_cast<uint64_t>(f.tellg()) < end_pos && std::getline(f, line)) {
            if (line.empty()) continue;
            parse_gold_line(eng, wid_cpos, wid_top_verb, line, toks);
            const size_t W = toks.size();
            uint8_t s2 = 0, s1 = 0;
            for (size_t i = 0; i < W; ++i) {
                const ParsedWord& t = toks[i];
                if (t.boundary_before) { s2 = 0; s1 = 0; }
                uint8_t s0 = word_state3(t, valency_packed);
                ++tr3[(static_cast<size_t>(s2) * NUM_TRANS3_STATES + s1) * NUM_TRANS3_STATES + s0];
                s2 = s1;
                s1 = s0;

                if (t.wid >= V || word_freq[t.wid] < 8) continue;
                const ParsedWord* p1 = (i >= 1 && !t.boundary_before && toks[i - 1].wid < V) ? &toks[i - 1] : nullptr;
                const ParsedWord* p2 = (p1 && i >= 2 && !p1->boundary_before && toks[i - 2].wid < V) ? &toks[i - 2] : nullptr;
                const ParsedWord* p3 = (p2 && i >= 3 && !p2->boundary_before && toks[i - 3].wid < V) ? &toks[i - 3] : nullptr;
                const ParsedWord* nx = (i + 1 < W && !toks[i + 1].boundary_before && toks[i + 1].wid < V) ? &toks[i + 1] : nullptr;

                // NgramV7 ending contexts: skip iltiqa register conflicts across ALL orders & noisy DIAC_NONE on strong consonants
                const bool iltiqa_conflict =
                    is_man_or_an_prep(t.syms, t.n) ||
                    (nx && nx->si.al_len > 0 && (is_plural_meem_ext(t.syms, t.n) || t.is_past_fem));
                const bool noisy_none_end = (t.end_id == DIAC_NONE && !is_weak_final_sym(t.syms[t.cpos]));
                if (word_multi_end[t.wid] && t.end_id < NUM_DIACRITICS && !iltiqa_conflict && !noisy_none_end) {
                    auto add_ng = [&](uint64_t key, uint8_t order) {
                        auto it = ngm.find(key);
                        if (it != ngm.end()) {
                            it->second.add(t.end_id);
                        } else if (ngm.size() < MAX_NG_LOCAL) {
                            SmallTop4 st;
                            st.target_wid = t.wid;
                            st.order = order;
                            st.add(t.end_id);
                            ngm.emplace(key, st);
                        }
                    };
                    if (nx && word_freq[nx->wid] >= 12) {
                        uint32_t wr[2] = {t.wid, nx->wid};
                        add_ng(NgramTableV6::fnv1a_key_tagged(wr, 2, NG_ORDER_RBG), NG_ORDER_RBG);
                        if (p1 && word_freq[p1->wid] >= 25 && word_freq[nx->wid] >= 25) {
                            uint32_t wc3[3] = {p1->wid, t.wid, nx->wid};
                            add_ng(NgramTableV6::fnv1a_key_tagged(wc3, 3, NG_ORDER_CTR3), NG_ORDER_CTR3);
                        }
                    }
                    if (p1 && p2 && word_freq[p1->wid] >= 25 && word_freq[p2->wid] >= 25) {
                        uint32_t w3[3] = {p2->wid, p1->wid, t.wid};
                        add_ng(NgramTableV6::fnv1a_key_tagged(w3, 3, 3), 3);
                        if (p3 && word_freq[p3->wid] >= 60) {
                            uint32_t w4[4] = {p3->wid, p2->wid, p1->wid, t.wid};
                            add_ng(NgramTableV6::fnv1a_key_tagged(w4, 4, 4), 4);
                        }
                    }
                    if (p2 && word_freq[p2->wid] >= 20) {
                        uint32_t ws1[2] = {p2->wid, t.wid};
                        add_ng(NgramTableV6::fnv1a_key_tagged(ws1, 2, NG_ORDER_SKIP1), NG_ORDER_SKIP1);
                    }
                }

                // CoreBgV7 core-disambiguation contexts (ONLY for multi-core ambiguous words!)
                if (wid_multi_core[t.wid]) {
                    auto add_cb = [&](uint64_t key, uint8_t dir) {
                        auto it = cbm.find(key);
                        if (it != cbm.end()) {
                            it->second.add(t.core_h);
                        } else if (cbm.size() < MAX_CB_LOCAL) {
                            SmallTop4 st;
                            st.target_wid = t.wid;
                            st.order = dir;
                            st.add(t.core_h);
                            cbm.emplace(key, st);
                        }
                    };
                    if (p1 && word_freq[p1->wid] >= 10) {
                        uint32_t wl[2] = {p1->wid, t.wid};
                        add_cb(NgramTableV6::fnv1a_key_tagged(wl, 2, static_cast<uint8_t>(0x80u | CORE_DIR_LEFT)), CORE_DIR_LEFT);
                    }
                    if (nx && word_freq[nx->wid] >= 10) {
                        uint32_t wr[2] = {t.wid, nx->wid};
                        add_cb(NgramTableV6::fnv1a_key_tagged(wr, 2, static_cast<uint8_t>(0x80u | CORE_DIR_RIGHT)), CORE_DIR_RIGHT);
                    }
                    if (p1 && nx && word_freq[p1->wid] >= 25 && word_freq[nx->wid] >= 25) {
                        uint32_t wc3[3] = {p1->wid, t.wid, nx->wid};
                        add_cb(NgramTableV6::fnv1a_key_tagged(wc3, 3, static_cast<uint8_t>(0x80u | CORE_DIR_CTR3)), CORE_DIR_CTR3);
                    }
                }
            }
        }
    };

    threads.clear();
    for (size_t t = 0; t < chunks.size(); ++t) threads.emplace_back(passB_worker, t);
    for (auto& th : threads) th.join();

    // 1. Merge & compute Trans3 PMI lift over 1st-order transition (Dirichlet backoff alpha=15)
    std::vector<uint64_t> tr3_cnt(TRANS3_TABLE_SIZE, 0);
    for (size_t t = 0; t < chunks.size(); ++t) {
        for (size_t k = 0; k < TRANS3_TABLE_SIZE; ++k) tr3_cnt[k] += tl_tr3[t][k];
        tl_tr3[t].clear(); tl_tr3[t].shrink_to_fit();
    }
    constexpr size_t S = NUM_TRANS3_STATES;
    std::vector<double> c12(S * S, 0.0), c2(S, 0.0), c23(S * S, 0.0);
    for (size_t a = 0; a < S; ++a) {
        for (size_t b = 0; b < S; ++b) {
            for (size_t c = 0; c < S; ++c) {
                double v = static_cast<double>(tr3_cnt[(a * S + b) * S + c]);
                c12[a * S + b] += v;
                c2[b] += v;
                c23[b * S + c] += v;
            }
        }
    }
    std::vector<int8_t> trans3_q(TRANS3_TABLE_SIZE, 0);
    const double alpha = 15.0;
    for (size_t a = 0; a < S; ++a) {
        for (size_t b = 0; b < S; ++b) {
            double denom12 = c12[a * S + b];
            for (size_t c = 0; c < S; ++c) {
                double p1 = (c23[b * S + c] + 0.5) / (c2[b] + 0.5 * S);
                double p2 = (static_cast<double>(tr3_cnt[(a * S + b) * S + c]) + alpha * p1) / (denom12 + alpha);
                double lift = (std::log(p2) - std::log(p1)) * 1000.0;
                int q = static_cast<int>(std::round(lift / 16.0));
                trans3_q[(a * S + b) * S + c] = static_cast<int8_t>(std::max(-127, std::min(127, q)));
            }
        }
    }
    {
        std::ofstream ot(out_dir + "/trans3.bin", std::ios::binary);
        ot.write(reinterpret_cast<const char*>(trans3_q.data()), trans3_q.size());
    }
    std::cout << "  Wrote trans3.bin (" << trans3_q.size() << " bytes)\n";

    // 2. Merge & build NgramV7 hash table (cap = 524288 -> max ~260k top entries)
    std::unordered_map<uint64_t, SmallTop4> g_ng;
    g_ng.reserve(tl_ng[0].size() * 2);
    for (size_t t = 0; t < chunks.size(); ++t) {
        for (const auto& kv : tl_ng[t]) {
            if (kv.second.tot < 2 && chunks.size() > 1) continue; // prune 1-hit wonders per shard
            auto it = g_ng.find(kv.first);
            if (it == g_ng.end()) g_ng.emplace(kv.first, kv.second);
            else it->second.merge(kv.second);
        }
        tl_ng[t].clear();
    }
    struct RankedNg {
        double score;
        uint64_t key;
        uint8_t order;
        uint8_t b_eid, a_eid;
        int16_t b_bon, a_bon;
    };
    std::vector<RankedNg> ranked_ng;
    ranked_ng.reserve(500000);
    for (const auto& kv : g_ng) {
        const SmallTop4& st = kv.second;
        uint32_t min_c = (st.order == NG_ORDER_SKIP1) ? 10 : ((st.order == NG_ORDER_RBG) ? 6 : 5);
        if (st.tot < min_c) continue;
        // Sort top 4 by count desc
        int idx[4] = {0, 1, 2, 3};
        std::sort(idx, idx + 4, [&](int x, int y) { return st.cnts[x] > st.cnts[y]; });
        if (st.cnts[idx[0]] == 0) continue;
        const auto& base_lp = word_end_logp[st.target_wid];
        uint8_t b_eid = static_cast<uint8_t>(st.keys[idx[0]]);
        double base_pb = std::exp(static_cast<double>(base_lp[b_eid]));
        constexpr double NG_ALPHA = 6.0;
        double p_b = (st.cnts[idx[0]] + NG_ALPHA * base_pb) / (st.tot + NG_ALPHA);
        int b_bon = static_cast<int>(std::round((std::log(p_b) - base_lp[b_eid]) * 850.0));
        b_bon = std::max(-3200, std::min(3200, b_bon));
        uint8_t a_eid = 0;
        int a_bon = 0;
        if (st.cnts[idx[1]] > 0) {
            a_eid = static_cast<uint8_t>(st.keys[idx[1]]);
            double base_pa = std::exp(static_cast<double>(base_lp[a_eid]));
            double p_a = (st.cnts[idx[1]] + NG_ALPHA * base_pa) / (st.tot + NG_ALPHA);
            a_bon = static_cast<int>(std::round((std::log(p_a) - base_lp[a_eid]) * 850.0));
            if (st.cnts[idx[1]] * 3 < st.tot) a_bon = std::min(0, a_bon);
            a_bon = std::max(-3200, std::min(3200, a_bon));
        }
        int max_abs = std::max(std::abs(b_bon), std::abs(a_bon));
        if (max_abs < 240) continue;
        double pri = std::log(1.0 + st.tot) * max_abs * (st.order == NG_ORDER_RBG || st.order == NG_ORDER_CTR3 ? 1.35 : 1.0);
        ranked_ng.push_back({pri, kv.first, st.order, b_eid, a_eid, static_cast<int16_t>(b_bon), static_cast<int16_t>(a_bon)});
    }
    g_ng.clear();
    const uint32_t NG_CAP = 524288;
    const size_t NG_KEEP = 280000;
    if (ranked_ng.size() > NG_KEEP) {
        std::nth_element(ranked_ng.begin(), ranked_ng.begin() + NG_KEEP, ranked_ng.end(),
                         [](const RankedNg& a, const RankedNg& b) { return a.score > b.score; });
        ranked_ng.resize(NG_KEEP);
    }
    std::vector<NgramEntryV6> ng_tbl(NG_CAP);
    std::memset(ng_tbl.data(), 0, ng_tbl.size() * sizeof(NgramEntryV6));
    uint32_t ng_placed = 0;
    uint32_t ng_by_ord[32] = {};
    for (const auto& r : ranked_ng) {
        uint32_t slot0 = static_cast<uint32_t>(r.key ^ (r.key >> 32)) & (NG_CAP - 1);
        for (uint32_t step = 0; step < 32; ++step) {
            NgramEntryV6& e = ng_tbl[(slot0 + step) & (NG_CAP - 1)];
            if (e.key_hash == 0) {
                e.key_hash = r.key;
                e.best_bonus = r.b_bon;
                e.best_ending_id = r.b_eid;
                e.order = r.order;
                e.alt_bonus = r.a_bon;
                e.alt_ending_id = r.a_eid;
                e.reserved = 0;
                ++ng_placed;
                if (r.order < 32) ++ng_by_ord[r.order];
                break;
            }
        }
    }
    {
        std::ofstream on(out_dir + "/ngrams_v7.bin", std::ios::binary);
        on.write(reinterpret_cast<const char*>(&NG_CAP), 4);
        on.write(reinterpret_cast<const char*>(&ng_placed), 4);
        on.write(reinterpret_cast<const char*>(ng_tbl.data()), ng_tbl.size() * sizeof(NgramEntryV6));
    }
    std::cout << "  Wrote ngrams_v7.bin: placed=" << ng_placed << "/" << NG_CAP
              << " (ord3=" << ng_by_ord[3] << " ord4=" << ng_by_ord[4]
              << " rbg=" << ng_by_ord[NG_ORDER_RBG] << " ctr3=" << ng_by_ord[NG_ORDER_CTR3]
              << " skip1=" << ng_by_ord[NG_ORDER_SKIP1] << ")\n";
    ranked_ng.clear(); ng_tbl.clear();

    // 3. Merge & build CoreBgV7 hash table (cap = 262144 -> max ~130k top entries)
    std::unordered_map<uint64_t, SmallTop4> g_cb;
    g_cb.reserve(tl_cb[0].size() * 2);
    for (size_t t = 0; t < chunks.size(); ++t) {
        for (const auto& kv : tl_cb[t]) {
            if (kv.second.tot < 2 && chunks.size() > 1) continue;
            auto it = g_cb.find(kv.first);
            if (it == g_cb.end()) g_cb.emplace(kv.first, kv.second);
            else it->second.merge(kv.second);
        }
        tl_cb[t].clear();
    }
    // Two-Stage Information-Theoretic Homograph Context-Switch Test for CoreBgV7:
    // Only words whose context genuinely SWITCHES between >= 2 distinct cores (true homographs like ذَكَر/ذُكِر, آخِر/آخَر, عُمَر/عُمْر,
    // مِنْ/مَنْ, يُحَدِّث/يَحْدُث) pass; single-meaning orthographic/domain variants (السُّعودية/السَّعودية, سِبتمبر/سَبتمبر, وِزارة/وَزارة) are excluded!
    struct WinCores { uint16_t h1 = 0, h2 = 0; uint32_t w1 = 0, w2 = 0; };
    std::unordered_map<uint32_t, WinCores> wid_wins;
    wid_wins.reserve(g_wcore.size());
    for (const auto& kv : g_cb) {
        const SmallTop4& st = kv.second;
        if (st.tot < 6) continue;
        int best_i = 0;
        for (int i = 1; i < 4; ++i) if (st.cnts[i] > st.cnts[best_i]) best_i = i;
        if (st.cnts[best_i] * 100 < st.tot * 75) continue;
        uint16_t bh = st.keys[best_i];
        WinCores& wc = wid_wins[st.target_wid];
        if (wc.h1 == 0 || wc.h1 == bh) { wc.h1 = bh; ++wc.w1; }
        else if (wc.h2 == 0 || wc.h2 == bh) { wc.h2 = bh; ++wc.w2; }
        else { ++wc.w2; }
    }
    uint32_t n_true_hom = 0, n_orth_skip = 0;
    std::vector<uint8_t> wid_is_hom(V, 0);
    for (const auto& kv : wid_wins) {
        uint32_t w = kv.first;
        const WinCores& wc = kv.second;
        if (wid_force_hom[w] || (wc.h1 != 0 && wc.h2 != 0 && wc.w1 >= 1 && wc.w2 >= 1)) {
            wid_is_hom[w] = 1;
            ++n_true_hom;
        } else {
            ++n_orth_skip;
        }
    }
    for (uint32_t w = 0; w < V; ++w) if (wid_force_hom[w] && !wid_is_hom[w]) { wid_is_hom[w] = 1; ++n_true_hom; }
    std::cout << "  CoreBg Homograph Filter: true_homographs=" << n_true_hom << " orthographic_variants_filtered=" << n_orth_skip << "\n";

    struct RankedCb {
        double score;
        uint64_t key;
        uint16_t b_h, a_h;
        int16_t b_bon, a_bon;
        uint8_t dir;
    };
    std::vector<RankedCb> ranked_cb;
    ranked_cb.reserve(250000);
    for (const auto& kv : g_cb) {
        const SmallTop4& st = kv.second;
        if (st.target_wid >= V || !wid_is_hom[st.target_wid]) continue;
        uint32_t min_c = (st.order == CORE_DIR_CTR3) ? 5 : 6;
        if (st.tot < min_c) continue;
        auto wit = g_wcore.find(st.target_wid);
        if (wit == g_wcore.end() || wit->second.tot < 12) continue;
        const SmallTop4& wbase = wit->second;
        auto base_prob = [&](uint16_t h) -> double {
            for (int i = 0; i < 4; ++i) {
                if (wbase.keys[i] == h && wbase.cnts[i] > 0)
                    return (wbase.cnts[i] + 1.0) / (wbase.tot + 4.0);
            }
            return 1.0 / (wbase.tot + 4.0);
        };
        auto lit = g_lexcore.find(st.target_wid);
        auto lex_prob = [&](uint16_t h) -> double {
            if (lit == g_lexcore.end() || lit->second.cnt == 0) return 0.0;
            for (int i = 0; i < lit->second.cnt; ++i) {
                if (lit->second.h[i] == h) return static_cast<double>(lit->second.p[i]);
            }
            return 0.01;
        };
        uint16_t lex_top_h = (lit != g_lexcore.end() && lit->second.cnt > 0) ? lit->second.h[0] : 0;

        int idx[4] = {0, 1, 2, 3};
        std::sort(idx, idx + 4, [&](int x, int y) { return st.cnts[x] > st.cnts[y]; });
        if (st.cnts[idx[0]] == 0) continue;
        uint16_t b_h = st.keys[idx[0]];
        double share_b = static_cast<double>(st.cnts[idx[0]]) / st.tot;
        // Require stronger purity when overriding baseline lexicon top core!
        if (b_h != lex_top_h) {
            if (share_b < 0.78 || st.cnts[idx[0]] < 6) continue;
        } else {
            if (share_b < 0.70 || st.cnts[idx[0]] < 5) continue;
        }
        constexpr double CB_ALPHA = 6.0;
        double bp_b = base_prob(b_h);
        double lp_b = lex_prob(b_h);
        double ref_pb = std::max(bp_b, 0.5 * lp_b);
        double p_b = (st.cnts[idx[0]] + CB_ALPHA * bp_b) / (st.tot + CB_ALPHA);
        int b_bon = static_cast<int>(std::round((std::log(p_b) - std::log(std::max(1e-4, ref_pb))) * 850.0));
        b_bon = std::max(0, std::min(3200, b_bon));
        uint16_t a_h = 0;
        int a_bon = 0;
        if (st.cnts[idx[1]] > 0) {
            a_h = st.keys[idx[1]];
            double bp_a = base_prob(a_h);
            double p_a = (st.cnts[idx[1]] + CB_ALPHA * bp_a) / (st.tot + CB_ALPHA);
            a_bon = static_cast<int>(std::round((std::log(p_a) - std::log(bp_a)) * 850.0));
            // CRITICAL: NEVER give positive bonus to minority alternate core!
            a_bon = std::max(-3200, std::min(0, a_bon));
        }
        int max_abs = std::max(std::abs(b_bon), std::abs(a_bon));
        if (max_abs < 260) continue;
        double pri = std::log(1.0 + st.tot) * max_abs;
        ranked_cb.push_back({pri, kv.first, b_h, a_h, static_cast<int16_t>(b_bon), static_cast<int16_t>(a_bon), st.order});
    }
    g_cb.clear();
    const uint32_t CB_CAP = 524288;
    const size_t CB_KEEP = 180000;
    if (ranked_cb.size() > CB_KEEP) {
        std::nth_element(ranked_cb.begin(), ranked_cb.begin() + CB_KEEP, ranked_cb.end(),
                         [](const RankedCb& a, const RankedCb& b) { return a.score > b.score; });
        ranked_cb.resize(CB_KEEP);
    }
    std::vector<CoreBgEntryV7> cb_tbl(CB_CAP);
    std::memset(cb_tbl.data(), 0, cb_tbl.size() * sizeof(CoreBgEntryV7));
    uint32_t cb_placed = 0;
    uint32_t cb_dir[4] = {};
    for (const auto& r : ranked_cb) {
        uint32_t slot0 = static_cast<uint32_t>(r.key ^ (r.key >> 32)) & (CB_CAP - 1);
        for (uint32_t step = 0; step < 32; ++step) {
            CoreBgEntryV7& e = cb_tbl[(slot0 + step) & (CB_CAP - 1)];
            if (e.key_hash == 0) {
                e.key_hash = r.key;
                e.best_core_h = r.b_h;
                e.alt_core_h = r.a_h;
                e.best_bonus = r.b_bon;
                e.alt_bonus = r.a_bon;
                ++cb_placed;
                if (r.dir < 4) ++cb_dir[r.dir];
                break;
            }
        }
    }
    {
        std::ofstream oc(out_dir + "/core_bg_v7.bin", std::ios::binary);
        oc.write(reinterpret_cast<const char*>(&CB_CAP), 4);
        oc.write(reinterpret_cast<const char*>(&cb_placed), 4);
        oc.write(reinterpret_cast<const char*>(cb_tbl.data()), cb_tbl.size() * sizeof(CoreBgEntryV7));
    }
    std::cout << "  Wrote core_bg_v7.bin: placed=" << cb_placed << "/" << CB_CAP
              << " (L=" << cb_dir[0] << " R=" << cb_dir[1] << " C=" << cb_dir[2] << ")\n";
    return 0;
}
