#!/bin/bash
# train_premium_voice.sh NAME CORPUS OUTDIR [LANGUAGE] [GENDER]
# Trains a premium Fasih voice: concatenative unit selection over the real recordings, guided by
# duration, pitch and spectrum models, stored as Opus and joined directly, with no vocoder.
# Larger than a compact voice (about 14 MB per hour of recordings).
#   CORPUS:   metadata.csv ("id|text" per line) and wavs/ID.wav (any sample rate, mono), one speaker.
#             Arabic text must be fully diacritized. Five hours or more; ten or more is better.
#   OUTDIR:   working folder; the finished voice is OUTDIR/voice/NAME
#   LANGUAGE: Arabic (default), English or French; GENDER: male (default) or female
# Every stage is resumable. OPUS_KBPS sets the audio bitrate (default 48).
# Needs the RHVoice Linux build and tools from unit/build_engine.sh (RHV_BUILD, default build/rhvoice/b: run unit/build_engine.sh once) and
# Python with numpy, scipy, scikit-learn, numba, pyworld, pysptk, soundfile, soxr.
# For a compact (HTS) voice from the same corpus, use train_compact_voice.sh. Details: unit/README.md.
set -euo pipefail
REPO=$(cd "$(dirname "$0")" && pwd)
exec bash "$REPO/unit/train_voice.sh" "$@"
