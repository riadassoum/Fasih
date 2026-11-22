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

struct CoreFormEntry {
    uint16_t core_h = 0;
    uint32_t core_cnt = 0;
    uint32_t best_form_cnt = 0;
    uint8_t best_diac[MAX_WORD_LEN] = {};
};

struct WordCoreTracker {
    uint32_t total = 0;
    uint8_t n = 0;
    uint8_t syms[MAX_WORD_LEN] = {};
    // Compact top-8 distinct cores per word
    uint8_t num_cores = 0;
    CoreFormEntry cores[8];

    void add(uint16_t ch, const uint8_t* diacs, uint8_t len, uint32_t cnt = 1) {
        total += cnt;
        for (uint8_t i = 0; i < num_cores; ++i) {
            if (cores[i].core_h == ch) {
                cores[i].core_cnt += cnt;
                if (std::memcmp(cores[i].best_diac, diacs, len) == 0) {
                    cores[i].best_form_cnt += cnt;
                } else if (cnt > cores[i].best_form_cnt) {
                    cores[i].best_form_cnt = cnt;
                    std::memcpy(cores[i].best_diac, diacs, len);
                }
                return;
            }
        }
        if (num_cores < 8) {
            CoreFormEntry& e = cores[num_cores++];
            e.core_h = ch;
            e.core_cnt = cnt;
            e.best_form_cnt = cnt;
            std::memcpy(e.best_diac, diacs, len);
        } else {
            int min_i = 0;
            for (int i = 1; i < 8; ++i) if (cores[i].core_cnt < cores[min_i].core_cnt) min_i = i;
            if (cnt > cores[min_i].core_cnt) {
                CoreFormEntry& e = cores[min_i];
                e.core_h = ch;
                e.core_cnt = cnt;
                e.best_form_cnt = cnt;
                std::memcpy(e.best_diac, diacs, len);
            }
        }
    }

