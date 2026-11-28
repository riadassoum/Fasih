#!/bin/bash
# prune_voice.sh WORKDIR NAME [workers]
# Makes a premium voice from train_premium_voice.sh smaller without an audible difference: each recorded
# sentence is spoken by the voice without its own recording, units that are never chosen (and not next to
# a chosen one) are marked removed (prune_voice.py), and the audio no kept unit can reach is dropped from
# audio.opa (prune_audio.py, lossless for what is kept). The voice in WORKDIR/voice/NAME is changed in
# place; a copy of its units.bin and audio.opa is kept as *.unpruned. Needs unit/build_engine.sh's tools.
set -euo pipefail
R=$(cd "$1" && pwd); NAME=$2; W=${3:-$(nproc)}
HERE=$(cd "$(dirname "$0")" && pwd)
V="$R/voice/$NAME"; WK="$R/work"
[ -s "$V/units.bin" ] && [ -d "$WK/full" ] || { echo "no trained voice in $V (run train_premium_voice.sh first)"; exit 1; }
[ -x "$HERE/bin/fasih-us-usage" ] || { echo "run unit/build_engine.sh first"; exit 1; }
[ -e "$V/units.bin.unpruned" ] || cp "$V/units.bin" "$V/units.bin.unpruned"
[ -e "$V/audio.opa.unpruned" ] || cp "$V/audio.opa" "$V/audio.opa.unpruned"
cp "$V/units.bin.unpruned" "$V/units.bin"; cp "$V/audio.opa.unpruned" "$V/audio.opa"
T=$(mktemp -d)
python3 - "$WK" "$R/corpus/text.tsv" "$T/list" <<'E'
import os, sys
wk, tsv, out = sys.argv[1:4]
ids = sorted(n[:-4] for n in os.listdir(os.path.join(wk, "mono")) if n.endswith(".lab"))
txt = {l.split("\t")[0]: l.rstrip("\n").split("\t")[2] for l in open(tsv, encoding="utf-8") if l.count("\t") >= 2}
with open(out, "w") as f:
    for i, u in enumerate(ids):
        t = txt.get(u, "").strip()
        ty = "q" if t.endswith(("?", "؟")) else "e" if t.endswith("!") else "s"
        f.write("%d %s %s\n" % (i, os.path.join(wk, "full", u + ".lab"), ty))
print(len(ids), "sentences")
E
split -n l/"$W" "$T/list" "$T/part_"
echo "choosing units for every sentence (this takes a while)"
pids=(); for p in "$T"/part_*; do "$HERE/bin/fasih-us-usage" "$V" "$p" "$p.used" "$p.u32" 2>/dev/null & pids+=($!); done
wait "${pids[@]}"
python3 - "$T" <<'E'
import sys, glob, numpy as np
parts = sorted(glob.glob(sys.argv[1] + "/part_*.u32"))
np.sum([np.fromfile(p, "<u4").astype(np.uint64) for p in parts], 0).astype("<u4").tofile(sys.argv[1] + "/uses.u32")
E
python3 "$HERE/prune_voice.py" "$V" "$T/uses.u32" | tail -n 2
python3 "$HERE/prune_audio.py" "$V" | tail -n 2
rm -rf "$T"
echo "pruned: $V (the originals are units.bin.unpruned and audio.opa.unpruned; packs leave them out)"
