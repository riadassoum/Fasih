#!/bin/bash
# Build the NVDA add-on from this repository alone (a clean checkout is enough).
# usage: addon_src/build_release.sh VERSION [OUT.nvda-addon]
# Needs only bash, python3 and zip. Retraining the voices is a separate process (see BUILD-NOTES.md).
set -euo pipefail
VER=${1:?version}
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${2:-$REPO/Fasih-$VER.nvda-addon}")
HERE=$REPO/addon_src; T=$REPO/third_party
B=$(mktemp -d); D=$B/synthDrivers/fasih
trap 'rm -rf "$B"' EXIT
mkdir -p "$D/lib/x64" "$D/data/languages" "$D/data/voices" "$D/data/variants" "$D/arabic/lib/vowelizers" "$D/arabic/lib/onnxruntime/capi"

# driver: RHVoice's official NVDA driver, patched for Fasih
python3 "$HERE/patch_driver.py" "$T/rhvoice-nvda-driver/__init__.py" "$D/__init__.py"
cp "$HERE/arabic_pipeline.py" "$HERE/loudness.py" "$D/"
cp -r "$HERE/locale" "$B/"
cp -r "$HERE/doc" "$B/"
cp "$T/rhvoice-win-x64/RHVoice.dll" "$T/rhvoice-win-x64/libRHVoice_core.dll" "$D/lib/x64/"

# languages and voices
cp -r "$REPO/lang/Arabic" "$D/data/languages/"
mkdir -p "$D/data/languages/English"; cp -r "$T/rhvoice-english/." "$D/data/languages/English/"
for V in "$REPO"/voices/*/; do
  VN=$(basename "$V")
  mkdir -p "$D/data/voices/$VN/24000" "$D/data/variants/$VN"
  cp "$V/voice.info" "$V/voice.params" "$D/data/voices/$VN/"
  cp "$V/24000/bpf.txt" "$D/data/voices/$VN/24000/"
  cp "$V"/variants/* "$D/data/variants/$VN/"
  cp "$V/variants/base.data" "$D/data/voices/$VN/24000/voice.data"
done
mkdir -p "$D/data/voices/SLT"; cp -r "$T/rhvoice-slt/." "$D/data/voices/SLT/"
sed -i 's/^gain=.*/gain=0.5/' "$D/data/voices/SLT/voice.params"
grep -q '^gain=' "$D/data/voices/SLT/voice.params" || echo "gain=0.5" >> "$D/data/voices/SLT/voice.params"
printf 'clarity=50\nbreathiness=b0\nsharpening=0\nrepair=0\n' > "$D/data/quality.txt"

# Arabic text pipeline: ClaritySynth modules (Riad Assoum) with Fasih's patches, PBT components
cp "$T"/claritysynth/*.py "$D/arabic/"; cp -r "$T/claritysynth/lib_pure" "$D/arabic/"
cp "$REPO/scripts/ar_prep.py" "$D/arabic/"; touch "$D/arabic/__init__.py"
python3 "$HERE/patch_tashkeel.py" "$D/arabic/ar_tashkeel.py" "$HERE/rawi_client.py"
python3 "$HERE/patch_g2p.py" "$D/arabic/ar_g2p.py"
cp -r "$HERE/pbt" "$D/pbt"; touch "$D/pbt/__init__.py"

# Rawi diacritizer: separate helper process + ONNX Runtime + model
cp "$HERE/rawi_host/rawi_host.exe" "$D/arabic/lib/"
cp "$T"/onnxruntime/* "$D/arabic/lib/onnxruntime/capi/"
cp "$T/rawi/rawi_ensemble.onnx" "$T/rawi/vocab.json" "$D/arabic/lib/vowelizers/"

cp "$REPO/README.md" "$B/"
find "$B" -name "__pycache__" -prune -exec rm -rf {} +
VOICES=$(ls "$REPO/voices" | paste -sd, - | sed 's/,/, /g')
cat > "$B/manifest.ini" <<EOF
name = fasih
summary = "Fasih (فصيح): Arabic RHVoice voices"
description = """Fasih (فصيح): Arabic voices for the RHVoice engine ($VOICES). Non-neural speech synthesis (HTS models, WORLD vocoder) with neural diacritization (Rawi)."""
author = "Riad Assoum"
url = https://github.com/riadassoum/Fasih
version = $VER
minimumNVDAVersion = 2026.1
lastTestedNVDAVersion = 2026.3
docFileName = readme.html
EOF
rm -f "$OUT"; (cd "$B" && zip -qr "$OUT" .)
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); print(len(z.namelist()),'files', z.testzip() or 'zip OK')" "$OUT"
ls -la "$OUT"
