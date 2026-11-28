#!/bin/bash
# pipeline.sh ROOT SRCDIR [PART]  : whole training of one voice, every stage resumable.
# SRCDIR has wav/ and text.tsv. PART = a|b|"" runs half of the feature extraction (for 2 cores).
set -e
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
R=$1; SRC=$2; PART=$3; mkdir -p $R/work $R/corpus; R=$(cd $R && pwd); cd $R
[ -e data ] || { ln -s ${RHV_DATA:-$REPO/data} data; ln -s ${RHV_LANG_DIR:-$REPO/lang} lang; ln -s $REPO/tools tools; mkdir -p scripts; cp $REPO/scripts/*.py $REPO/scripts/*.sh scripts/; ln -s $SRC/wav corpus/wav; cp $SRC/text.tsv corpus/text.tsv; }
if [ ! -s work/ids.txt ]; then
  python3 scripts/make_ssml.py corpus/text.tsv work/input.ssml work/ids.txt | tail -n 1; mkdir -p work/lab0
  RHVOICE_DATA_PATH=$R/data RHVOICE_CONFIG_PATH=${RHVOICE_LABEL_CONFIG:-$R/labelcfg} ${RHVOICE_LABELS:-$REPO/build/rhvoice/b/src/utils/RHVoice-make-hts-labels} -p u work/input.ssml work/lab0 2>&1 | tail -n 1
fi
N=$(wc -l < work/ids.txt); H=$((N/2))
case "$PART" in a) A=0; B=$H;; b) A=$H; B=$N;; *) A=0; B=$N;; esac
python3 scripts/feats.py corpus/wav work/feat $A $B
python3 scripts/feats2.py work/feat2 $A $B
[ -n "$PART" ] && { echo "features part $PART done"; exit 0; }
bash scripts/run_voice.sh . >> work/run.log 2>&1
[ -s work/train2v2/tree_mgc_4.pkl ] || bash tools/final_trees.sh . train2 >> work/run.log 2>&1
[ -s work/calib.txt ] || awk 'NR%60==20' work/ids.txt > work/calib.txt
[ -s work/train2v2/vratio.npy ] || bash tools/calib.sh . train2v2
echo "VOICE READY"
