# Barq — Arabic Diacritization Engine & Model Documentation

## 1. Overview

**Barq** is a self-contained, zero-dependency, CPU-only Arabic diacritization (Tashkeel) engine written in portable C++17. It combines a compact rank-select finite-state lexicon, Arabic morphological decomposition, high-order contextual statistical tables, five quantized gradient-boosted decision tree (GBDT) ensembles, a two-pass stacked Viterbi + syntactic clause-stack beam decoder, and deterministic Arabic grammatical post-processing laws.

Key properties of this release:
- **Single compact model file (`models/barq.tash`):** `17.01 MB` losslessly compressed (`29.23 MB` raw binary image, CRC-32 verified on load), achieving **4.571% DER**, **15.835% WER**, **9.470% no-case WER**, and **8.327% I'rab (case-ending) error** across 11,142 sentences (156,858 words) spanning five Modern Standard Arabic (MSA) and Classical Arabic benchmarks.
- **High CPU throughput with zero external dependencies:** `~66,000–68,300 words/sec` single-threaded on x86_64 (`~1.30 s` for all 11,142 sentences on 4 threads — `~0.117 s` per 1,000 sentences), with `~188 ms` compressed load time using a custom built-in RFC-1951 DEFLATE decompressor faster than system `zlib`.
- **Cross-platform portable SIMD:** Runtime CPUID/XGETBV AVX2 dispatch on x86_64 (safe on any x86_64 CPU, with automatic scalar fallback), native ARM64 NEON SIMD on Android `arm64-v8a`, and clean scalar execution on `armeabi-v7a`.
- **Pre-compiled libraries for Linux, Windows, and Android:** All named `barq`, ready to drop into desktop, server, Python, C/C++, NVDA/screen-reader, or Android apps.

---

## 2. Package Layout & Pre-Compiled Artifacts

```text
barq/
├── DOCUMENTATION.md                  # This technical guide
├── CMakeLists.txt                    # Cross-platform CMake build (Linux, Windows, Android)
├── Makefile                          # Linux Makefile build
├── models/
│   └── barq.tash                     # 17.01 MB losslessly compressed Barq model (CRC-32: 0x6c1ba85a)
├── linux/
│   ├── libbarq.so                    # 651 KB x86_64 shared library (runtime AVX2 + scalar fallback)
│   └── barq                          # 651 KB x86_64 command-line executable
├── windows/
│   ├── barq.dll                      # 934 KB x86_64 Windows DLL (UCRT only; no VC++ redist required)
│   ├── barq.lib                      # 4.4 KB Windows import library
│   └── barq.exe                      # 1.4 MB x86_64 Windows command-line executable
├── android/
│   ├── BarqEngine.java               # Primary Java/Android wrapper (package com.barq)
│   ├── TashkeelEngine.java           # Backward-compatible Java wrapper (package com.tashkeel)
│   ├── build_android.sh              # NDK build script
│   └── jniLibs/
│       ├── arm64-v8a/libbarq.so      # 671 KB ARM64 Android library (NEON SIMD, 16 KB page-aligned)
│       ├── armeabi-v7a/libbarq.so    # 424 KB ARMv7 Android library (16 KB page-aligned)
│       └── x86_64/libbarq.so         # 741 KB x86_64 Android library (runtime AVX2, 16 KB page-aligned)
├── cpp/
│   ├── include/tashkeel/             # C & C++17 engine headers (barq.h, tashkeel.h, diacritizer.hpp, ...)
│   └── src/                          # diacritizer.cpp, c_api.cpp, cli.cpp, jni_bridge.cpp
├── python/                           # Python bindings (barq_py.py, tashkeel_py.py) & training/packing tools
├── scripts/                          # Platform build scripts, corpus builders & benchmark evaluators
└── tools/                            # High-speed C++ training data & statistics extractors
```

