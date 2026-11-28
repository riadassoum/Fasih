#!/bin/bash
# Build the Arabic data-only RHVoice language into $OUT (default ../Arabic)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
RH=${RH:-$HERE/../build/rhvoice}   # an RHVoice checkout (unit/build_engine.sh fetches one)
ALB=${ALB:-$RH/data/languages/Albanian}   # RHVoice's Albanian language (git -C $RH submodule update --init data/languages/Albanian)
OUT=${1:-$HERE/Arabic}
GEN=$HERE/src
python3 "$HERE/gen_foma.py" "$GEN" >/dev/null
mkdir -p "$OUT"
cd "$GEN"
for f in g2p syl tok downcase gpos spell numbers key msg lseq; do
  foma -e "source $f.foma" -e "write att $f.att" -s >/dev/null 2>&1
  [ -s $f.att ] || { echo "foma failed: $f"; exit 1; }
  python3 "$RH/src/scripts/general/att2bin" $f.att "$OUT/$f.fst"
done
python3 "$RH/src/scripts/general/dtree2bin" phrasing.tree "$OUT/phrasing.dt"
cp phonemes.xml "$OUT/"
sed 's/prev_ph_flag_l\|next_ph_flag_l\|ph_flag_l/none/' "$ALB/labelling.xml" > "$OUT/labelling.xml"
python3 - "$OUT/graph.txt" <<'EOF'
import sys
letters = [chr(c) for c in list(range(0x0621, 0x063B)) + list(range(0x0641, 0x064B))] + ["ٱ", "ڤ", "پ", "گ", "چ"]
marks = [chr(c) for c in range(0x064B, 0x0653)] + ["ٰ", "ـ"]
with open(sys.argv[1], "w", encoding="utf-8") as f:
    for ch in letters:
        f.write(f"{ch} {'v' if ch in chr(0x627)+chr(0x648)+chr(0x64A)+chr(0x649) else 'c'}\n")
    for ch in marks:
        f.write(f"{ch} {'v' if ch in ''.join(chr(c) for c in range(0x64B,0x651)) else 'c'}\n")
EOF
printf 'name=Arabic\ndata_only=true\nformat=1\nrevision=1\n' > "$OUT/language.info"
printf 'language2=ar\nlanguage3=ara\nms_id=1025\n' > "$OUT/locale.info"
printf 'g2p.case=false\n' > "$OUT/language.conf"
echo "built $OUT"
