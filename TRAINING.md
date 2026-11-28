# Training Fasih voices

Fasih has two kinds of voices, and anyone can train either from the same recordings.

| | Compact | Premium |
|---|---|---|
| How it speaks | statistical models (HTS) and the WORLD vocoder | real recorded speech, chosen and joined by unit selection |
| Size | a few megabytes | about 20 MB per hour of recordings |
| Recordings needed | two hours or more | five hours or more (ten or more is better) |
| Neural parts | none | none |
| Script | `train_compact_voice.sh` | `train_premium_voice.sh` |

## The corpus

Both scripts take the same folder:

- `metadata.csv`: one line per recording, `id|text`
- `wavs/id.wav`: the recordings, mono, any sample rate

One speaker, recorded consistently (same microphone, room and level). Arabic text must be fully diacritized; Fasih's numbered recording script already is. Recordings of that script work as they are.

## Setup (once)

On Linux (Ubuntu or similar), with git, cmake, a C++ compiler, Python 3 and `pip install numpy scipy scikit-learn numba pyworld pysptk soundfile soxr`, from the root of your clone:

```
bash unit/build_engine.sh
```

This fetches RHVoice at the commit Fasih is built on, with the parts it needs, and Opus 1.5.2, into the clone's `build` folder, and builds the engine, the label maker and the premium voice tools there. The training scripts find everything in that folder by themselves.

## Training

```
./train_compact_voice.sh Name /path/to/corpus ~/work/name-compact Arabic male
./train_premium_voice.sh Name /path/to/corpus ~/work/name-premium Arabic male
```

The language can be `Arabic`, `English` or `French`, and the gender `male` or `female`. Set `RHV_F0_MIN` and `RHV_F0_MAX` for the speaker's pitch range (defaults 60–400 Hz; for a low male voice 60–300). Both scripts can be interrupted and run again; they continue where they stopped. A compact voice takes many hours of computing; a premium voice of ten hours takes one to two hours on two cores.

The compact voice comes out in `~/work/name-compact/voice/Name`, finished the way Fasih's own voices are: its quality settings, gain and modulation postfilter included. `RHV_LEVEL_DB` (loudness) and `RHV_DUR_SCALE` (pace, below 1 is faster) adjust it while it is written; `tools/adjust_voice.py` changes them afterwards.

The premium voice comes out in `~/work/name-premium/voice/Name`. Two optional steps make it fit the other voices:

```
bash unit/prune_voice.sh ~/work/name-premium Name
python3 unit/match_voice.py premium.wav compact.wav ~/work/name-premium/voice/Name
```

`prune_voice.sh` removes the units the voice never chooses and their audio, without an audible difference (the shipped premium voices are a quarter to a third smaller this way). `match_voice.py` gives the voice the pace, loudness and tone of a compact voice, from two recordings of both voices reading the same text at default settings.

## Using a voice

- A compact voice goes in `voices/Name` (bundled with the add-on), or into a voice pack: `FASIH_ONLY=Name python3 tools/make_packs.py VERSION OUTDIR`.
- A premium voice becomes a voice pack: `FASIH_ONLY="Name," FASIH_PREMIUM="$HOME/work/name-premium/voice/Name" python3 tools/make_packs.py VERSION OUTDIR` (several voices: separate their folders with `:`). The unit table travels with its fixed fields column by column and its join features vector-quantized (4 bytes per unit edge); the add-on rebuilds it at install. Upload everything in OUTDIR to the "voices" release; it appears in Tools > Fasih > Download voices.
- To rebuild the Windows engine for the add-on: `bash unit/build_engine.sh "" windows` (needs `pip install ziglang==0.13.0`), then copy `build/rhvoice/bw/src/lib/libRHVoice.dll` to `third_party/rhvoice-win-x64/RHVoice.dll` and `build/rhvoice/bw/src/core/libRHVoice_core.dll` to `third_party/rhvoice-win-x64/`.

How the premium engine works, its files and its options: `unit/README.md`. How compact training works: `BUILD-NOTES.md`.