### Binary Compatibility & Linking Notes
- **Linux (`linux/libbarq.so`):** Compiled with `-O3 -mpopcnt -fPIC -fvisibility=hidden`, dynamically linked only against standard libc/libstdc++. Exports both `barq_*` and ABI-compatible `tashkeel_*` C symbols.
- **Windows (`windows/barq.dll`, `barq.exe`):** Cross-compiled for `x86_64-windows-gnu` via Zig, depending solely on the Windows Universal C Runtime (built into Windows 10/11). Opens model files via UTF-8 to UTF-16 conversion with `CreateFileW`, ensuring full support for non-ASCII / Arabic file paths on Windows.
- **Android (`android/jniLibs/*/libbarq.so`):** Compiled with Android NDK (API 24+, `ANDROID_STL=c++_static`, `-Wl,-z,max-page-size=16384`). Each `.so` has **zero external C++ runtime dependency** (depends solely on `libm.so`, `libdl.so`, and `libc.so`), meets Android 15+ / Google Play 16 KB ELF page-alignment requirements, and exports 28 symbols covering `barq_*`, `tashkeel_*`, `Java_com_barq_BarqEngine_*`, and `Java_com_tashkeel_TashkeelEngine_*`.

---

## 3. Model & Engine Architecture (From Scratch)

### 3.1 Text Representation & Symbol Inventory
Barq operates directly on UTF-8 input and maps Arabic base letters into a compact **38-symbol integer alphabet** (`0..37`, including all hamza forms, alef variants, teh marbuta, and alef maksura) and **15 diacritic classes** (`0..14`):
- `0`: None
- `1..4`: Fatha, Damma, Kasra, Sukun
- `5..7`: Fathatan, Dammatan, Kasratan (Tanween)
- `8`: Shadda
- `9..14`: Shadda combined with Fatha, Damma, Kasra, Fathatan, Dammatan, Kasratan

Non-Arabic tokens, digits, punctuation, and whitespace are preserved verbatim while punctuation boundaries (`BK_HARD`, `BK_COMMA`, `BK_OPEN`, `BK_CLOSE`) and adjacent numeric classes are tracked as syntactic signals. An optional spelling-restoration mode (`set_restore_spelling(true)`, enabled by default) restores dropped/normalized hamzas and final alef-maksura/yeh orthographic forms from lexical evidence.

### 3.2 Binary Model Layout (`models/barq.tash`)
The model uses a **two-layer lossless compression architecture**:

