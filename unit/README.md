# Fasih Unit: concatenative (unit-selection) voices

Fasih's premium voices: a concatenative speech synthesizer of the same family as Vocalizer and Acapela, built into Fasih's RHVoice engine. A unit voice speaks with pieces of real recordings: every phone of the corpus is cut into two half-phones, and for each sentence a Viterbi search chooses the sequence of half-phones that best fits the context (target cost) and joins most smoothly (join cost). Pieces that were next to each other in the recordings join for free, so whole syllables and words often come from one recording.

Everything in front of the voice is Fasih's: the Arabic text processing (Rawi or Barq diacritization, numbers, abbreviations, punctuation, the pronunciation dictionary, wasl rules), RHVoice's language switching, and the NVDA driver with its rate, rate boost, pitch and volume. A unit voice is an ordinary RHVoice voice folder, so it can be the primary voice, a secondary voice, or both (an Arabic unit voice with an English unit voice).

## How a sentence is spoken

1. RHVoice turns the text into full-context phone labels, exactly as for Fasih's other voices.
2. `select_units` (in `src/us_core.cpp`) finds candidates for every half-phone: units of the same phone whose neighbouring phone matches, scored on the context (neighbours two phones away, stress, position in syllable, word and phrase, phrase-final position, question or exclamation), plus the natural continuations of the previous step's candidates. A Viterbi search then minimises target cost plus join cost (spectral distance, pitch step, voicing change and energy at the join).
   With `models.bin` the search is hybrid, like the HMM-guided unit selection of the best non-neural systems (USTC's Blizzard Challenge entries from 2007, MaryTTS): context-clustering regression trees trained on the voice's own units predict every half-phone's duration, pitch and spectrum, and candidates are scored against those predictions. Every unit also carries a quality cost (how far it is from what its own context predicts, plus an acoustic check that a vowel really sounds like its label), so theatrical lengthening, misreadings and alignment errors are avoided. When a unit's recorded neighbour differs from the target's, the cost depends on how differently that neighbour colours the sound (`phonesim.txt`), with emphasis spreading from ص ض ط ظ weighed most. Joins at phone boundaries cost much more than joins inside a phone, so nearly every transition between two sounds is one real recorded transition, as in diphone synthesis. Candidates are pre-selected on context (from contiguous per-pool context tables) before the acoustic scoring, and the Viterbi search prunes exactly, which keeps a five-second sentence under about 100 ms.
3. `concatenate` joins the chosen pieces. At each join it searches ±5 ms for the best waveform match and cross-fades over 10 ms. Where the pitch on the two sides differs, both sides are bent towards each other over 45 ms with TD-PSOLA, using the pitch marks stored with the voice.
4. Pauses cut the search, and a long phrase is also cut where the silent closure of an unvoiced stop (ت ك ق ط ء) begins: a short first piece and longer ones after it, each chosen with a look-ahead and continuing from the unit spoken last. Speech starts within about 30 ms, like Fasih's compact voices. Inside a stop (closure then burst) the engine does not join two recordings, so a stop is never doubled or lost; joining where a stop's closure begins is as cheap as a join inside a vowel. Rate and pitch changes are pitch-synchronous (sonic), and rate boost works as in Fasih.

## Neural smoothing

A voice whose `weights.txt` says `vocoder 1` is, when the vocoder is installed, resynthesized through Vocos (a fast neural vocoder, MIT licence, 24 kHz): for every streamed piece the engine computes the mel spectrum of the concatenated speech, blends it across each join, runs the model through ONNX Runtime and rebuilds the waveform (`src/us_vocoder.cpp`). The sound still comes from the recordings; the vocoder smooths the joins. It runs at about 13 times real time inside NVDA, and the first audio arrives in about 50 ms. In Fasih the vocoder lives in `synthDrivers/fasih/vocoder` (`vocos_core.onnx`, `melfb.f32`, and ONNX Runtime under the private name `fasih_ort.dll`, so it can never clash with another add-on's copy); the setting "Neural smoothing for premium voices" switches it off by creating `vocoder/off`. For tests outside NVDA, set `FASIH_VOCODER_DIR` and `FASIH_ORT_LIB`, or `FASIH_VOCODER=0` to switch it off. `vocoder/export.py` makes the model files from the published Vocos model.

## Spectrum voices

`build_mel.py` stores every recording as the compact mel spectrum the vocoder takes instead of audio: 100 bands at 94 frames a second, kept as 60 DCT coefficients, quantized, delta-coded, varint-packed and deflated per recording (about 22 kbps against 56 for the Opus audio). Measured against the original recordings this is closer than Opus followed by the vocoder (spectral distortion 1.8 dB against 2.5 dB, and less loss above 8 kHz), and a voice pack is less than half the size (Zayd 175 MB against 364 MB, Haytham 96 MB against 207 MB). Premium voices are spectrum voices; `train_voice.sh` makes one unless `AUDIO_VOICE=1` asks for an audio-based voice. A voice with `mel.bin` and no audio is assembled directly in the spectral domain (`concatenate_mel`: the units' frames, the recording's own onset and release at phrase edges, spectral blending across joins) and always speaks through the vocoder, which then has to be installed.

The vocoder (the Vocos model, its mel filterbank and ONNX Runtime as `fasih_ort.dll`) is its own add-on, `fasihVocoder`, built by `tools/make_packs.py` as `Fasih-vocoder.zip` and listed in `voices.json`; the download window installs it together with the first premium voice. The engine looks for it in `addons/fasihVocoder/vocoder`, then in a `vocoder` folder inside Fasih.

## A voice folder

| file | contents |
|---|---|
| `voice.info` | name, language, gender (`format=3`) |
| `units.bin` | the half-phone units: phone, recording, position, context, join features |
| `audio.opa` | the recordings, Opus 56 kbps, one independent stream per recording with a frame table for random access |
| `marks.bin` | pitch marks (epochs) of every recording |
| `layout.txt`, `context.txt`, `phones.txt` | how to read RHVoice's labels, and the context vocabulary |
| `models.bin` | duration, pitch and spectrum trees for the hybrid target cost |
| `phonesim.txt` | phonetic substitution costs between context phones |
| `mel.bin` | spectrum voices: the recordings as compressed mel spectra (instead of `audio.opa`) |
| `weights.txt` | optional cost weights (`join_f0 12`, `max_cands 100`, any context feature by name) |

`audio.pcm` (16-bit, 24 kHz) is the uncompressed audio. The engine uses it when `audio.opa` is absent; it is only needed to re-encode.

## Training a voice

A corpus is `metadata.csv` (`id|text` per line) plus `wavs/ID.wav`, from one speaker, recorded consistently. Five hours is a minimum and ten or more is better. Arabic text must be fully diacritized; for a corpus without tashkeel, run it through Fasih's text pipeline (Rawi) first.

```
OPUS_SRC=~/opus unit/build_engine.sh ~/rhv linux                  # RHVoice + backend, label maker, tools
RHV_BUILD=~/rhv/b unit/train_voice.sh NAME CORPUS WORKDIR Arabic male
```

`train_voice.sh` resamples to 24 kHz, makes the labels with Fasih's Arabic language, extracts pitch and spectrum, aligns the phones with Fasih's own HMM aligner (`scripts/align.py`), builds the unit database, the pitch marks and the Opus audio, and writes the voice to `WORKDIR/voice/NAME`. Every stage is resumable. Set `RHV_F0_MIN` and `RHV_F0_MAX` for the speaker's pitch range (defaults 60–400 Hz).

### English (or French) voices

Use the same script with `English` (RHVoice's English language, as for Evan and Bdl) or `French` (Fasih's French language):

```
RHV_BUILD=~/rhv/b RHV_F0_MIN=60 RHV_F0_MAX=300 unit/train_voice.sh NAME CORPUS WORKDIR English male
```

The context features are read from each language's `labelling.xml`, so nothing else changes. With an Arabic unit voice as the primary voice and an English unit voice as the secondary, RHVoice switches between them by script, exactly as Fasih does.

## In Fasih

Fasih has two kinds of Arabic voices. The compact voices are the HTS voices (a few megabytes each): Riad, Aseel and Elise come with the add-on, and the others are voice packs. The premium voices are unit voices (a few hundred megabytes each), offered only as voice packs in Tools > Fasih > Download voices, from the "voices" release. Both kinds run in the same engine, so any of them can be paired with Fasih's English and French voices.

```
OPUS_SRC=~/opus unit/build_engine.sh ~/rhv windows      # RHVoice.dll + libRHVoice_core.dll (zig 0.13)
cp ~/rhv/bw/src/lib/libRHVoice.dll third_party/rhvoice-win-x64/RHVoice.dll
cp ~/rhv/bw/src/core/libRHVoice_core.dll third_party/rhvoice-win-x64/
FASIH_PREMIUM="WORKDIR/voice/NAME" python3 tools/make_packs.py VERSION OUTDIR   # packs + voices.json for the release
```

`build_engine.sh` applies Fasih's engine patch (`engine/rhvoice-world-vocoder.patch`, against `engine/RHVOICE_BASE_COMMIT`) and the unit backend (`src/us_*`) to an RHVoice checkout, with sonic and Opus built into the core. The DLLs run every HTS voice as before. A premium voice folder may also hold `about.txt` (`name_ar=` and `description=` for the catalogue) and a `NOTICE.md` for its pack.

### Size

Nothing that changes the sound is used to make a voice smaller. `compact_voice.py`, the last training step, stores each boundary between neighbouring units once (99% of unit edges are shared) and the audio frame table as 16-bit packet sizes; the engine produces bit-identical audio from either form. Join features are stored in half precision, pitch marks as differences, and audio no unit can reach as digital silence. A lower Opus bitrate or dropping the least-used recordings would each save more, but both measurably change the result (higher spectral distortion below 56 kbps; about 14% higher selection cost and 6% more joins with a fifth of the recordings removed), so neither is done.

## Credits

- RHVoice, by Olga Yakovleva and contributors (GPL): the engine this backend lives in.
- sonic, by Bill Cox (Apache 2.0): pitch-synchronous rate and pitch changes.
- Opus, by Xiph.Org and contributors (BSD): audio compression.
- WORLD, by Masanori Morise: pitch analysis in training (and the vocoder of Fasih's HTS voices).
- Fasih's Arabic text pipeline, Rawi and ClaritySynth (Riad Assoum); Barq by Shamseddin of PlanetBlindTech, fine-tuned by Riad Assoum.
