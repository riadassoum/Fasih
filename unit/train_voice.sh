#!/bin/bash
# train_voice.sh NAME CORPUS OUTDIR [LANGUAGE] [GENDER]
# Builds a unit-selection voice from a recorded corpus, every stage resumable.
#   CORPUS:   metadata.csv ("id|text" per line) and wavs/ID.wav (any sample rate, mono).
#             Arabic text must be fully diacritized (Rawi can do it: see unit/README.md).
#   OUTDIR:   working folder; the finished voice is written to OUTDIR/voice/NAME
#   LANGUAGE: Arabic (default), English or French; GENDER: male (default) or female
# Environment: RHV_BUILD (RHVoice Linux build from build_engine.sh, default ../rhv/b),
#   RHV_F0_MIN / RHV_F0_MAX (pitch range, default 60-400 Hz), OPUS_KBPS (default 56), WORKERS (2)
set -euo pipefail
NAME=${1:?name}; SRC=$(cd "${2:?corpus}" && pwd); R=$(realpath -m "${3:?outdir}"); LANG_=${4:-Arabic}; GENDER=${5:-male}
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/.." && pwd)
RB=$(cd "${RHV_BUILD:-$REPO/../rhv/b}" && pwd)
W=${WORKERS:-2}
export LD_LIBRARY_PATH="$RB/src/core${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export RHVOICE_LABELS="$RB/src/utils/RHVoice-make-hts-labels" RHVOICE_LABEL_CONFIG="$R/labelcfg" RHV_LANG="$LANG_"
mkdir -p "$R/work" "$R/corpus/wav" "$R/labelcfg" "$R/data/languages" "$R/data/voices" "$R/langs" "$R/scripts"
cd "$R"
case "$LANG_" in
  Arabic)  LDIR="$REPO/lang/Arabic";             REF="$REPO/voices/Aseel";;
  English) LDIR="$REPO/third_party/rhvoice-english"; REF="$REPO/third_party/rhvoice-bdl";;
  French)  LDIR="$REPO/lang_fr/French";          REF="$REPO/voices/Elise";;
  *) echo "unknown language $LANG_"; exit 1;;
esac
ln -sfn "$LDIR" "data/languages/$LANG_"; ln -sfn "$REF" "data/voices/ref"
ln -sfn "$LDIR" "langs/$LANG_"; ln -sfn "$R/langs" lang
cp "$REPO"/scripts/*.py scripts/
stage() { echo "== $(date +%H:%M) $*"; }

if [ ! -s corpus/text.tsv ]; then
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
if [ ! -s work/ids.txt ]; then
  stage "labels"
  python3 scripts/make_ssml.py corpus/text.tsv work/input.ssml work/ids.txt | tail -n 1; mkdir -p work/lab0
  RHVOICE_DATA_PATH="$R/data" RHVOICE_CONFIG_PATH="$R/labelcfg" "$RHVOICE_LABELS" -p u work/input.ssml work/lab0 2>&1 | tail -n 1
fi
N=$(wc -l < work/ids.txt)
if [ "$(ls work/feat/bap 2>/dev/null | wc -l)" -lt "$N" ]; then
  stage "features (pitch, spectrum)"
  # aperiodicity is not used by unit voices
  sed -i 's/^    ap = pw.d4c(x, f0, t, SR)$/    ap = np.ones_like(sp)*0.5/' scripts/feats.py
  pids=(); for k in $(seq 0 $((W-1))); do python3 scripts/feats.py corpus/wav work/feat $((N*k/W)) $((N*(k+1)/W)) & pids+=($!); done
  wait "${pids[@]}"
fi
if [ ! -s work/mono.ssml ]; then
  stage "alignment"
  python3 scripts/align.py init --workers $W | tail -n 1
  for k in 1 2 3 4; do python3 scripts/align.py iter --workers $W | tail -n 1; done
  for k in 1 2; do python3 scripts/align.py iter --pauses --workers $W | tail -n 1; done
  python3 scripts/align.py final --pauses --workers $W | tail -n 1
  python3 scripts/full_labels.py | tail -n 1
fi
V="$R/voice/$NAME"; mkdir -p "$V"
if [ ! -s "$V/units.bin" ]; then
  stage "unit database"
  RHV_LABELLING="$LDIR/labelling.xml" python3 "$HERE/build_units.py" "$R" "$V" | tail -n 1
fi
[ -s "$V/marks.bin" ] || { stage "pitch marks"; WORKERS=$W python3 "$HERE/build_units.py" "$R" "$V" marks; }
[ -s "$V/models.bin" ] || { stage "duration, pitch and spectrum models (hybrid target cost, unit quality)"; python3 "$HERE/build_models.py" "$V" "$LDIR/phonemes.xml" | tail -n 5; }
if [ ! -s "$V/audio.opa" ]; then
  stage "audio compression (Opus)"
  "$HERE/bin/fasih-us-encode" "$V" "${OPUS_KBPS:-56}" 2>&1 | tail -n 1
fi
python3 "$HERE/compact_voice.py" "$V" | sed 's/^/   /'   # lossless: shared boundaries, 16-bit frame sizes
[ -s "$V/weights.txt" ] || printf "join_f0 12\nmax_cands 100\nling 0.5\ndur 0.6\nf0 0.6\nspec 0.4\njoin 1.5\n" > "$V/weights.txt"
printf "name=%s\nlanguage=%s\ngender=%s\nformat=3\nrevision=1\n" "$NAME" "$LANG_" "$GENDER" > "$V/voice.info"
stage "voice ready: $V (audio.pcm is only needed to re-encode; the add-on uses audio.opa)"