1. **Layer 1 — Structural Binary Compaction (`29,227,712 bytes = 29.23 MB`):**
   - **Rank-Select Bitmapped FST (`2.38 MB`):** Maps `348,291` Arabic vocabulary words to dense word IDs across `473,627` trie nodes using 64-bit child bitmaps, 256-bit superblock rank tables, and compact suffix tails.
   - **Word Metadata & Candidates (`6.68 MB`):** `348,291` 8-byte word metadata entries (including 15-bit reachable ending bitmasks) and `778,532` 5-byte bit-packed candidates (`20-bit core offset`, `4-bit ending ID`, `4-bit flags`, `12-bit quantized log-probability`), backed by a `121 KB` 4-bit nibble-packed deduplicated internal-core diacritic pool.
   - **Block-Indexed CSR Bigrams (`6.02 MB`):** `899,891` `(prev_word, curr_word)` pairs stored in CSR rows with 32-entry block base offsets (`5 bytes/entry`) and `int8`-quantized ending bonuses masked by each word's reachable ending bitmask.
   - **Role-Ending Transition Matrix (`259 KB`):** `24 × 15 × 24 × 15 = 129,600` `int16` transition scores between syntactic roles and final case endings.
   - **Compact Multi-Order Directional N-grams (`3.34 MB`):** `279,998` 3-gram, 4-gram, 5-gram, right-bigram (`RBG`), centered-trigram (`CTR3`), and skip-1 bigram (`SKIP1`) entries stored in a 65,536-bucket top-16-bit prefix-indexed table (`11 bytes/entry`: 48-bit key suffix + 5-byte bit-packed best/alt endings, bonuses, and order code).
   - **Compact Directional Core-Bigrams (`1.69 MB`):** `101,919` left, right, and centered-trigram core-diacritic disambiguation entries (`14 bytes/entry`) targeting homographs that share consonants but differ in internal voweling (e.g., active vs. passive verbs, noun vs. verb readings).
   - **Sparse Rank-Indexed Valency Table (`898 KB`):** Bitmap + superblock rank-compressed table encoding grammatical valency (verb transitivity, syntactic type, subject/object case expectations) for lexicalized words.
   - **Third-Order Syntactic State Transition Cube (`216 KB`):** `60 × 60 × 60 = 216,000` `int8` trigram transition scores over `(12 syntactic roles × 5 vowel families)`.
   - **Extra-Core Lexical Expansion Table (`68 KB`):** Sparse rank-indexed table storing `2,155` additional rare/secondary valid internal vocalization cores missed by top-K lexical pruning, improving coverage on homographs and passive/derived stems.
   - **Five Compact 4-Byte/Node GBDT Ensembles (`2.79 MB` total):**
     - **OOV Ensemble (`649 KB`):** 1,350 trees, 15 classes, 160,918 nodes, max depth 8, 38 features (character n-grams, weak-letter/hamza codes, proclitic/enclitic structure, left-to-right auto-regressive diacritic family history).
     - **Morphology Ensemble (`95 KB`):** 384 trees, 12 morphological classes, 23,424 nodes, max depth 6, 32 features.
     - **Pass-1 I'rab Ensemble (`188 KB`):** 600 trees, 15 classes, 46,302 nodes, max depth 6, 56 contextual and grammatical features.
     - **Pass-2 Stacked Core Ranker (`Core2`, `401 KB`):** 399 trees, 2-class group-softmax candidate ranker, 99,845 nodes, max depth 9, 72 bidirectional Pass-1 + lexical + morphological + contextual features.
     - **Pass-2 Stacked I'rab Reranker (`Irab2`, `1.46 MB`):** 1,950 trees, 15 classes, 363,444 nodes, max depth 9, 96 bidirectional Pass-1 + agreement + syntactic-frame features.
   - **Tuned Global Scoring Weights:** 13 base weights, 12 contextual/Pass-2 weights, and 3 Core-reranker/extra-core weights stored directly in the binary header.

2. **Layer 2 — Lossless Container Compression (`17,009,332 bytes = 17.01 MB`, `-41.8%`):**
   - Wraps the raw Layer-1 image in a 16-byte `TSZH` header (`magic=0x48535A54`, raw RFC-1951 DEFLATE level 9 payload, exact uncompressed size, and IEEE CRC-32 checksum `0x6c1ba85a`).
   - Decompressed and CRC-verified in memory on `load()` by Barq's built-in `tiny_inflate.hpp` with zero external libraries.

### 3.3 End-to-End Inference Pipeline
For each input sentence/line, `Diacritizer::diacritize()` executes a deterministic two-pass pipeline:

