#!/bin/bash
# build_addon.sh VERSION OUT.nvda-addon VOICE_DIR...
# Builds the unit-selection add-on: Fasih's driver and Arabic text pipeline (Rawi, Barq, numbers,
# dictionary...), the RHVoice core with the unit-selection backend, the unit voices given on the
# command line, and Bdl (English) and Elise (French) as secondary voices for language switching.
#   VOICE_DIR: a built unit voice (units.bin, audio.opa, marks.bin, layout.txt, phones.txt,
#              context.txt, weights.txt, voice.info)
#   RHV_WIN:   RHVoice Windows build with the backend (build_engine.sh RHVOICE_SRC windows) -> its bw/ folder
#   ADDON_ID / ADDON_NAME: driver id and display name (defaults below)
set -euo pipefail
VER=${1:?version}; OUT=$(realpath -m "${2:?output}"); shift 2
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/.." && pwd)
RHV_WIN=${RHV_WIN:?set RHV_WIN to the bw/ build folder}
ID=${ADDON_ID:-fasihUnit}; NAME=${ADDON_NAME:-Fasih Unit}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
# start from a Fasih build with its secondary voices only
FASIH_CORE="Elise" bash "$REPO/addon_src/build_release.sh" "$VER" "$T/base.nvda-addon" >/dev/null
B=$T/b; mkdir -p "$B"; (cd "$B" && unzip -q "$T/base.nvda-addon")
mv "$B/synthDrivers/fasih" "$B/synthDrivers/$ID"
D=$B/synthDrivers/$ID
# the voice downloader belongs to Fasih (it installs Fasih voice packs); not part of this add-on
rm -rf "$B/globalPlugins"
python3 - "$D/__init__.py" "$ID" "$NAME" <<'E'
import sys
p, i, n = sys.argv[1:4]; s = open(p, encoding="utf-8").read()
s = s.replace('    name = "fasih"\n', '    name = "%s"\n' % i, 1)
s = s.replace('    description = "Fasih (فصيح), RHVoice Arabic"', '    description = "%s"' % n, 1)
open(p, "w", encoding="utf-8").write(s)
E
cp "$RHV_WIN/src/lib/libRHVoice.dll" "$D/lib/x64/RHVoice.dll"
cp "$RHV_WIN/src/core/libRHVoice_core.dll" "$D/lib/x64/libRHVoice_core.dll"
for V in "$@"; do
  VN=$(sed -n 's/^name=//p' "$V/voice.info")
  mkdir -p "$D/data/voices/$VN"
  for f in voice.info units.bin audio.opa marks.bin layout.txt phones.txt context.txt weights.txt; do
    cp -L "$V/$f" "$D/data/voices/$VN/"
  done
  [ -e "$V/voice.params" ] && cp -L "$V/voice.params" "$D/data/voices/$VN/"
done
VOICES=$(for V in "$@"; do sed -n 's/^name=//p' "$V/voice.info"; done | paste -sd, - | sed 's/,/, /g')
cat > "$B/manifest.ini" <<EOF
name = $ID
summary = "$NAME: Arabic concatenative voices"
description = """$NAME: high-quality concatenative (unit-selection) Arabic voices for NVDA ($VOICES), built on Fasih's RHVoice engine and Arabic text processing (Rawi or Barq diacritization, numbers, abbreviations, pronunciation dictionary), with English and French voices for language switching. Non-neural speech: recorded speech units chosen by a Viterbi search and joined with pitch-synchronous smoothing."""
author = "Riad Assoum"
url = https://github.com/riadassoum/Fasih
version = $VER
minimumNVDAVersion = 2026.1
lastTestedNVDAVersion = 2026.3
docFileName = readme.html
EOF
find "$B" -name "__pycache__" -prune -exec rm -rf {} +
rm -f "$OUT"; (cd "$B" && zip -qr "$OUT" .)
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); print(len(z.namelist()),'files', z.testzip() or 'zip OK')" "$OUT"
ls -la "$OUT"
