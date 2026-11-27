# Training Fasih voices

Fasih has two kinds of voices, and anyone can train either from the same recordings.

| | Compact | Premium |
|---|---|---|
| How it speaks | statistical models (HTS) and the WORLD vocoder | real recorded speech, chosen and joined by unit selection, made audible by a neural vocoder |
| Size | a few megabytes | about 14 MB per hour of recordings |
| Recordings needed | two hours or more | five hours or more (ten or more is better) |
| Neural parts | none | the vocoder (downloaded once, with the first premium voice) |
| Script | `train_compact_voice.sh` | `train_premium_voice.sh` |

## The corpus

Both scripts take the same folder:

- `metadata.csv`: one line per recording, `id|text`
- `wavs/id.wav`: the recordings, mono, any sample rate

One speaker, recorded consistently (same microphone, room and level). Arabic text must be fully diacritized; Fasih's numbered recording script already is. Recordings of that script work as they are.

## Setup (once)

On Linux, with Python 3 and `pip install numpy scipy scikit-learn numba pyworld pysptk soundfile soxr`:

```
git clone https://github.com/RHVoice/RHVoice ~/rhv && git -C ~/rhv checkout $(cat engine/RHVOICE_BASE_COMMIT)
git clone --branch v1.5.2 https://github.com/xiph/opus ~/opus
OPUS_SRC=~/opus unit/build_engine.sh ~/rhv linux
```

## Training

```
RHV_BUILD=~/rhv/b ./train_compact_voice.sh Name /path/to/corpus ~/work/name-compact Arabic male
RHV_BUILD=~/rhv/b ./train_premium_voice.sh Name /path/to/corpus ~/work/name-premium Arabic male
```

The language can be `Arabic`, `English` or `French`. Set `RHV_F0_MIN` and `RHV_F0_MAX` for the speaker's pitch range (defaults 60–400 Hz; for a low male voice 60–300). Both scripts can be interrupted and run again; they continue where they stopped. A compact voice takes many hours of computing; a premium voice of ten hours takes one to two hours on two cores.

## Using a voice

- A compact voice goes in `voices/Name` (bundled with the add-on), or into a voice pack: `FASIH_ONLY=Name python3 tools/make_packs.py VERSION OUTDIR`.
- A premium voice becomes a voice pack: `FASIH_ONLY="Name," FASIH_PREMIUM=~/work/name-premium/voice/Name python3 tools/make_packs.py VERSION OUTDIR`. This also builds the vocoder pack. Upload everything in OUTDIR to the "voices" release; it appears in Tools > Fasih > Download voices.

How the premium engine works, its files and its options: `unit/README.md`. How compact training works: `BUILD-NOTES.md`.
