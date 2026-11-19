#!/bin/bash
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
# calib.sh ROOT TRAINDIR : measure how flat the model's spectrum is against the recordings -> work/TRAINDIR/vratio.npy
R=$(cd "$1" && pwd); T=$2; cd $R; N=Calib$(basename $R); V=data/voices/$N; mkdir -p $V/24000
BPF=$REPO/engine/bpf.txt; [ -s "$BPF" ] || BPF=$REPO/voice/bpf.txt
cp "$BPF" $V/24000/; printf "name=$N\nlanguage=${RHV_LANG:-Arabic}\ngender=male\nformat=4\nrevision=1\n" > $V/voice.info; printf "beta=0.300\ngain=0.7\n" > $V/voice.params
RHV_MGC_DIM=40 RHV_MGC_DIR=feat2 RHV_TRAIN_DIR=$T RHV_VOCODER=world python3 scripts/train.py write $V/24000/voice.data > /dev/null
EVAL_SAVE_GV=work/$T/vratio.npy python3 "$REPO/tools/eval_voice.py" $R $N work/calib.txt 2>&1 | tail -n 1
ls -la work/$T/vratio.npy | awk '{print $5}'