1. **UTF-8 Tokenization & Surface Analysis:** Segments Arabic words, preserves non-Arabic spans, extracts surface morphology (`SurfaceInfo`: definite article `al-`, sun/moon letters, proclitics `wa-/fa-/bi-/li-/ka-`, enclitic pronouns, particle class, verb/noun cues, diptote and feminine-plural flags), and computes 24-bit cached extra surface flags (`ExtraSurfFlags`).
2. **Candidate Generation & Morphological Expansion:** Looks up exact words in `BitmappedFST` (with a 2-way × 8,192-set thread-local candidate cache), merges `ExtraCore` entries, falls back to relaxed hamza/alef-maksura lookup or proclitic+stem+enclitic morphological stripping, and invokes the 38-feature **OOV GBDT** for unseen words. Expands valid case endings according to grammatical declension rules.
3. **Pass-1 Unary Scoring:** Applies Arabic unary morphological rules, evaluates the sentence-batched **Morph GBDT** (12 classes), scores directional **Core-Bigrams** (left, right, centered-3), and evaluates the sentence-batched **Pass-1 I'rab GBDT** (15 classes, masked to reachable candidate endings).
4. **Pass-1 Viterbi DP:** Computes token-hoisted pairwise grammatical compatibility (`fill_pair_bin`), looks up CSR bigrams and directional 3/4/5-gram/RBG/CTR3/SKIP1 tables with chunked prefetching, and runs exact first-order Viterbi decoding across each punctuation-delimited clause segment.
5. **Pass-1 Syntactic Clause-Stack Beam Search:** Runs a 4-level syntactic frame-stack beam decoder (`SynState`: nominal/verbal/inna/kana/relative clause frames, prepositional phrase state, idafa construct chains, subject/object valency expectations, mood triggers, 3rd-order `trans3` state transitions) to refine case endings globally across each segment.
6. **Pass-2 Stacked Core Reranking (`Core2`):** Extracts 72 bidirectional features for ambiguous multi-core words using full Pass-1 left/right context, evaluates the 399-tree depth-9 **Core2** group-softmax ensemble in a single sentence batch, selects optimal internal vocalizations, and locks high-confidence cores.
7. **Pass-2 Stacked I'rab Reranking (`Irab2`) & Final Beam Search:** Lazily refreshes Pass-1 I'rab scores where core locks shifted candidates, extracts 96 bidirectional syntactic/agreement features, evaluates the 1,950-tree depth-9 **Irab2** ensemble in a sentence batch, and executes the final Pass-2 syntactic beam search.
8. **Deterministic Post-Processing Laws & UTF-8 Output:** Enforces invariant Arabic orthographic and phonological rules (sun-letter assimilation, hamzat al-wasl, iltiqa al-sakinayn, particle/title/compound-number invariants, spelling restoration) and emits UTF-8 text via table-driven encoding.

---

## 4. Complete History of Model & Accuracy Improvements

All accuracy numbers below are measured with `scripts/eval_unified.py` on the exact same **5-benchmark evaluation suite (11,142 sentences, 156,858 words)** spanning three Modern Standard Arabic benchmarks (`WikiNews-2014`, `WikiNews-2014-MR` multi-reference, `WikiNews-2024-MR` multi-reference) and two Classical/mixed benchmarks (`TigreVal-5k`, `TigreTest-5k`). Lower percentages indicate fewer errors.

### 4.1 Chronological Accuracy Progression (UNIFIED_ALL_5)

| Milestone / Modification | DER (%) | WER (%) | WER_nc (%) | I'rab Err (%) | MSA-3 DER | CLS-2 DER |
|---|---:|---:|---:|---:|---:|---:|
| 1. Initial baseline model (`20.65 MB` raw) | 5.723 | 19.237 | 10.976 | 11.123 | 6.101 | 5.571 |
| 2. + Contextual tables (directional N-grams, Valency, `trans3`, `CoreBg`) | 5.384 | 18.234 | 10.470 | 10.370 | 5.895 | 5.179 |
| 3. + Pass-2 Stacked I'rab Reranker (`Irab2`, 96 feats) + joint weight tuning + 2-layer lossless packing (`16.57 MB`) | 4.803 | 16.253 | 9.990 | 8.440 | 5.344 | 4.585 |
| 4. + Upgraded 38-feature OOV GBDT ensemble (`1,350` trees, depth 8) | 4.760 | 16.180 | 9.907 | 8.442 | 5.262 | 4.558 |
| 5. + Extra-Core lexical expansion (`2,155` cores) + expanded Core-Bigrams | 4.758 | 16.179 | 9.910 | 8.440 | 5.255 | 4.558 |
| 6. + Pass-2 Stacked Bidirectional Core Ranker (`Core2`, initial binary log-loss) | 4.605 | 15.898 | 9.564 | 8.339 | 5.052 | 4.425 |
| 7. + `Core2` custom Group-Softmax objective (72 features) | 4.599 | 15.890 | 9.523 | 8.362 | 5.104 | 4.395 |
| **8. Final Barq (`models/barq.tash`, balanced Group-Softmax `Core2` + lossless compression, `17.01 MB`)** | **4.571** | **15.835** | **9.470** | **8.327** | **4.988** | **4.403** |

