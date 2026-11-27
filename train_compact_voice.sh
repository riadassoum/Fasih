#!/bin/bash
# train_compact_voice.sh NAME CORPUS OUTDIR [LANGUAGE] [GENDER]
# Trains a compact Fasih voice (HTS statistical models, WORLD vocoder): a few megabytes, no neural parts.
#   CORPUS:   metadata.csv ("id|text" per line) and wavs/ID.wav (any sample rate, mono), one speaker.
#             Arabic text must be fully diacritized. Two hours or more; more is better.
#   OUTDIR:   working folder; the finished voice is OUTDIR/voice/NAME, ready for voices/ or a voice pack
#   LANGUAGE: Arabic (default), English or French; GENDER: male (default) or female
# Every stage is resumable: run the same command again after an interruption.
# Needs the RHVoice Linux build from unit/build_engine.sh (RHV_BUILD, default ../rhv/b) and Python with
# numpy, scipy, numba, pyworld, pysptk, soundfile, soxr. Pitch range: RHV_F0_MIN / RHV_F0_MAX (60-400 Hz).
# Voice options when writing (see addon_src/build_addon.sh): RHV_LEVEL_DB, RHV_PAU_SCALE, VOICE_GAIN.
# For a premium (concatenative) voice from the same corpus, use train_premium_voice.sh.
set -euo pipefail
NAME=${1:?name}; SRC=$(cd "${2:?corpus}" && pwd); R=$(realpath -m "${3:?outdir}"); LANG_=${4:-Arabic}; GENDER=${5:-male}
REPO=$(cd "$(dirname "$0")" && pwd)
RB=$(cd "${RHV_BUILD:-$REPO/../rhv/b}" && pwd)
W=${WORKERS:-2}
export LD_LIBRARY_PATH="$RB/src/core${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export RHVOICE_LABELS="$RB/src/utils/RHVoice-make-hts-labels" RHVOICE_LABEL_CONFIG="$R/labelcfg" RHV_LANG="$LANG_"
mkdir -p "$R/labelcfg" "$R/rhvdata/languages" "$R/rhvdata/voices" "$R/langs" "$R/corpus/wav"
case "$LANG_" in
  Arabic)  LDIR="$REPO/lang/Arabic";                REF="$REPO/voices/Aseel";;
  English) LDIR="$REPO/third_party/rhvoice-english"; REF="$REPO/third_party/rhvoice-bdl";;
  French)  LDIR="$REPO/lang_fr/French";             REF="$REPO/voices/Elise";;
  *) echo "unknown language $LANG_"; exit 1;;
esac
ln -sfn "$LDIR" "$R/rhvdata/languages/$LANG_"; ln -sfn "$REF" "$R/rhvdata/voices/ref"; ln -sfn "$LDIR" "$R/langs/$LANG_"
export RHV_DATA="$R/rhvdata" RHV_LANG_DIR="$R/langs"
stage() { echo "== $(date +%H:%M) $*"; }

# the compact pipeline takes wav/ at 24 kHz and text.tsv (id, duration, text)
if [ ! -s "$R/corpus/text.tsv" ]; then
  stage "corpus: 24 kHz audio and text"
  python3 - "$SRC" "$R/corpus" "$W" <<'E'
import sys, os, soundfile as sf, soxr
from multiprocessing import Pool
S, D, W = sys.argv[1], sys.argv[2], int(sys.argv[3])
rows = [l.rstrip("\n").split("|", 1) for l in open(os.path.join(S, "metadata.csv"), encoding="utf-8") if "|" in l]
def one(r):
    i, t = r; o = os.path.join(D, "wav", i + ".wav")
    x, sr = sf.read(os.path.join(S, "wavs", i + ".wav"))
    if x.ndim > 1: x = x.mean(1)
    if sr != 24000: x = soxr.resample(x, sr, 24000, "VHQ")
    if not os.path.exists(o): sf.write(o, x, 24000, subtype="PCM_16")
    return i, len(x) / 24000, t.strip()
with Pool(W) as p: out = p.map(one, rows, 64)
with open(os.path.join(D, "text.tsv"), "w", encoding="utf-8") as f:
    for i, d, t in out: f.write("%s\t%.3f\t%s\n" % (i, d, t))
print(len(out), "utterances,", round(sum(o[1] for o in out) / 3600, 2), "hours")
E
fi

stage "training (labels, features, alignment, models): this takes hours"
bash "$REPO/tools/new_voice.sh" "$R/train" "$R/corpus"

V="$R/voice/$NAME"; mkdir -p "$V/24000"
stage "writing the voice"
(cd "$R/train" && env RHV_TRAIN_DIR=train2v5 RHV_MGC_DIR=feat2 RHV_MGC_DIM=40 RHV_VOCODER=world RHV_DUR_PCT=97 \
   RHV_LEVEL_DB=${RHV_LEVEL_DB:-0} RHV_PAU_SCALE=${RHV_PAU_SCALE:-1.0} python3 scripts/train.py write "$V/24000/voice.data" > /dev/null)
printf 'name=%s\nlanguage=%s\ngender=%s\nformat=4\nrevision=1\n' "$NAME" "$LANG_" "$GENDER" > "$V/voice.info"
printf 'beta=0.300\ngain=%s\n' "${VOICE_GAIN:-0.7}" > "$V/voice.params"
stage "compact voice ready: $V (copy it to voices/$NAME, or pack it with tools/make_packs.py)"
