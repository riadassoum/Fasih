# Build notes

How Fasih is put together, for anyone rebuilding or retraining it. The README explains what the add-on does; this file explains how its parts are made.

## Building the add-on

`addon_src/build_release.sh VERSION OUT.nvda-addon` builds the add-on from this repository alone: the voices in `voices/`, the languages in `lang/Arabic` and `lang_fr/French`, the engine DLLs in `third_party/rhvoice-win-x64`, Rawi, the driver patches and the documentation. Set `FASIH_SHARP=1` to keep Sharpening patches when the voices have them.

`tests/test_arabic.py` runs the text tests (Arabic pipeline, French front end, patches, limiter, downloader). The full Rawi tests also need `RAWI_ORT_LIB` (an ONNX Runtime library) and `FASIH_ADDON_DIR` (a built driver folder).

## Engine

RHVoice with two changes, in `engine/rhvoice-world-vocoder.patch` (against the commit in `engine/RHVOICE_BASE_COMMIT`):

- the WORLD vocoder (sources in `engine/world`), used by voices whose label format contains `WORLD`;
- the modulation spectrum postfilter: when a voice has `ms_gain.txt` next to its `voice.data`, every mel-cepstral trajectory of each generation window is filtered in the modulation-frequency domain with those gains. `tools/ms_estimate.py` measures them from a voice's recordings.

The Windows DLLs are cross-compiled with zig (clang for x86_64-windows-gnu); libc++ needs `-D_LIBCPP_ENABLE_CXX17_REMOVED_FEATURES -D_LIBCPP_ENABLE_CXX20_REMOVED_FEATURES`. A Linux build of the same tree is what the training tools load (`RHVOICE_LIB`, `RHVOICE_LABELS`).

## Languages

- `lang/`: the Arabic data-only language. `gen_foma.py` and `gen_rest.py` write the foma sources; `build_lang.sh` compiles them with foma and RHVoice's `att2bin` and `dtree2bin` (set `RH` to an RHVoice source checkout).
- `lang_fr/`: the French language. `gen_fr.py` turns the ipa-dict lexicon (fetched by `build_lang.sh` when missing) and the letter-to-sound rules into foma sources; `build_lang.sh` compiles them the same way.
- `scripts/ar_prep.py` and `scripts/fr_prep.py` prepare text identically in training and in the add-on (wasl marks for Arabic; liaison, numbers and abbreviations for French).

## Training a voice

`tools/new_voice.sh ROOT CORPUS` trains one voice. `CORPUS` holds `wav/` (24 kHz) and `text.tsv` (id, duration, text). Every stage is resumable; run the same command again after an interruption.

1. Labels (`make_ssml.py`, RHVoice's label maker), features (`feats.py`: WORLD F0, mel-cepstrum, aperiodicity; `feats2.py`: 40 coefficients).
2. Alignment (`align.py`), full-context labels (`full_labels.py`), statistics and decision trees (`train.py stats`, `train.py trees`), realignment, larger trees.
3. Duration cap, separate pause tree, cleaned pitch statistics, pitch trees, and a calibration of spectral detail against held-out recordings (`calib.sh`).
4. `train.py write OUT/voice.data` writes the voice. Options (environment variables) set the speaking rate, level, tone filter, the heavy lam of الله and other corrections; `addon_src/build_addon.sh` holds the values used for each voice.

For another language set `RHV_LANG`, `RHV_LANG_DIR`, `RHV_DATA`, and `RHV_F0_MIN`/`RHV_F0_MAX` for the voice's pitch range.

`tools/freeze_stats.py` keeps only the statistics a finished voice needs to be written again. `tools/hts_edit.py` and `tools/prebuilt.py` match a voice delivered already built (rate, level, tone) without its training data.