**Total cumulative accuracy gain over initial baseline:**
- **DER:** `5.723% -> 4.571%` (**-1.152 pp**, **-20.1% relative error reduction**)
- **WER:** `19.237% -> 15.835%` (**-3.402 pp**, **-17.7% relative error reduction**)
- **WER (no case ending):** `10.976% -> 9.470%` (**-1.506 pp**, **-13.7% relative error reduction**)
- **I'rab (case-ending error):** `11.123% -> 8.327%` (**-2.796 pp**, **-25.1% relative error reduction**)

### 4.2 Final Barq Per-Benchmark Breakdown (`models/barq.tash`)

| Benchmark | Sentences | Words | DER (%) | WER (%) | WER_nc (%) | I'rab Err (%) |
|---|---:|---:|---:|---:|---:|---:|
| WikiNews-2014 | 393 | 16,206 | 3.873 | 14.661 | 5.905 | 10.397 |
| WikiNews-2014-MR (multi-ref) | 393 | 15,358 | 3.804 | 14.305 | 5.795 | 10.216 |
| WikiNews-2024-MR (multi-ref) | 356 | 8,921 | 8.948 | 37.754 | 32.418 | 9.158 |
| TigreVal-5k | 5,000 | 58,390 | 4.315 | 14.343 | 8.577 | 7.599 |
| TigreTest-5k | 5,000 | 57,983 | 4.492 | 14.697 | 8.809 | 7.854 |
| **UNIFIED_MSA_3** | **1,142** | **40,485** | **4.988** | **19.615** | **11.706** | **10.056** |
| **UNIFIED_CLS_2** | **10,000** | **116,373** | **4.403** | **14.520** | **8.693** | **7.726** |
| **UNIFIED_ALL_5** | **11,142** | **156,858** | **4.571** | **15.835** | **9.470** | **8.327** |

---

## 5. Complete Engineering Log of Speed & Systems Optimizations

Every engine optimization below was engineered under a strict **zero-accuracy-drift invariant**: output text is verified **100.000% bit-exact via SHA-256 checksums** across all 11,142 benchmark sentences against the unoptimized reference engine.

### 5.1 Phase 1 — Hot Primitives, Branchless Bit Operations, BFS Trees & Fast CRC/Huffman
- **Branchless Arabic Grammar & UTF-8 Tables (`arabic_grammar.hpp`, `arabic_utf8.hpp`):** Replaced hot `switch` statements in `is_sun_letter`, `diac_family`, `diac_is_tanween`, `make_diac`, `strip_tanween`, and `add_shadda` with constant lookup tables and bitmasks; replaced UTF-8 emission branches with direct table-driven `utf8_append` and `append_diacritic`.
- **FST & Hash Table Micro-Optimizations (`bit_fst.hpp`):** Simplified `rank1` to a single 64-bit load with hardware `POPCNT` (`-mpopcnt` on x86_64); added explicit software prefetching (`prefetch_row`, `prefetch_row_mid`) and `best_bonus_v5` to `BigramTable`; unrolled FNV-1a key hashing and added bucket prefetching to `NgramTableV6` and `CoreBgTableV7`.
- **Complete 1-Based BFS Trees & Portable SIMD (`tree_eval.hpp`):** Flattened 4-byte packed trees at model load time into 1-based complete-BFS layouts (`Bfs6One`, `Bfs7One`, `Bfs8One`) where early leaves are padded with dummy splits `{feat=0, thresh=127}` (since `int8_t x > 127` is always false, tree traversal is 100% branchless). Added portable inline `cpuid`/`xgetbv` runtime AVX2 detection (`__attribute__((target("avx2")))` — zero illegal instructions on non-AVX2 CPUs), ARM64 NEON SIMD, and 8-way unrolled scalar fallbacks.
- **Slicing-by-8 CRC-32 & 11-Bit Huffman LUT (`tiny_inflate.hpp`):** Upgraded IEEE CRC-32 from bit-by-bit to a 2 KB slicing-by-8 table (`~17.7 ms` over 29.2 MB) and replaced bit-by-bit DEFLATE decoding with an 11-bit primary Huffman lookup table + 64-bit branchless bit-buffer refill.

