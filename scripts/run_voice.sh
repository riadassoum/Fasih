#!/bin/bash
# Full training of one voice in a project root (corpus/, work/feat from feats.py done).
# usage: run_voice.sh ROOT     (each stage skipped if its output exists)
set -e
R=$(cd "$1" && pwd); cd "$R"
N=$(wc -l < work/ids.txt)
stage() { echo "== $(date +%H:%M) $*"; }
if [ "$(ls work/feat2/mgc 2>/dev/null | wc -l)" -lt "$N" ]; then
  stage "40-coef spectrum"
  python3 scripts/feats2.py work/feat2 0 $((N/2)) & python3 scripts/feats2.py work/feat2 $((N/2)) $N; wait
fi
if [ ! -s work/mono.ssml ]; then
  stage "alignment"
  python3 scripts/align.py init | tail -1
  for k in 1 2 3 4; do python3 scripts/align.py iter | tail -1; done
  for k in 1 2; do python3 scripts/align.py iter --pauses | tail -1; done
  python3 scripts/align.py final --pauses | tail -1
  python3 scripts/full_labels.py | tail -1
fi
export RHV_MGC_DIM=40 RHV_MGC_DIR=feat2 RHV_MIN_OCC_MGC=200
export RHV_TRAIN_DIR=train1; mkdir -p work/train1
[ -s work/train1/codes.npy ] || { stage "stats 1"; python3 scripts/train.py stats | tail -1; }
stage "trees 1"; python3 scripts/train.py trees | tail -3
if [ ! -s work/train2/codes.npy ]; then
  stage "context realign"; python3 scripts/train.py realign | tail -1
  export RHV_TRAIN_DIR=train2; mkdir -p work/train2
  stage "stats 2"; python3 scripts/train.py stats | tail -1
fi
export RHV_TRAIN_DIR=train2
stage "trees 2"; python3 scripts/train.py trees | tail -3
python3 scripts/train.py gv | tail -1
stage "done"
