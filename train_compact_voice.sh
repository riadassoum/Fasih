#!/bin/bash
# train_compact_voice.sh NAME CORPUS OUTDIR [LANGUAGE] [GENDER]
# Trains a compact Fasih voice (HTS statistical models, WORLD vocoder): a few megabytes, no neural parts.
#   CORPUS:   metadata.csv ("id|text" per line) and wavs/ID.wav (any sample rate, mono), one speaker.
#             Arabic text must be fully diacritized. Two hours or more; more is better.
#   OUTDIR:   working folder; the finished voice is OUTDIR/voice/NAME, ready for voices/ or a voice pack
#   LANGUAGE: Arabic (default), English or French; GENDER: male (default) or female
# Every stage is resumable: run the same command again after an interruption.
# Needs the RHVoice Linux build from unit/build_engine.sh (RHV_BUILD, default build/rhvoice/b: run unit/build_engine.sh once) and Python with
# numpy, scipy, numba, pyworld, pysptk, soundfile, soxr. Pitch range: RHV_F0_MIN / RHV_F0_MAX (60-400 Hz).
# Voice options when writing (see addon_src/build_addon.sh): RHV_LEVEL_DB, RHV_PAU_SCALE, VOICE_GAIN.
# For a premium (concatenative) voice from the same corpus, use train_premium_voice.sh.
set -euo pipefail
NAME=${1:?name}; SRC=$(cd "${2:?corpus}" && pwd); R=$(realpath -m "${3:?outdir}"); LANG_=${4:-Arabic}; GENDER=${5:-male}
REPO=$(cd "$(dirname "$0")" && pwd)
RB=$(cd "${RHV_BUILD:-$REPO/build/rhvoice/b}" && pwd)
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

V="$R/voice/$NAME"; VAR="$V/variants"; mkdir -p "$V/24000" "$VAR"
# The voice as Fasih ships it: variants/base.data plus byte patches for its quality settings
# (breathiness b6/b12/b20, "repair"), gain.txt, and the modulation postfilter (ms_gain.txt) measured
# against the recordings. Options: RHV_LEVEL_DB (loudness), RHV_DUR_SCALE (pace, <1 faster),
# RHV_EQ_MC (a tone filter from tools/eq_match.py), RHV_PAU_SCALE, VOICE_GAIN.
stage "writing the voice and its variants"
cp "$REPO/engine/bpf.txt" "$V/24000/"
printf 'name=%s\nlanguage=%s\ngender=%s\nformat=4\nrevision=1\n' "$NAME" "$LANG_" "$GENDER" > "$V/voice.info"
printf 'beta=0.300\ngain=%s\n' "${VOICE_GAIN:-0.7}" > "$V/voice.params"; echo "${VOICE_GAIN:-0.7}" > "$VAR/gain.txt"
EXTRA="RHV_PAU_SILENT=1 RHV_DUR_SCALE=${RHV_DUR_SCALE:-1}"
[ "$LANG_" = Arabic ] && EXTRA="$EXTRA RHV_ALLAH=1"
[ -n "${RHV_EQ_MC:-}" ] && EXTRA="$EXTRA RHV_EQ_MC=$(realpath "$RHV_EQ_MC")"
VS="$R/train/work/train2v2/vratio.npy"; [ -s "$VS" ] && EXTRA="$EXTRA RHV_VSCALE=$VS RHV_VSCALE_W=0.7"
wr() { (cd "$R/train" && env $EXTRA $2 RHV_PAU_SCALE=${RHV_PAU_SCALE:-1.0} RHV_TRAIN_DIR=train2v5 RHV_MGC_DIR=feat2 RHV_MGC_DIM=40 \
          RHV_VOCODER=world RHV_DUR_PCT=97 RHV_LEVEL_DB=${RHV_LEVEL_DB:-0} RHV_BAP_OFFSET_DB=$1 python3 scripts/train.py write "$3" > /dev/null); }
TMPV=$(mktemp -d)
wr 0 "" "$VAR/base.data"
for n in 6 12 20; do wr -$n "" "$TMPV/b$n.data"; python3 "$REPO/addon_src/mkpatch.py" "$VAR/base.data" "$TMPV/b$n.data" "$VAR/b$n.patch" > /dev/null; done
wr 0 "RHV_FIX_VOICELESS=1 RHV_PHONE_C0=Q:1-3:-3 RHV_SOFT_UNVOICED=1" "$TMPV/repair.data"
python3 "$REPO/addon_src/mkpatch.py" "$VAR/base.data" "$TMPV/repair.data" "$VAR/repair.patch" > /dev/null
rm -rf "$TMPV"

stage "modulation postfilter (measured against the recordings)"
MS="$R/msdata"; rm -rf "$MS"; mkdir -p "$MS/voices/$NAME/24000" "$MS/languages"
ln -sfn "$LDIR" "$MS/languages/$LANG_"; cp "$V/voice.info" "$V/voice.params" "$MS/voices/$NAME/"
cp "$V/24000/bpf.txt" "$MS/voices/$NAME/24000/"; cp "$VAR/base.data" "$MS/voices/$NAME/24000/voice.data"
RHVOICE_LIB="$RB/src/lib/libRHVoice.so" python3 "$REPO/tools/ms_estimate.py" "$NAME" "$MS" "$R/corpus" "$VAR/ms_gain.txt" | tail -n 1
rm -rf "$MS"
stage "compact voice ready: $V (copy it to voices/$NAME to build it into the add-on, or pack it with tools/make_packs.py)"
