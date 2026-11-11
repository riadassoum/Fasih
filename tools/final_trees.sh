#!/bin/bash
# final_trees.sh ROOT SRC_TRAIN_DIR : new dir <SRC>v2 = same stats/pitch/duration/breath trees, larger spectrum trees
R=$(cd "$1" && pwd); SRC=$2; T=$R/work/${SRC}v2; mkdir -p $T
for f in $R/work/$SRC/*; do b=$(basename $f); case $b in tree_mgc*|leafof_mgc*|gv.npz) ;; *) ln -sf $f $T/$b;; esac; done
cd $R && RHV_MGC_DIM=40 RHV_MGC_DIR=feat2 RHV_TRAIN_DIR=${SRC}v2 RHV_ONLY_STREAMS=mgc RHV_MIN_OCC_MGC=50 python3 scripts/train.py trees 0.5
