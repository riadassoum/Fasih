#!/bin/bash
# Assemble the NVDA add-on.  usage: build_addon.sh VOICE_DIR VERSION OUT.nvda-addon
set -e
VOICE=$(cd "$1" && pwd); VER=$2; OUT=$(realpath -m "$3")
HERE=$(cd "$(dirname "$0")" && pwd); RHV=$(cd "$HERE/.." && pwd)
CS=/home/claude/salvage/cs/synthDrivers/claritysynth   # ClaritySynth 3.0 (Riad Assoum)
B=$RHV/build/addon; D=$B/synthDrivers/fasih
rm -rf "$RHV/build/addon"; mkdir -p "$D/lib/x64" "$D/data/languages" "$D/data/voices" "$D/arabic/lib/vowelizers"
python3 "$HERE/patch_driver.py" /tmp/rh/src/nvda-addon/synthDriver/__init__.py "$D/__init__.py"
cp "$HERE/arabic_pipeline.py" "$D/"
cp /tmp/rhwin/src/lib/libRHVoice.dll "$D/lib/x64/RHVoice.dll"
cp /tmp/rhwin/src/core/libRHVoice_core.dll "$D/lib/x64/"
cp -r "$RHV/lang/Arabic" "$D/data/languages/"
# VOICES="Name=project_root=train_dir=mgc_dir=mgc_dim ..." ; default = the single VOICE given
if [ -z "$VOICES" ]; then VOICES="$(basename "$VOICE")=$RHV=${RHV_TRAIN_DIR:-train}=${RHV_MGC_DIR:-feat}=${RHV_MGC_DIM:-31}"; fi
for spec in $VOICES; do
  IFS== read -r VN VROOT VTR VMG VDIM VLVL <<< "$spec"; VLVL=${VLVL:-0}; VGAIN=0.7
  mkdir -p "$D/data/voices/$VN/24000" "$D/data/variants/$VN"
  cp "$RHV/voice/bpf.txt" "$D/data/voices/$VN/24000/"
  printf 'name=%s\nlanguage=Arabic\ngender=%s\nformat=4\nrevision=1\n' "$VN" "$( [ "$VN" = Nour ] && echo female || echo male )" > "$D/data/voices/$VN/voice.info"
  for n in 0 6 12 20; do
    (cd "$VROOT" && RHV_TRAIN_DIR=$VTR RHV_MGC_DIR=$VMG RHV_MGC_DIM=$VDIM RHV_VOCODER=world RHV_LEVEL_DB=$VLVL RHV_BAP_OFFSET_DB=-$n python3 scripts/train.py write "$D/data/variants/$VN/b$n.data" >/dev/null)
  done
  echo "$VGAIN" > "$D/data/variants/$VN/gain.txt"
  python3 - "$D" "$VN" "$VGAIN" <<'PY'
import sys
d, vn, g = sys.argv[1], sys.argv[2], sys.argv[3]
b = open(f"{d}/data/variants/{vn}/b0.data", "rb").read().replace(b"USE_GV[MCP]:1", b"USE_GV[MCP]:0")
open(f"{d}/data/voices/{vn}/24000/voice.data", "wb").write(b)
open(f"{d}/data/voices/{vn}/voice.params", "w").write("beta=0.300\ngain=" + g + "\n")
PY
done
printf 'clarity=50\nbreathiness=b0\nsharpening=0\n' > "$D/data/quality.txt"
# English (RHVoice language data, LGPL) + SLT voice (CMU license), 24 kHz only
mkdir -p "$D/data/languages/English" "$D/data/voices/SLT"
cp /tmp/rh_English/* "$D/data/languages/English/" 2>/dev/null || true
cp /tmp/rh_slt-eng/voice.info /tmp/rh_slt-eng/voice.params /tmp/rh_slt-eng/README.md "$D/data/voices/SLT/"
cp -r /tmp/rh_slt-eng/24000 "$D/data/voices/SLT/"
# loudness matched to the Arabic voices (RHVoice's lowest allowed gain is 0.5)
sed -i 's/^gain=.*/gain=0.5/' "$D/data/voices/SLT/voice.params"; grep -q '^gain=' "$D/data/voices/SLT/voice.params" || echo "gain=0.5" >> "$D/data/voices/SLT/voice.params"
# Arabic text pipeline (from ClaritySynth, by Riad Assoum) + our wasl marker
for f in ar_tashkeel.py ar_g2p.py ar_textnorm.py _libboot.py data_paths.py ort_config.py thread_calibration.py; do cp "$CS/$f" "$D/arabic/"; done
cp -r "$CS/lib_pure" "$D/arabic/"
cp "$RHV/scripts/ar_prep.py" "$D/arabic/"
python3 "$HERE/patch_tashkeel.py" "$D/arabic/ar_tashkeel.py" "$HERE/rawi_client.py"
# PlanetBlindTech components (GPL-3.0): dictionaries, text fixes, audio post-filter
cp -r "$HERE/pbt" "$D/pbt"; touch "$D/pbt/__init__.py"
cp "$RHV/README.md" "$B/" 2>/dev/null || true
touch "$D/arabic/__init__.py"
# Rawi runs in its own process (rawi_host.exe + onnxruntime.dll); NVDA loads no numpy/onnxruntime
mkdir -p "$D/arabic/lib/onnxruntime/capi"
cp "$CS/lib/onnxruntime/capi/onnxruntime.dll" "$CS/lib/onnxruntime/capi/onnxruntime_providers_shared.dll" "$CS/lib/onnxruntime/capi/msvcp140.dll" "$CS/lib/onnxruntime/LICENSE" "$D/arabic/lib/onnxruntime/capi/"
cp "$HERE/rawi_host/rawi_host.exe" "$D/arabic/lib/"
cp "$CS/lib/vowelizers/rawi_ensemble.onnx" "$CS/lib/vowelizers/vocab.json" "$D/arabic/lib/vowelizers/"
find "$B" -name "__pycache__" -prune -exec rm -rf {} +
cat > "$B/manifest.ini" <<EOF
name = fasih
summary = "Fasih (فصيح): Arabic RHVoice voices"
description = """Fasih (فصيح): non-neural Arabic voices for the RHVoice engine (Jamal, Nour, Sami), with Rawi diacritization from ClaritySynth and the WORLD vocoder."""
author = "Riad Assoum <riad.assoum@gmail.com>"
url = https://github.com/riadassoum/Fasih
version = $VER
minimumNVDAVersion = 2024.1
lastTestedNVDAVersion = 2026.3
EOF
rm -f "$OUT"; (cd "$B" && zip -qr "$OUT" .)
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); print(len(z.namelist()),'files', z.testzip() or 'zip OK')" "$OUT"
ls -la "$OUT"