    void merge(const WordCoreTracker& o) {
        if (n == 0 && o.n > 0) {
            n = o.n;
            std::memcpy(syms, o.syms, o.n);
        }
        total += o.total;
        for (uint8_t i = 0; i < o.num_cores; ++i) {
            const auto& oc = o.cores[i];
            bool found = false;
            for (uint8_t j = 0; j < num_cores; ++j) {
                if (cores[j].core_h == oc.core_h) {
                    cores[j].core_cnt += oc.core_cnt;
                    if (std::memcmp(cores[j].best_diac, oc.best_diac, n) == 0) {
                        cores[j].best_form_cnt += oc.best_form_cnt;
                    } else if (oc.best_form_cnt > cores[j].best_form_cnt) {
                        cores[j].best_form_cnt = oc.best_form_cnt;
                        std::memcpy(cores[j].best_diac, oc.best_diac, n);
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (num_cores < 8) {
                    cores[num_cores++] = oc;
                } else {
                    int min_j = 0;
                    for (int j = 1; j < 8; ++j) if (cores[j].core_cnt < cores[min_j].core_cnt) min_j = j;
                    if (oc.core_cnt > cores[min_j].core_cnt) cores[min_j] = oc;
                }
            }
        }
    }
};

static inline bool is_clean_corpus_diac(const uint8_t* syms, uint8_t* diacs, uint8_t n, const SurfaceInfo& si, uint8_t cpos) {
    if (n < 2) return false;
    // Reject corrupted vowel on bare alef / alef madda (لاَ / آَنَ typo, اَل register leak)
    for (uint8_t p = 0; p < n; ++p) {
        if (syms[p] == S_ALEF || syms[p] == S_ALEF_WASLA || syms[p] == S_ALEF_MADDA) {
            if (diacs[p] != DIAC_NONE) return false;
        }
        if (syms[p] == S_ALEF_MAKSURA && diacs[p] != DIAC_NONE) return false;
    }
    if (si.al_len >= 2) {
        uint8_t lp = si.al_lam_pos;
        if (lp < n) {
            if (is_sun_letter(syms[si.al_len])) {
                if (diacs[lp] != DIAC_NONE) return false;
                if (!diac_has_shadda(diacs[si.al_len])) return false;
            } else {
                if (diacs[lp] != DIAC_NONE && diacs[lp] != DIAC_SUKUN) return false;
            }
        }
    }
    // Require genuine internal vocalization (reject partially undiacritized noise)
    uint8_t cons_cnt = 0, voc_cnt = 0;
    uint8_t lskip = (si.al_len >= 2) ? si.al_lam_pos : 255;
    for (uint8_t p = 0; p < cpos; ++p) {
        if (p == lskip) continue;
        uint8_t s = syms[p];
        if (s == S_ALEF || s == S_ALEF_WASLA || s == S_ALEF_MAKSURA || s == S_WAW || s == S_YEH) continue;
        ++cons_cnt;
        if (diacs[p] != DIAC_NONE) ++voc_cnt;
    }
    if (cons_cnt >= 2 && voc_cnt * 2 < cons_cnt) return false;
    if (cons_cnt == 1 && voc_cnt == 0) return false;
    // Require valid ending on non-weak case_pos
    uint8_t es = syms[cpos];
    if (es != S_ALEF && es != S_ALEF_MAKSURA && es != S_YEH && es != S_WAW && diacs[cpos] == DIAC_NONE) {
        return false;
    }
    // Reject double consecutive sukun inside stem
    for (uint8_t p = (si.al_len >= 2 ? si.al_len : 0); p + 1 < n; ++p) {
        if (diacs[p] == DIAC_SUKUN && diacs[p + 1] == DIAC_SUKUN) return false;
    }
    return true;
}

static inline WordCandidateV5 pack_cand_v5(uint32_t core_off, uint8_t eid, uint8_t fl, int16_t lp) {
    int32_t lpq = std::max<int32_t>(0, std::min<int32_t>(4095, (-static_cast<int32_t>(lp) + 1) / 3));
    uint64_t v = (static_cast<uint64_t>(core_off) & 0xFFFFFu) |
                 ((static_cast<uint64_t>(eid) & 0xFu) << 20) |
                 ((static_cast<uint64_t>(fl) & 0xFu) << 24) |
                 ((static_cast<uint64_t>(lpq) & 0xFFFu) << 28);
    WordCandidateV5 out{};
    std::memcpy(out.packed, &v, 5);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <model.tash> <corpus_strict_train.txt> <out_extra_core.bin> [threads=4]\n";
        return 1;
    }
    std::string model_path = argv[1];
    std::string corpus_path = argv[2];
    std::string out_path = argv[3];
    int n_threads = (argc >= 5) ? std::max(1, std::atoi(argv[4])) : 4;

    Diacritizer eng;
    std::string err;
    if (!eng.load(model_path, &err)) {
        std::cerr << "Failed to load model: " << err << "\n";
        return 1;
    }
    const uint32_t V = eng.vocab_size();
    std::cout << "Loaded model vocab=" << V << " threads=" << n_threads << "\n";

    auto chunks = get_chunks(corpus_path, n_threads);
    std::vector<std::unordered_map<uint32_t, WordCoreTracker>> tl_maps(chunks.size());
    std::vector<std::vector<uint8_t>> tl_cpos(chunks.size(), std::vector<uint8_t>(V, 255));
    std::vector<uint64_t> tl_lines(chunks.size(), 0);

    auto worker = [&](size_t tid) {
        std::ifstream f(corpus_path, std::ios::binary);
        f.seekg(static_cast<std::streamoff>(chunks[tid].first));
        const uint64_t end_pos = chunks[tid].second;
        auto& mp = tl_maps[tid];
        mp.reserve(200000);
        auto& cp_cache = tl_cpos[tid];
        std::string line;
        uint8_t syms[MAX_WORD_LEN], diacs[MAX_WORD_LEN];

        while (static_cast<uint64_t>(f.tellg()) < end_pos && std::getline(f, line)) {
            if (line.empty()) continue;
            ++tl_lines[tid];
            const char* s = line.data();
            const size_t len = line.size();
            size_t pos = 0;
            while (pos < len) {
                uint32_t cp = utf8_next(s, len, pos);
                if (!is_arabic_letter(cp)) continue;
                auto read_d = [&]() -> uint8_t {
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
                    if (!sh) return base;
                    if (base == DIAC_FATHA) return DIAC_SHADDA_FATHA;
                    if (base == DIAC_DAMMA) return DIAC_SHADDA_DAMMA;
                    if (base == DIAC_KASRA) return DIAC_SHADDA_KASRA;
                    if (base == DIAC_FATHATAN) return DIAC_SHADDA_FATHATAN;
                    if (base == DIAC_DAMMATAN) return DIAC_SHADDA_DAMMATAN;
                    if (base == DIAC_KASRATAN) return DIAC_SHADDA_KASRATAN;
                    return DIAC_SHADDA;
                };
                syms[0] = char_to_sym(cp);
                diacs[0] = read_d();
                size_t cnt = 1;
                while (pos < len) {
                    size_t ns = pos;
                    uint32_t ncp = utf8_next(s, len, pos);
                    if (is_tatweel(ncp) || is_arabic_diacritic(ncp)) continue;
                    if (is_arabic_letter(ncp)) {
                        if (cnt < MAX_WORD_LEN) {
                            syms[cnt] = char_to_sym(ncp);
                            diacs[cnt] = read_d();
                        } else {
                            (void)read_d();
                        }
                        ++cnt;
                    } else {
                        pos = ns;
                        break;
                    }
                }
                if (cnt < 2 || cnt > MAX_WORD_LEN) continue;
                uint32_t wid = eng.lookup_word_id(syms, cnt);
                if (wid == UINT32_MAX || wid >= V) continue;
                uint8_t cpos = cp_cache[wid];
                if (cpos == 255) {
                    uint8_t te = 0, tv = 0;
                    eng.extract_word_top_info(wid, syms, cnt, cpos, te, tv);
                    if (cpos >= cnt) cpos = static_cast<uint8_t>(cnt - 1);
                    cp_cache[wid] = cpos;
                }
                SurfaceInfo si = analyze_surface(syms, cnt);
                if (!is_clean_corpus_diac(syms, diacs, static_cast<uint8_t>(cnt), si, cpos)) continue;
                uint8_t lskip = (si.al_len >= 2) ? si.al_lam_pos : 255;
                uint16_t ch = compute_core_hash(syms, diacs, cpos, lskip);
                auto& tr = mp[wid];
                if (tr.n == 0) {
                    tr.n = static_cast<uint8_t>(cnt);
                    std::memcpy(tr.syms, syms, cnt);
                }
                tr.add(ch, diacs, static_cast<uint8_t>(cnt), 1);
            }
        }
    };

    std::vector<std::thread> ths;
    for (size_t t = 0; t < chunks.size(); ++t) ths.emplace_back(worker, t);
    for (auto& th : ths) th.join();

    uint64_t tot_lines = 0;
    for (auto c : tl_lines) tot_lines += c;
    std::cout << "Scanned " << tot_lines << " lines. Merging word core stats...\n";

    auto& merged = tl_maps[0];
    for (size_t t = 1; t < tl_maps.size(); ++t) {
        for (auto& kv : tl_maps[t]) {
            merged[kv.first].merge(kv.second);
        }
        tl_maps[t].clear();
    }

    // Filter to genuine evicted secondary cores not present in existing lexicon
    struct ExtraWordEntries {
        uint32_t wid;
        std::vector<std::pair<std::vector<uint8_t>, std::pair<uint8_t, int16_t>>> items; // (core_diacs[0..n-2], (end_id, lp))
    };
    std::vector<ExtraWordEntries> selected;
    selected.reserve(20000);
    std::vector<uint16_t> ex_hashes;
    uint64_t total_entries = 0;

    std::vector<uint32_t> wids;
    wids.reserve(merged.size());
    for (const auto& kv : merged) wids.push_back(kv.first);
    std::sort(wids.begin(), wids.end());

    for (uint32_t wid : wids) {
        const auto& tr = merged[wid];
        if (tr.total < 15 || tr.n < 2 || tr.num_cores < 2) continue;
        // Skip words that are purely particles/closed function words? No, check existing cores!
        int16_t top_lp = 0;
        eng.extract_word_existing_cores(wid, tr.syms, tr.n, ex_hashes, top_lp);
        if (ex_hashes.empty()) continue;

        // Sort observed cores by frequency descending
        std::vector<const CoreFormEntry*> cands;
        for (uint8_t i = 0; i < tr.num_cores; ++i) cands.push_back(&tr.cores[i]);
        std::sort(cands.begin(), cands.end(), [](const CoreFormEntry* a, const CoreFormEntry* b) {
            return a->core_cnt > b->core_cnt;
        });

        ExtraWordEntries ewe;
        ewe.wid = wid;
        for (const CoreFormEntry* ce : cands) {
            if (ewe.items.size() >= 3) break;
            bool exists = false;
            for (uint16_t eh : ex_hashes) {
                if (eh == ce->core_h) { exists = true; break; }
            }
            if (exists) continue;
            double share = static_cast<double>(ce->core_cnt) / static_cast<double>(tr.total);
            bool keep = (ce->core_cnt >= 6 && share >= 0.015) || (ce->core_cnt >= 15 && share >= 0.005);
            if (!keep) continue;

            // Compute corpus log-prob capped safely below top_lp - 150 so neutral context never hijacks #1 prior
            double p_form = static_cast<double>(ce->core_cnt) / static_cast<double>(tr.total);
            int32_t lp_raw = static_cast<int32_t>(std::round(1000.0 * std::log(std::max(p_form, 1e-5))));
            int32_t lp_cap = std::min<int32_t>(lp_raw, static_cast<int32_t>(top_lp) - 150);
            lp_cap = std::max<int32_t>(-12285, std::min<int32_t>(0, lp_cap));

            std::vector<uint8_t> core_seq(tr.n - 1);
            for (size_t p = 0; p + 1 < tr.n; ++p) core_seq[p] = ce->best_diac[p];
            uint8_t end_id = ce->best_diac[tr.n - 1];
            ewe.items.push_back({std::move(core_seq), {end_id, static_cast<int16_t>(lp_cap)}});
            ex_hashes.push_back(ce->core_h);
        }
        if (!ewe.items.empty()) {
            total_entries += ewe.items.size();
            selected.push_back(std::move(ewe));
        }
    }

    std::cout << "Selected " << selected.size() << " words with " << total_entries << " evicted secondary cores!\n";

    // Build deduplicated 4-bit nibble core_pool + bitmap + superblocks + entry_off + WordCandidateV5
    uint32_t bitmap_words = (V + 63u) / 64u;
    uint32_t super_blocks = (V + 255u) / 256u;
    std::vector<uint64_t> bits(bitmap_words, 0);
    std::vector<uint32_t> supers(super_blocks, 0);
    std::vector<uint32_t> entry_off;
    entry_off.reserve(selected.size() + 1);
    std::vector<WordCandidateV5> cands_v5;
    cands_v5.reserve(total_entries);

    std::unordered_map<std::string, uint32_t> pool_dedup;
    std::vector<uint8_t> core_nibbles;
    core_nibbles.reserve(total_entries * 5);

    for (const auto& ewe : selected) {
        bits[ewe.wid >> 6] |= (1ULL << (ewe.wid & 63u));
        entry_off.push_back(static_cast<uint32_t>(cands_v5.size()));
        for (const auto& it : ewe.items) {
            std::string key(reinterpret_cast<const char*>(it.first.data()), it.first.size());
            uint32_t coff = 0;
            auto mit = pool_dedup.find(key);
            if (mit != pool_dedup.end()) {
                coff = mit->second;
            } else {
                coff = static_cast<uint32_t>(core_nibbles.size());
                if (coff >= (1u << 20)) {
                    std::cerr << "ERROR: extra_core nibble pool exceeded 20-bit offset!\n";
                    return 2;
                }
                for (uint8_t d : it.first) core_nibbles.push_back(d & 0x0Fu);
                pool_dedup.emplace(std::move(key), coff);
            }
            cands_v5.push_back(pack_cand_v5(coff, it.second.first, 0, it.second.second));
        }
    }
    entry_off.push_back(static_cast<uint32_t>(cands_v5.size()));

    uint32_t run_pop = 0;
    for (uint32_t sb = 0; sb < super_blocks; ++sb) {
        supers[sb] = run_pop;
        uint32_t w0 = sb << 2;
        uint32_t w1 = std::min<uint32_t>(bitmap_words, w0 + 4u);
        for (uint32_t w = w0; w < w1; ++w) run_pop += portable_popcnt64(bits[w]);
    }
    if (run_pop != selected.size()) {
        std::cerr << "ERROR: popcount mismatch " << run_pop << " != " << selected.size() << "\n";
        return 3;
    }

    std::vector<uint8_t> core_pool((core_nibbles.size() + 1u) / 2u, 0);
    for (size_t i = 0; i < core_nibbles.size(); ++i) {
        if (i & 1u) core_pool[i >> 1] |= static_cast<uint8_t>((core_nibbles[i] & 0x0Fu) << 4);
        else core_pool[i >> 1] |= static_cast<uint8_t>(core_nibbles[i] & 0x0Fu);
    }

    auto align8 = [](size_t x) -> size_t { return (x + 7u) & ~7u; };
    ExtraCoreHeader hdr{};
    hdr.vocab_count = V;
    hdr.num_words_extra = static_cast<uint32_t>(selected.size());
    hdr.num_entries = static_cast<uint32_t>(cands_v5.size());
    hdr.bitmap_words = bitmap_words;
    hdr.super_blocks = super_blocks;
    hdr.core_pool_bytes = static_cast<uint32_t>(core_pool.size());

    size_t off_hdr = 0;
    size_t off_bits = align8(sizeof(ExtraCoreHeader));
    size_t off_sup = off_bits + bitmap_words * sizeof(uint64_t);
    size_t off_eo = off_sup + align8(super_blocks * sizeof(uint32_t));
    size_t off_cd = off_eo + align8(entry_off.size() * sizeof(uint32_t));
    size_t off_cp = off_cd + align8(cands_v5.size() * sizeof(WordCandidateV5));
    size_t total_sz = off_cp + core_pool.size();

    std::vector<uint8_t> out(total_sz, 0);
    std::memcpy(out.data() + off_hdr, &hdr, sizeof(hdr));
    std::memcpy(out.data() + off_bits, bits.data(), bitmap_words * sizeof(uint64_t));
    std::memcpy(out.data() + off_sup, supers.data(), super_blocks * sizeof(uint32_t));
    std::memcpy(out.data() + off_eo, entry_off.data(), entry_off.size() * sizeof(uint32_t));
    std::memcpy(out.data() + off_cd, cands_v5.data(), cands_v5.size() * sizeof(WordCandidateV5));
    std::memcpy(out.data() + off_cp, core_pool.data(), core_pool.size());

    // Self-verify ExtraCoreTableV8 reader
    ExtraCoreTableV8 tbl;
    if (!tbl.init(out.data(), static_cast<uint32_t>(out.size()))) {
        std::cerr << "ERROR: ExtraCoreTableV8 self-init failed!\n";
        return 4;
    }
    for (const auto& ewe : selected) {
        uint32_t rs = 0, re = 0;
        if (!tbl.get_range(ewe.wid, rs, re) || (re - rs) != ewe.items.size()) {
            std::cerr << "ERROR: ExtraCoreTableV8 self-verify mismatch on wid=" << ewe.wid << "\n";
            return 5;
        }
    }

    std::ofstream fo(out_path, std::ios::binary);
    fo.write(reinterpret_cast<const char*>(out.data()), out.size());
    std::cout << "Wrote verified ExtraCoreTableV8 to " << out_path << " (" << (out.size() / 1024.0)
              << " KB, nibbles=" << core_nibbles.size() << ")\n";
    return 0;
}
