# Tashkeel engine by Shamsuddin

A fast Arabic diacritizer in C++ (word lexicon, word bigrams and small decision trees), written by Shamsuddin and included in Fasih with his agreement, under the GNU GPL v3 or later.

- `cpp/`: the engine's source, as Shamsuddin sent it, with one change for Fasih: on Windows the model path is converted from UTF-8 to UTF-16 and opened with `CreateFileW`, so folders with non-ASCII names (an Arabic Windows user name) work.
- `python/`: his training pipeline.
- `bin/tashkeel_engine.dll`: 64-bit Windows build, cross-compiled with zig: `python3 -m ziglang c++ -target x86_64-windows-gnu -O3 -std=c++17 -shared -DTASHKEEL_EXPORTS -D_LIBCPP_ENABLE_CXX17_REMOVED_FEATURES -Icpp/include cpp/src/diacritizer.cpp cpp/src/c_api.cpp -o bin/tashkeel_engine.dll`
- `models/tashkeel_engine.tash`: the model Fasih ships, retrained with `python/train_pipeline.py --max-vocab 600000 --max-bigrams 1200000 --max-tree-lines 600000 --max-tree-samples 1000000 --oov-trees 40 --irab-trees 40 --morph-trees 32 --max-depth 7` on a balanced 8.9 million sentence mix: TigreGotico's Arabic diacritized text (twice), ArVoice's text, and 1.5 million sentences each from Shamela (ReligiousLLMs) and Abdou's Arabic tashkeel dataset (MIT), plus 1 million from Tashkeela, with every sentence of the test sets removed.
