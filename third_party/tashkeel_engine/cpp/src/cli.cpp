#include "tashkeel/diacritizer.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " <model.tash> [--text \"...\"]\n"
                  << "  " << argv[0] << " <model.tash> --bench <file.txt> [--iters N] [--threads N]\n"
                  << "  " << argv[0] << " <model.tash> --file <in.txt> [--out <out.txt>] [--threads N]\n"
                  << "  " << argv[0] << " <model.tash> --dump-v9 <gold.txt> <out_dump.bin>\n"
                  << "  " << argv[0] << " <model.tash> --dump-core2 <gold.txt> <out_prefix> [--threads N]\n";
        return 1;
    }

    std::string model_path = argv[1];
    tashkeel::Diacritizer engine;
    std::string err;

    auto t0 = std::chrono::steady_clock::now();
    if (!engine.load(model_path, &err)) {
        std::cerr << "Error loading model: " << err << "\n";
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::string mode = "stdin";
    std::string text_arg;
    std::string file_arg;
    std::string out_arg;
    int iters = 10;
    int threads = 1;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--text" && i + 1 < argc) {
            mode = "text";
            text_arg = argv[++i];
        } else if (a == "--bench" && i + 1 < argc) {
            mode = "bench";
            file_arg = argv[++i];
        } else if (a == "--file" && i + 1 < argc) {
            mode = "file";
            file_arg = argv[++i];
        } else if ((a == "--dump-v9" || a == "--dump-v8") && i + 2 < argc) {
            mode = "dump_v9";
            file_arg = argv[++i];
            out_arg = argv[++i];
        } else if (a == "--dump-core2" && i + 2 < argc) {
            mode = "dump_core2";
            file_arg = argv[++i];
            out_arg = argv[++i];
        } else if (a == "--set-weight" && i + 2 < argc) {
            uint32_t wid = static_cast<uint32_t>(std::stoul(argv[++i]));
            int32_t wval = static_cast<int32_t>(std::stoi(argv[++i]));
            engine.set_weight(wid, wval);
        } else if (a == "--out" && i + 1 < argc) {
            out_arg = argv[++i];
        } else if (a == "--iters" && i + 1 < argc) {
            iters = std::stoi(argv[++i]);
        } else if (a == "--threads" && i + 1 < argc) {
            threads = std::stoi(argv[++i]);
        }
    }

    if (threads == 0) {
        threads = static_cast<int>(std::thread::hardware_concurrency());
        if (threads < 1) threads = 1;
    }

    if (mode == "dump_v9") {
        std::ifstream fin(file_arg);
        if (!fin) {
            std::cerr << "Cannot open gold file: " << file_arg << "\n";
            return 1;
        }
        std::ofstream fout(out_arg, std::ios::binary);
        if (!fout) {
            std::cerr << "Cannot create dump file: " << out_arg << "\n";
            return 1;
        }
        std::vector<tashkeel::Diacritizer::V9DumpToken> toks;
        std::string line;
        uint64_t num_sent = 0;
        uint64_t num_tok = 0;
        while (std::getline(fin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            toks.clear();
            engine.dump_v9_sentence(line.data(), line.size(), toks);
            if (toks.empty()) continue;
            uint32_t w = static_cast<uint32_t>(toks.size());
            fout.write(reinterpret_cast<const char*>(&w), sizeof(w));
            fout.write(reinterpret_cast<const char*>(toks.data()), static_cast<std::streamsize>(w * sizeof(tashkeel::Diacritizer::V9DumpToken)));
            ++num_sent;
            num_tok += w;
        }
        std::cout << "Dumped " << num_sent << " sentences (" << num_tok << " tokens) to " << out_arg << "\n";
        return 0;
    }

    if (mode == "dump_core2") {
        std::ifstream fin(file_arg);
        if (!fin) {
            std::cerr << "Cannot open gold file: " << file_arg << "\n";
            return 1;
        }
        std::ofstream fx(out_arg + "_X.bin", std::ios::binary);
        std::ofstream fy(out_arg + "_y.bin", std::ios::binary);
        std::ofstream fp1(out_arg + "_p1.bin", std::ios::binary);
        std::ofstream fgid(out_arg + "_gid.bin", std::ios::binary);
        if (!fx || !fy || !fp1 || !fgid) {
            std::cerr << "Cannot create dump files with prefix: " << out_arg << "\n";
            return 1;
        }
        const int nthreads = std::max(1, threads);
        constexpr size_t BATCH_LINES = 16384;
        std::vector<std::string> batch;
        batch.reserve(BATCH_LINES);
        uint64_t total_sents = 0;
        uint64_t total_rows = 0;
        uint32_t global_gid_base = 0;

        auto process_batch = [&]() {
            if (batch.empty()) return;
            const int nw = std::min<int>(nthreads, static_cast<int>(batch.size()));
            struct WorkerOut {
                std::vector<tashkeel::Diacritizer::Core2DumpSample> samples;
                uint32_t local_gids = 0;
            };
            std::vector<WorkerOut> wouts(static_cast<size_t>(nw));
            std::vector<std::thread> workers;
            workers.reserve(static_cast<size_t>(nw));
            const size_t per = (batch.size() + static_cast<size_t>(nw) - 1) / static_cast<size_t>(nw);
            for (int wi = 0; wi < nw; ++wi) {
                const size_t s_idx = static_cast<size_t>(wi) * per;
                const size_t e_idx = std::min<size_t>(batch.size(), s_idx + per);
                workers.emplace_back([&, wi, s_idx, e_idx]() {
                    WorkerOut& wo = wouts[static_cast<size_t>(wi)];
                    uint32_t ngid = 0;
                    for (size_t li = s_idx; li < e_idx; ++li) {
                        engine.dump_core2_sentence(batch[li].data(), batch[li].size(), wo.samples, ngid);
                    }
                    wo.local_gids = ngid;
                });
            }
            for (auto& th : workers) th.join();

            std::vector<int8_t> x_buf;
            std::vector<uint8_t> y_buf, p1_buf;
            std::vector<uint32_t> gid_buf;
            for (int wi = 0; wi < nw; ++wi) {
                const WorkerOut& wo = wouts[static_cast<size_t>(wi)];
                const size_t nr = wo.samples.size();
                if (nr > 0) {
                    x_buf.resize(nr * tashkeel::CORE2_NUM_FEATURES);
                    y_buf.resize(nr);
                    p1_buf.resize(nr);
                    gid_buf.resize(nr);
                    for (size_t r = 0; r < nr; ++r) {
                        const auto& smp = wo.samples[r];
                        for (size_t f = 0; f < tashkeel::CORE2_NUM_FEATURES; ++f) {
                            x_buf[r * tashkeel::CORE2_NUM_FEATURES + f] = static_cast<int8_t>(smp.feat[f]);
                        }
                        y_buf[r] = smp.label;
                        p1_buf[r] = smp.p1_correct;
                        gid_buf[r] = global_gid_base + smp.group_id;
                    }
                    fx.write(reinterpret_cast<const char*>(x_buf.data()), static_cast<std::streamsize>(x_buf.size()));
                    fy.write(reinterpret_cast<const char*>(y_buf.data()), static_cast<std::streamsize>(y_buf.size()));
                    fp1.write(reinterpret_cast<const char*>(p1_buf.data()), static_cast<std::streamsize>(p1_buf.size()));
                    fgid.write(reinterpret_cast<const char*>(gid_buf.data()), static_cast<std::streamsize>(gid_buf.size() * sizeof(uint32_t)));
                    total_rows += nr;
                }
                global_gid_base += wo.local_gids;
            }
            total_sents += batch.size();
            batch.clear();
        };

        std::string line;
        while (std::getline(fin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            batch.push_back(std::move(line));
            if (batch.size() >= BATCH_LINES) process_batch();
        }
        process_batch();
        std::cout << "Dumped Core2 (F=" << tashkeel::CORE2_NUM_FEATURES << ") "
                  << total_sents << " sentences, " << global_gid_base << " groups, "
                  << total_rows << " rows to prefix " << out_arg << "\n";
        return 0;
    }

    if (mode == "text") {
        std::cout << engine.diacritize(text_arg) << "\n";
        return 0;
    }

    if (mode == "bench") {
        std::ifstream fin(file_arg);
        if (!fin) {
            std::cerr << "Cannot open benchmark file: " << file_arg << "\n";
            return 1;
        }
        std::vector<std::string> lines;
        std::string line;
        size_t total_bytes = 0;
        size_t total_words = 0;
        while (std::getline(fin, line)) {
            if (line.empty()) continue;
            total_bytes += line.size();
            std::istringstream iss(line);
            std::string w;
            while (iss >> w) ++total_words;
            lines.push_back(std::move(line));
        }

        if (threads <= 1) {
            for (const auto& l : lines) {
                volatile size_t sz = engine.diacritize(l).size();
                (void)sz;
            }
            auto b0 = std::chrono::steady_clock::now();
            for (int it = 0; it < iters; ++it) {
                for (const auto& l : lines) {
                    volatile size_t sz = engine.diacritize(l).size();
                    (void)sz;
                }
            }
            auto b1 = std::chrono::steady_clock::now();
            double sec = std::chrono::duration<double>(b1 - b0).count();
            double words_per_sec = (static_cast<double>(total_words) * iters) / sec;
            double mb_per_sec = (static_cast<double>(total_bytes) * iters) / (1024.0 * 1024.0 * sec);
            std::cout << "Model load: " << load_ms << " ms | Vocab: " << engine.vocab_size()
                      << " | FST nodes: " << engine.fst_nodes_count()
                      << " | OOV trees: " << (engine.has_oov_trees() ? "yes" : "no")
                      << " | Irab trees: " << (engine.has_irab_trees() ? "yes" : "no")
                      << " | Threads: 1\n"
                      << "Processed " << (total_words * iters) << " words in " << sec << " s\n"
                      << "Speed: " << static_cast<uint64_t>(words_per_sec) << " words/sec ("
                      << mb_per_sec << " MB/sec)\n";
        } else {
            std::vector<std::string> out_lines(lines.size());
            engine.diacritize_lines(lines.data(), out_lines.data(), lines.size(), threads);
            auto b0 = std::chrono::steady_clock::now();
            for (int it = 0; it < iters; ++it) {
                engine.diacritize_lines(lines.data(), out_lines.data(), lines.size(), threads);
                volatile size_t sz = out_lines.empty() ? 0 : out_lines.back().size();
                (void)sz;
            }
            auto b1 = std::chrono::steady_clock::now();
            double sec = std::chrono::duration<double>(b1 - b0).count();
            double words_per_sec = (static_cast<double>(total_words) * iters) / sec;
            double mb_per_sec = (static_cast<double>(total_bytes) * iters) / (1024.0 * 1024.0 * sec);
            std::cout << "Model load: " << load_ms << " ms | Vocab: " << engine.vocab_size()
                      << " | FST nodes: " << engine.fst_nodes_count()
                      << " | OOV trees: " << (engine.has_oov_trees() ? "yes" : "no")
                      << " | Irab trees: " << (engine.has_irab_trees() ? "yes" : "no")
                      << " | Threads: " << threads << "\n"
                      << "Processed " << (total_words * iters) << " words in " << sec << " s\n"
                      << "Speed: " << static_cast<uint64_t>(words_per_sec) << " words/sec ("
                      << mb_per_sec << " MB/sec)\n";
        }
        return 0;
    }

    if (mode == "file") {
        std::ios::sync_with_stdio(false);
        std::vector<char> in_io_buf(256 * 1024);
        std::vector<char> out_io_buf(256 * 1024);
        std::ifstream fin;
        fin.rdbuf()->pubsetbuf(in_io_buf.data(), static_cast<std::streamsize>(in_io_buf.size()));
        fin.open(file_arg);
        if (!fin) {
            std::cerr << "Cannot open input file: " << file_arg << "\n";
            return 1;
        }
        std::ostream* os = &std::cout;
        std::ofstream fout;
        if (!out_arg.empty()) {
            fout.rdbuf()->pubsetbuf(out_io_buf.data(), static_cast<std::streamsize>(out_io_buf.size()));
            fout.open(out_arg);
            if (!fout) {
                std::cerr << "Cannot open output file: " << out_arg << "\n";
                return 1;
            }
            os = &fout;
        }
        if (threads <= 1) {
            std::string line;
            while (std::getline(fin, line)) {
                *os << engine.diacritize(line) << '\n';
            }
        } else {
            constexpr size_t BATCH_LINES = 4096;
            std::vector<std::string> in_batch;
            std::vector<std::string> out_batch;
            in_batch.reserve(BATCH_LINES);
            std::string line;
            auto flush_batch = [&]() {
                if (in_batch.empty()) return;
                out_batch.resize(in_batch.size());
                engine.diacritize_lines(in_batch.data(), out_batch.data(), in_batch.size(), threads);
                for (const auto& ol : out_batch) *os << ol << '\n';
                in_batch.clear();
            };
            while (std::getline(fin, line)) {
                in_batch.push_back(std::move(line));
                if (in_batch.size() >= BATCH_LINES) flush_batch();
            }
            flush_batch();
        }
        return 0;
    }

    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::cout << engine.diacritize(line) << '\n';
    }
    return 0;
}