### 5.2 Phase 2 — Algorithmic Hoisting, Pool-Backed Caching & Sentence-Batched SIMD
- **24-Bit Cached Surface Flags & Pool-Backed Candidate Cache (`diacritizer.hpp`, `diacritizer.cpp`):** Consolidated 24 repeated token-level linguistic predicates (`XF_*` flags such as `XF_IS_KANA_VERB`, `XF_IS_TITLE`, `XF_IS_TIME_ADV`, `XF_AL_SUN`, `XF_IS_ALLAH`, `XF_NON_IDAFA_MABNI`, etc.) computed once and stored inside a 2-way × 8,192-set candidate cache backed by a flat `131,072`-entry `Cand` pool (eliminating repeated heap allocations and per-token string scans).
- **8-Byte Compact `CandMeta` & Hoisted Token/Pair Computations:** Packed per-candidate Viterbi/Beam metadata (`core_h`, `ce`, `cf`, `eff_fam`, `role`, `st3`, `bits`) into a single 8-byte `CandMeta` struct, cached per-token valency (`tok_val`), hoisted invariant token logic in `apply_unary_rules()`, `build_oov()`, and `fill_pair_bin()`, and reused precomputed `xflags` in Post-Laws.
- **Sentence-Batched SIMD GBDT Evaluation:** Refactored `Morph`, `Irab1`, lazy `Irab1`, `Core2`, and `Irab2` to extract compact 128-byte `int8` feature vectors for all eligible tokens in a sentence and evaluate trees in L1-cache-tiled sentence batches (`eval_class_batch_i8`, `predict_batch_binary_i8`), amortizing feature transposition and SIMD top-tree traversal across up to 32 tokens simultaneously.
- **Chunked Viterbi Prefetching & Beam Search Hoisting:** Added 16-token chunked prefetching for bigrams and multi-order N-grams ahead of the Viterbi DP loop; hoisted all token invariants (`SynTokCtx`) and option invariants (`SynOpt`) out of the inner hypothesis×option beam loop in `run_case_beam`.

### 5.3 Phase 3 — Cache-Line Split Depth-9 Trees, L2-Resident Transitions & Register-Local Inflate
Profiling after Phase 2 revealed that depth-9 tree evaluation (`Core2` + `Irab2`) accounted for **41–44%** of total runtime, while Viterbi DP + Beam Search accounted for **26%**, and DEFLATE decompression dominated compressed model load time. Phase 3 directly targeted every remaining bottleneck:

- **Branchless Split `Top5` + `Sub4` Depth-9 Tree Layout (`tree_eval.hpp`):**
  - Replaced monolithic `2,048 B` `Bfs9One` trees (which suffered from 5-cache-line Phase-2 scatter and 2048B power-of-2 L1d cache-set aliasing) with two contiguous 64-byte-aligned arrays: `Bfs9Top5` (`64 B` = 1 cache line for levels 0..4) and `Bfs9Sub4` (`64 B` = 1 cache line holding ALL 15 splits of levels 5..8 PLUS all 16 `int16` leaves for one subtree).
  - Phase 1 evaluates `Top5` using 32-lane AVX2 (`L5_32`, `cnt > 16`), 16-lane AVX2/SSE (`L5_16`, `6 <= cnt <= 16`), or 16-lane ARM64 NEON (`L5_16`), directly producing subtree index `s in 0..31`. Phase 2 accesses **exactly one 64-byte cache line** per tree with 8-way unrolled scalar ILP.
  - Upgraded the 16-lane path (`6 <= cnt <= 16`) for depth 6–8 trees (`Bfs6One..Bfs8One`, used by `Irab1`, `Morph`, and `OOV`) from Level-4 to Level-5 on both AVX2 and ARM64 NEON, cutting scalar Phase-2 depth on `Irab1`/`Morph` in half.
  - Micro-benchmark verified **1.15×–1.34× speedup across every batch size `B = 1..48`** on both `Core2` and `Irab2`, 100% bit-exact.
