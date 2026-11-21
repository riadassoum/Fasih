#include "tashkeel/diacritizer.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " <model.tash> [--text \"...\"]\n"
                  << "  " << argv[0] << " <model.tash> --bench <file.txt> [--iters N]\n"
                  << "  " << argv[0] << " <model.tash> --file <in.txt> [--out <out.txt>]\n";
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
        } else if (a == "--out" && i + 1 < argc) {
            out_arg = argv[++i];
        } else if (a == "--iters" && i + 1 < argc) {
            iters = std::stoi(argv[++i]);
        }
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

        // Warmup
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
                  << " | Irab trees: " << (engine.has_irab_trees() ? "yes" : "no") << "\n"
                  << "Processed " << (total_words * iters) << " words in " << sec << " s\n"
                  << "Speed: " << static_cast<uint64_t>(words_per_sec) << " words/sec ("
                  << mb_per_sec << " MB/sec)\n";
        return 0;
    }

    if (mode == "file") {
        std::ifstream fin(file_arg);
        if (!fin) {
            std::cerr << "Cannot open input file: " << file_arg << "\n";
            return 1;
        }
        std::ostream* os = &std::cout;
        std::ofstream fout;
        if (!out_arg.empty()) {
            fout.open(out_arg);
            if (!fout) {
                std::cerr << "Cannot open output file: " << out_arg << "\n";
                return 1;
            }
            os = &fout;
        }
        std::string line;
        while (std::getline(fin, line)) {
            *os << engine.diacritize(line) << "\n";
        }
        return 0;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        std::cout << engine.diacritize(line) << "\n";
    }
    return 0;
}
