#!/bin/bash
# lf0fix.sh ROOT SRC_DIR NEW_DIR MGCDIR : rebuild only the pitch/voicing models with ه and hamza
# no longer forced voiceless. Everything else is taken from SRC_DIR unchanged.
set -e
R=$(cd $1 && pwd); SRC=$2; NEW=$3; MG=$4; cd $R
export RHV_MGC_DIM=40 RHV_MGC_DIR=$MG RHV_VOICED_OK=h,Q RHV_STATS_NO_MGC=1
if [ ! -s work/${NEW}_st/st_lf0_4.npy ]; then
  RHV_TRAIN_DIR=${NEW}_st python3 scripts/train.py stats | tail -n 1
  rm -f work/${NEW}_st/st_mgc_* work/${NEW}_st/st_bap_*
fi
mkdir -p work/$NEW
for f in work/$SRC/*; do b=$(basename $f); case $b in tree_lf0*|leafof_lf0*|st_lf0*) ;; *) ln -sfn $(realpath $f) work/$NEW/$b;; esac; done
for f in work/${NEW}_st/st_lf0_*; do ln -sfn $(realpath $f) work/$NEW/; done
RHV_TRAIN_DIR=$NEW RHV_ONLY_STREAMS=lf0 python3 scripts/train.py trees | tail -n 5