- **L2-Compact Transition Tables & Flipped Viterbi DP (`diacritizer.hpp`, `diacritizer.cpp`):**
  - Replaced the `1.05 MB` `std::vector<int32_t> trans3_scaled_` (4× larger than a 256 KB L2 cache) with a **1 KB `int32_t trans3_lut_[256]`** lookup table indexed by the raw `216 KB` `int8_t trans3_` cube — shrinking memory footprint by **4×** so every `(prev2, prev1)` row fits inside a single 64-byte cache line.
  - Shrunk `trans_scaled_` from `518 KB` (`int32_t`) to **`259 KB` (`int16_t trans_scaled_16_`)** with an automatic `int32_t` fallback if custom weights ever exceed `int16` range.
  - Flipped the Viterbi inner loop from `(c outer, p inner)` — which strided by 1,440 bytes across `trans_scaled_` and strided across `pbin` — to **`(p outer, c inner)`**, giving 100% sequential `pbin` streaming, a single hoisted `tr_row` pointer per `p`, and mathematically identical strict-`>` tie-breaking.
  - Hoisted `opt_tr_col[16]`, `tr_row`, and `tr3_row` per hypothesis in `run_case_beam`, and reordered state-deduplication comparisons to check `last_end` (highest entropy field) first.
- **Register-Local Fast DEFLATE Inflate & No-Zero Allocation (`tiny_inflate.hpp`, `diacritizer.cpp`):**
  - Added a dedicated register-local fast inflate loop (`src + 16 <= src_end` and `dst + 266 <= dst_end`) with 3-literal loop unrolling and wild 8-byte overlapping match copies, plus `std::unique_ptr<uint8_t[]>` avoiding 29 MB zero-fill before decompression.
  - Decompresses the full model in **`~146–150 ms`** — **1.58× faster than system C `zlib` (`230.8 ms`)** on the same CPU!
- **Shared-Library Visibility (`Makefile`, `CMakeLists.txt`):** Enabled `-fvisibility=hidden` across all builds so internal C++ calls inside `libbarq.so` bypass PLT indirection while keeping all public C and JNI APIs explicitly exported.

### 5.4 Evaluated & Rejected Experiments
Several candidate optimizations were benchmarked and intentionally rejected because they did not improve real-world performance:
- **Branched early-leaf `SplitBfs9`:** Adding an early-exit leaf bitmask check between `Top5` and `Sub4` regressed `Core2` speed to `0.71×` due to ~52% unpredictable branch mispredictions; branchless `Bfs9Sub4` won across every batch size.
- **Global suffix-max branch-and-bound pruning on `Irab1`:** Outlier leaf magnitudes prevented pruning on >95% of tokens while adding comparison overhead.
- **Link-Time Optimization (`-flto=auto`):** Slightly regressed single-thread throughput (`65.3k` vs `68.3k w/s`) due to different inlining heuristics around `target("avx2")` kernels.

### 5.5 Before / After Performance Summary
Measured on Intel Xeon Broadwell-EP @ 2.20 GHz (4 vCPU, Linux x86_64, GCC 13.3):

| Metric | Baseline (Before Optimization) | Final Barq (After Phases 1–3) | Speedup |
|---|---:|---:|---:|
| Single-thread CLI throughput (`wikinews_2014`, 54,900 words) | 31,160 words/sec (`1.762 s`) | **67,899–68,329 words/sec** (`0.803–0.809 s`) | **2.18×–2.19×** |
| Full 5-benchmark suite (11,142 sentences, 4 workers) | `2.221 s` | **`1.300 s`** | **1.71×** |
| Latency per 1,000 sentences (overall / TiGRE, 4 workers) | `0.199 s` | **`0.117 s`** overall / **`0.095 s`** TiGRE | **1.70×–2.09×** |
| Compressed model load + CRC-32 verify (`models/barq.tash`) | `~307 ms` | **`~175–188 ms`** | **1.63×–1.75×** |
| Output bit-exactness across all 11,142 benchmark sentences | Reference | **5 / 5 SHA-256 100.000% exact** | Zero drift |

---

## 6. Usage & Integration Guide

