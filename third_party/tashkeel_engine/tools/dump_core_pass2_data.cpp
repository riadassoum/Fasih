#include "tashkeel/diacritizer.hpp"

#include <algorithm>
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

    std::vector<std::string> lines;
    lines.reserve(std::min<size_t>(max_lines, 500000));
    {
        std::ifstream f(corpus_path);
        std::string line;
        std::mt19937_64 rng(20261123ULL);
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            if (sample_rate < 0.999999 && uni(rng) > sample_rate) continue;
            lines.push_back(std::move(line));
            if (lines.size() >= max_lines) break;
        }
    }
    std::cout << "Loaded " << lines.size() << " lines for Core2 feature dump (threads=" << n_threads << ")...\n";

    std::vector<std::vector<Diacritizer::Core2DumpSample>> tl_out(n_threads);
    auto worker = [&](int tid) {
        size_t s = (lines.size() * tid) / n_threads;
        size_t e = (lines.size() * (tid + 1)) / n_threads;
        auto& out = tl_out[tid];
        out.reserve((e - s) * 8);
        uint32_t local_gid = 0;
        for (size_t i = s; i < e; ++i) {
            eng.dump_core2_sentence(lines[i].data(), lines[i].size(), out, local_gid);
        }
    };
    std::vector<std::thread> ths;
    for (int t = 0; t < n_threads; ++t) ths.emplace_back(worker, t);
    for (auto& th : ths) th.join();

    std::ofstream fx(out_prefix + "_X.bin", std::ios::binary);
    std::ofstream fy(out_prefix + "_y.bin", std::ios::binary);
    std::ofstream fp(out_prefix + "_p1.bin", std::ios::binary);
    std::ofstream fg(out_prefix + "_gid.bin", std::ios::binary);

    std::vector<int8_t> xbuf;
    std::vector<uint8_t> ybuf, pbuf;
    std::vector<uint32_t> gbuf;
    xbuf.reserve(65536 * CORE2_NUM_FEATURES);
    ybuf.reserve(65536);
    pbuf.reserve(65536);
    gbuf.reserve(65536);

    auto flush_bufs = [&]() {
        if (ybuf.empty()) return;
        fx.write(reinterpret_cast<const char*>(xbuf.data()), xbuf.size());
        fy.write(reinterpret_cast<const char*>(ybuf.data()), ybuf.size());
        fp.write(reinterpret_cast<const char*>(pbuf.data()), pbuf.size());
        fg.write(reinterpret_cast<const char*>(gbuf.data()), gbuf.size() * sizeof(uint32_t));
        xbuf.clear();
        ybuf.clear();
        pbuf.clear();
        gbuf.clear();
    };

    uint64_t total_rows = 0, total_groups = 0, p1_err_groups = 0;
    uint32_t global_gid_offset = 0;
    for (int t = 0; t < n_threads; ++t) {
        uint32_t last_local_gid = UINT32_MAX;
        uint32_t max_local_gid = 0;
        for (const auto& smp : tl_out[t]) {
            uint32_t ggid = global_gid_offset + smp.group_id;
            if (smp.group_id != last_local_gid) {
                last_local_gid = smp.group_id;
                ++total_groups;
                if (!smp.p1_correct) ++p1_err_groups;
            }
            if (smp.group_id + 1u > max_local_gid) max_local_gid = smp.group_id + 1u;
            ++total_rows;
            for (size_t k = 0; k < CORE2_NUM_FEATURES; ++k) {
                int32_t v = smp.feat[k];
                xbuf.push_back(static_cast<int8_t>(v < -8 ? -8 : (v > 119 ? 119 : v)));
            }
            ybuf.push_back(smp.label);
            pbuf.push_back(smp.p1_correct);
            gbuf.push_back(ggid);
            if (ybuf.size() >= 65536) flush_bufs();
        }
        global_gid_offset += max_local_gid;
        tl_out[t].clear();
        tl_out[t].shrink_to_fit();
    }
    flush_bufs();
    std::cout << "Dumped " << total_rows << " rows across " << total_groups
              << " multi-core token groups (P1 core err="
              << (100.0 * p1_err_groups / std::max<uint64_t>(1, total_groups))
              << "%) to " << out_prefix << "_*.bin\n";
    return 0;
}
