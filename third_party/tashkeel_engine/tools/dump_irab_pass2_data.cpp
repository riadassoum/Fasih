#include "tashkeel/diacritizer.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace tashkeel;

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <model.tash> <input_corpus.txt> <out_prefix> <max_lines> [threads=4] [sample_rate=1.0]\n";
        return 1;
    }
    std::string model_path = argv[1];
    std::string corpus_path = argv[2];
    std::string out_prefix = argv[3];
    size_t max_lines = static_cast<size_t>(std::strtoull(argv[4], nullptr, 10));
    int n_threads = (argc >= 6) ? std::max(1, std::atoi(argv[5])) : 4;
    double sample_rate = (argc >= 7) ? std::atof(argv[6]) : 1.0;

    Diacritizer eng;
    std::string err;
    if (!eng.load(model_path, &err)) {
        std::cerr << "Failed to load model: " << err << "\n";
        return 1;
    }

    // Read selected lines cleanly (uniform sampling when sample_rate < 1.0)
    std::vector<std::string> lines;
    lines.reserve(std::min<size_t>(max_lines, 500000));
    {
        std::ifstream f(corpus_path);
        std::string line;
        std::mt19937_64 rng(20261122ULL);
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            if (sample_rate < 0.999999 && uni(rng) > sample_rate) continue;
            lines.push_back(std::move(line));
            if (lines.size() >= max_lines) break;
        }
    }
    std::cout << "Loaded " << lines.size() << " lines for Irab2 feature dump (threads=" << n_threads << ")...\n";

    std::vector<std::vector<Diacritizer::Irab2DumpSample>> tl_out(n_threads);
    auto worker = [&](int tid) {
        size_t s = (lines.size() * tid) / n_threads;
        size_t e = (lines.size() * (tid + 1)) / n_threads;
        auto& out = tl_out[tid];
        out.reserve((e - s) * 8);
        for (size_t i = s; i < e; ++i) {
            eng.dump_irab2_sentence(lines[i].data(), lines[i].size(), out);
        }
    };
    std::vector<std::thread> ths;
    for (int t = 0; t < n_threads; ++t) ths.emplace_back(worker, t);
    for (auto& th : ths) th.join();

    // Keep multi-ending tokens only (matching inference where irab2_trees_ runs on multi-ending tokens!)
    uint64_t total_tok = 0, kept_tok = 0, p1_err = 0;
    std::ofstream fx(out_prefix + "_X.bin", std::ios::binary);
    std::ofstream fy(out_prefix + "_y.bin", std::ios::binary);
    std::ofstream fp(out_prefix + "_p1.bin", std::ios::binary);
    std::vector<int8_t> xbuf;
    std::vector<uint8_t> ybuf, pbuf;
    xbuf.reserve(65536 * IRAB2_NUM_FEATURES);
    ybuf.reserve(65536);
    pbuf.reserve(65536);

    auto flush_bufs = [&]() {
        if (ybuf.empty()) return;
        fx.write(reinterpret_cast<const char*>(xbuf.data()), xbuf.size());
        fy.write(reinterpret_cast<const char*>(ybuf.data()), ybuf.size());
        fp.write(reinterpret_cast<const char*>(pbuf.data()), pbuf.size());
        xbuf.clear();
        ybuf.clear();
        pbuf.clear();
    };

    for (int t = 0; t < n_threads; ++t) {
        for (const auto& smp : tl_out[t]) {
            ++total_tok;
            if (!smp.multi_end || smp.gold_end >= NUM_DIACRITICS) continue;
            ++kept_tok;
            if (smp.gold_end != smp.p1_end) ++p1_err;
            for (size_t k = 0; k < IRAB2_NUM_FEATURES; ++k) {
                int32_t v = smp.feat[k];
                xbuf.push_back(static_cast<int8_t>(v < -8 ? -8 : (v > 119 ? 119 : v)));
            }
            ybuf.push_back(smp.gold_end);
            pbuf.push_back(smp.p1_end);
            if (ybuf.size() >= 65536) flush_bufs();
        }
        tl_out[t].clear();
        tl_out[t].shrink_to_fit();
    }
    flush_bufs();
    std::cout << "Dumped " << kept_tok << " / " << total_tok << " multi-end tokens (P1 Irab err on dumped="
              << (100.0 * p1_err / std::max<uint64_t>(1, kept_tok)) << "%) to " << out_prefix << "_*.bin\n";
    return 0;
}