### 6.1 Command-Line Interface (Linux & Windows)
```bash
# Linux
./linux/barq models/barq.tash --text "العلم نور والجهل ظلام"
./linux/barq models/barq.tash --file input.txt --out output.txt
./linux/barq models/barq.tash --bench input.txt --iters 3

# Windows (PowerShell / CMD)
.\windows\barq.exe models\barq.tash --text "العلم نور والجهل ظلام"
```

### 6.2 Python Integration (`python/barq_py.py`)
```python
from barq_py import BarqEngine

with BarqEngine("models/barq.tash") as engine:
    print(engine.diacritize("ذهب الطالب إلى المدرسة"))
```
*(Backward-compatible `from tashkeel_py import TashkeelEngine` is also included and automatically loads `linux/libbarq.so` or `windows/barq.dll`.)*

### 6.3 C / C++ Integration (`cpp/include/tashkeel/barq.h`)
```c
#include "tashkeel/barq.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    void* eng = barq_create("models/barq.tash");
    const char* txt = "العلم نور والجهل ظلام";
    size_t out_len = 0;
    char* res = barq_diacritize(eng, txt, strlen(txt), &out_len);
    printf("%s\n", res);
    barq_free_str(res);
    barq_destroy(eng);
    return 0;
}
```
Thread safety: A single loaded engine handle (`void*` / `tashkeel::Diacritizer`) is **safe for concurrent read-only `diacritize()` calls across multiple threads** (each thread maintains its own `thread_local` scratch buffers and candidate cache).

### 6.4 Android Integration (`android/`)
1. Copy `android/jniLibs/{arm64-v8a,armeabi-v7a,x86_64}/libbarq.so` into your Android module's `app/src/main/jniLibs/`.
2. Copy `android/BarqEngine.java` (`package com.barq`) — or `android/TashkeelEngine.java` (`package com.tashkeel`) for legacy callers — into your app's Java/Kotlin source tree.
3. Bundle `models/barq.tash` in `assets/` and extract it to `context.filesDir` on first launch, then open it:
```java
import com.barq.BarqEngine;

try (BarqEngine barq = new BarqEngine(modelFile.getAbsolutePath())) {
    String diacritized = barq.diacritize("العلم نور والجهل ظلام");
}
```

### 6.5 Rebuilding From Source
- **Linux:** `bash scripts/build_linux.sh` (CMake) or `make` (Makefile) -> produces `linux/libbarq.so` and `linux/barq`.
- **Windows (from Linux/macOS via Zig):** `pip install ziglang && sh scripts/build_windows_zig.sh` -> produces `windows/barq.dll`, `windows/barq.lib`, `windows/barq.exe`.
- **Windows (native MSVC/MinGW):** `scripts\build_windows.bat`.
- **Android (all 3 ABIs):** `ANDROID_NDK_HOME=/path/to/android-ndk bash scripts/build_android.sh` -> produces `android/jniLibs/{arm64-v8a,armeabi-v7a,x86_64}/libbarq.so`.

### 6.6 Training & Model Packing Scripts
- `python/train_pipeline.py`: End-to-end base lexicon, FST, bigram, transition, OOV, I'rab, and Morph GBDT training.
- `tools/extract_stats.cpp` & `tools/extract_extra_cores.cpp`: Multi-threaded C++ extractors for directional N-grams, valency, `trans3`, `CoreBg`, and `ExtraCore`.
- `tools/dump_irab_pass2_data.cpp` & `tools/dump_core_pass2_data.cpp`: Zero-skew C++ Pass-2 feature dumpers.
- `python/train_oov.py`, `python/train_irab_pass2.py`, `python/train_core_pass2.py`: LightGBM trainers for the 38-feature OOV, 96-feature Pass-2 I'rab, and 72-feature Group-Softmax Pass-2 Core ensembles.
- `python/pack_lossless.py` & `python/build_model.py`: Lossless Layer-1 table compaction and Layer-2 `TSZH` DEFLATE compression into `barq.tash`.
- `python/tune_barq_weights.py` & `scripts/eval_unified.py`: Weight tuning and 5-benchmark evaluation.
