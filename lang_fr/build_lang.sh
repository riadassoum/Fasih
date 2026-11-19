#!/bin/bash
# Build the French data-only RHVoice language into $OUT (default ../French)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
# RH: an RHVoice source checkout (for att2bin and dtree2bin)
RH=${RH:?set RH to an RHVoice source checkout}
OUT=${1:-$HERE/French}
# the pronunciation lexicon (ipa-dict, MIT); fetched once if it is not there
LEX=${LEX:-$HERE/fr_FR.txt}
[ -s "$LEX" ] || curl -sSL -o "$LEX" https://raw.githubusercontent.com/open-dict-data/ipa-dict/master/data/fr_FR.txt
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
cp "$HERE/../lang/Arabic/labelling.xml" "$OUT/labelling.xml"
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
