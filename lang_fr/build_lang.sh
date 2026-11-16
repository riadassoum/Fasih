#!/bin/bash
# Build the French data-only RHVoice language into $OUT (default ../French)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
RH=${RH:-/tmp/rh}
OUT=${1:-$HERE/French}
LEX=${LEX:-/home/claude/ext/ipa-dict/data/fr_FR.txt}
GEN=$HERE/src
python3 "$HERE/gen_fr.py" "$LEX" "$GEN"
mkdir -p "$OUT"
cd "$GEN"
for f in g2p syl tok downcase gpos spell numbers key msg lseq; do
  foma -e "source $f.foma" -e "write att $f.att" -s > $f.log 2>&1 || true
  [ -s $f.att ] || { echo "foma failed: $f"; tail -5 $f.log; exit 1; }
  python3 "$RH/src/scripts/general/att2bin" $f.att "$OUT/$f.fst"
done
python3 "$RH/src/scripts/general/dtree2bin" phrasing.tree "$OUT/phrasing.dt"
cp phonemes.xml "$OUT/"
cp /home/claude/rhv/lang/Arabic/labelling.xml "$OUT/labelling.xml"
python3 - "$OUT/graph.txt" <<'PY'
import sys
L = "abcdefghijklmnopqrstuvwxyzàâæçèéêëîïñôöùûüÿœ"
V = set("aàâæeéèêëiîïoôöuùûüyÿœ")
with open(sys.argv[1], "w", encoding="utf-8") as f:
    for ch in L + L.upper():
        f.write(f"{ch} {'v' if ch.lower() in V else 'c'}\n")
PY
printf 'name=French\ndata_only=true\nformat=1\nrevision=1\n' > "$OUT/language.info"
printf 'language2=fr\nlanguage3=fra\nms_id=1036\n' > "$OUT/locale.info"
printf 'g2p.case=false\n' > "$OUT/language.conf"
echo "built $OUT"; du -sh "$OUT"
