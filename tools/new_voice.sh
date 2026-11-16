#!/bin/bash
# new_voice.sh ROOT SRCDIR : train one voice with the full 1.1 recipe, every stage resumable.
#   base training (pipeline.sh) -> train2, larger spectrum trees -> train2v2
#   duration: final-lengthening cap + separate pause tree      -> train2v3
#   pitch: h and hamza voiced, pitch slips cleaned              -> train2v4_lf0st
#   pitch trees at min occupancy 200                            -> train2v5 (the voice to build)
# Features are deleted at the end (writing a voice does not need them; they can be re-extracted).
set -e
R=$1; SRC=$2
H=/home/claude/rhv/tools
if [ ! -s "$R/work/train2v2/tree_mgc_4.pkl" ]; then
  # features in two processes, then the rest of the base pipeline
  # (part b starts once part a has set up the root and made the labels)
  bash $H/pipeline.sh "$R" "$SRC" a & PA=$!
  until [ -s "$R/work/ids.txt" ] && [ "$(ls $R/work/lab0 2>/dev/null | wc -l)" -ge "$(wc -l < $R/work/ids.txt)" ]; do sleep 5; done
  bash $H/pipeline.sh "$R" "$SRC" b; wait $PA
  bash $H/pipeline.sh "$R" "$SRC"
fi
R=$(cd "$R" && pwd); cd "$R"
export RHV_MGC_DIM=40 RHV_MGC_DIR=feat2
link() { # link everything from $1 into $2 except files matching $3
  mkdir -p work/$2
  for f in work/$1/*; do b=$(basename $f); [[ $b =~ $3 ]] || ln -sfn "$(realpath $f)" work/$2/$b; done
}
if [ ! -s work/train2v3/tree_dur_None.pkl ]; then
  link train2v2 train2v3 '^(tree_dur|leafof_dur)'
  RHV_TRAIN_DIR=train2v3 RHV_ONLY_STREAMS=dur RHV_DUR_FINAL_CAP=1.5 RHV_DUR_SPLIT_PAU=1 python3 scripts/train.py trees | tail -n 2
fi
if [ ! -s work/train2v4_lf0st/st_lf0_4.npy ]; then
  RHV_TRAIN_DIR=train2v4_lf0st RHV_VOICED_OK=h,Q RHV_LF0_CLEAN=1 RHV_STATS_NO_MGC=1 python3 scripts/train.py stats | tail -n 1
fi
if [ ! -s work/train2v5/tree_lf0_4.pkl ]; then
  link train2v3 train2v5 '^(tree_lf0|leafof_lf0|st_lf0)'
  for f in work/train2v4_lf0st/st_lf0_*; do ln -sfn "$(realpath $f)" work/train2v5/; done
  RHV_TRAIN_DIR=train2v5 RHV_ONLY_STREAMS=lf0 RHV_MIN_OCC_LF0=200 python3 scripts/train.py trees | tail -n 5
fi
rm -rf work/feat work/feat2 work/lab0
echo "VOICE V5 READY"
