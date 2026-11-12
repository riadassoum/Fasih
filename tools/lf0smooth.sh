#!/bin/bash
# lf0smooth.sh ROOT SRC_DIR NEW_DIR : pitch models with ه/hamza voiced and larger minimum occupancy
# (fewer, better-supported pitch models: no more pitch jumps between neighbouring states)
set -e
R=$(cd $1 && pwd); SRC=$2; NEW=$3; cd $R
export RHV_MGC_DIM=40 RHV_MGC_DIR=feat2 RHV_VOICED_OK=h,Q RHV_STATS_NO_MGC=1 RHV_LF0_CLEAN=1
ST=work/${SRC}_lf0st
[ -s $ST/st_lf0_4.npy ] || RHV_TRAIN_DIR=$(basename $ST) python3 scripts/train.py stats | tail -n 1
mkdir -p work/$NEW
for f in work/$SRC/*; do b=$(basename "$f"); case $b in tree_lf0*|leafof_lf0*|st_lf0*) ;; *) ln -sfn "$(realpath "$f")" work/$NEW/$b;; esac; done
for f in $ST/st_lf0_*; do ln -sfn "$(realpath "$f")" work/$NEW/; done
RHV_TRAIN_DIR=$NEW RHV_ONLY_STREAMS=lf0 RHV_MIN_OCC_LF0=${MINOCC:-200} python3 scripts/train.py trees | grep tree
rm -f $ST/st_lf0_*.npy
