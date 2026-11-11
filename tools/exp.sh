#!/bin/bash
# One spectrum-model experiment with held-out test.
# usage: exp.sh ROOT NAME DIM FEATDIR MINOCC MDL [GV]   (GV=1 keeps spectral global variance on)
set -e
R=$(cd "$1" && pwd); NAME=$2; DIM=$3; FD=$4; MO=$5; MDL=$6; GV=${7:-0}
cd "$R"
export RHV_MGC_DIM=$DIM RHV_MGC_DIR=$FD RHV_HOLDOUT=$R/work/holdout.txt
S=work/stats$DIM
if [ ! -s $S/codes.npy ]; then RHV_TRAIN_DIR=stats$DIM python3 scripts/train.py stats | tail -1; fi
B=work/xbase                       # duration, pitch and breath trees: shared by all experiments
if [ ! -s $B/tree_bap_4.pkl ]; then
  mkdir -p $B; for f in $S/*; do ln -sf "$R/$f" $B/; done
  RHV_TRAIN_DIR=xbase RHV_ONLY_STREAMS=dur,lf0,bap python3 scripts/train.py trees | tail -1
fi
T=work/$NAME; mkdir -p $T
for f in $S/*; do ln -sf "$R/$f" $T/; done
for f in $B/tree_dur* $B/tree_lf0* $B/tree_bap* $B/leafof_dur* $B/leafof_lf0* $B/leafof_bap*; do ln -sf "$R/$f" $T/; done
RHV_TRAIN_DIR=$NAME RHV_ONLY_STREAMS=mgc RHV_MIN_OCC_MGC=$MO python3 scripts/train.py trees $MDL | grep "tree mgc" | awk '{s+=$4} END {print "mgc leaves", s}'
if [ "$GV" = 1 ] && [ ! -s $T/gv.npz ]; then RHV_TRAIN_DIR=$NAME python3 scripts/train.py gv >/dev/null; fi
V=data/voices/$NAME; mkdir -p $V/24000
cp ../rhv/voice/bpf.txt $V/24000/
printf "name=$NAME\nlanguage=Arabic\ngender=male\nformat=4\nrevision=1\n" > $V/voice.info
printf "beta=0.300\ngain=0.7\n" > $V/voice.params
RHV_TRAIN_DIR=$NAME RHV_VOCODER=world python3 scripts/train.py write $V/24000/voice.data >/dev/null
python3 tools/eval_voice.py $R $NAME work/holdout.txt 2>&1 | tail -1
