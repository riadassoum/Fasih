# Barq (برق): PlanetBlindTech's tashkeel engine

A fast Arabic diacritizer in C++ (word lexicon, word n-grams, grammar rules and small decision trees), written by Shamseddin of PlanetBlindTech and included in Fasih with his agreement, under the GNU GPL v3 or later. Riad Assoum fine-tuned earlier versions of its model for Fasih.

- `cpp/`, `CMakeLists.txt`, `Makefile`, `scripts/`, `android/`: the engine's source and build scripts, as Shamseddin released them (Barq 2, November 2026).
- `bin/tashkeel_engine.dll`: 64-bit Windows build of that source, made for Fasih with zig: `python3 -m ziglang c++ -target x86_64-windows-gnu -O3 -std=c++17 -shared -DTASHKEEL_EXPORTS -D_LIBCPP_ENABLE_CXX17_REMOVED_FEATURES -Icpp/include cpp/src/diacritizer.cpp cpp/src/c_api.cpp -o bin/tashkeel_engine.dll` (also `scripts/build_windows_zig.sh`). It needs only the C runtime that ships with Windows 10 and 11.
- `models/tashkeel_engine.tash`: Shamseddin's Barq 2 model.
- `python/`: the training pipeline (model format v6, with 3-, 4- and 5-word context) and tools, plus `build_fasih_corpus.py`, which rebuilds the corpus Fasih 1.3's model was trained on from public datasets.
