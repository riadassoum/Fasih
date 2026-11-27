#!/bin/bash
# Build the NVDA add-on from this repository alone (a clean checkout is enough).
# usage: addon_src/build_release.sh VERSION [OUT.nvda-addon]
# Needs only bash, python3 and zip. Retraining the voices is a separate process (see BUILD-NOTES.md).
set -euo pipefail
VER=${1:?version}
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${2:-$REPO/Fasih-$VER.nvda-addon}")
mkdir -p "$(dirname "$OUT")"
HERE=$REPO/addon_src; T=$REPO/third_party
B=$(mktemp -d); D=$B/synthDrivers/fasih
trap 'rm -rf "$B"' EXIT
mkdir -p "$D/lib/x64" "$D/data/languages" "$D/data/voices" "$D/data/variants" "$D/arabic/lib/vowelizers" "$D/arabic/lib/onnxruntime/capi"

# driver: RHVoice's official NVDA driver, patched for Fasih
cp "$HERE/synthDrivers/fasih/__init__.py" "$D/__init__.py"
cp "$HERE/arabic_pipeline.py" "$HERE/loudness.py" "$REPO/scripts/fr_prep.py" "$D/"
cp -r "$HERE/locale" "$B/"
# Tools > Fasih > Download voices (catalogue of RHVoice Voice Manager, by Alexander Linkov)
mkdir -p "$B/globalPlugins"; cp "$HERE/globalPlugins/fasihVoices.py" "$B/globalPlugins/"
mkdir -p "$B/samples"; cp "$HERE"/samples/*.wav "$B/samples/"   # Listen, in Tools > Fasih > Download voices
mkdir -p "$B/doc/en" "$B/doc/ar"         # help pages generated from the READMEs, so they never fall behind
python3 "$REPO/tools/md2html.py" "$REPO/README.md" "$B/doc/en/readme.html" en
python3 "$REPO/tools/md2html.py" "$HERE/doc/ar/readme.md" "$B/doc/ar/readme.html" ar
cp "$T/rhvoice-win-x64/RHVoice.dll" "$T/rhvoice-win-x64/libRHVoice_core.dll" "$D/lib/x64/"

# languages and voices
cp -r "$REPO/lang/Arabic" "$D/data/languages/"
cp -r "$REPO/lang_fr/French" "$D/data/languages/"
mkdir -p "$D/data/languages/English"; cp -r "$T/rhvoice-english/." "$D/data/languages/English/"
# the add-on carries the core voices; the others are voice packs (tools/make_packs.py) that
# Tools > Fasih > Download voices installs. FASIH_ALL=1 puts every voice in the add-on.
CORE=${FASIH_CORE:-Riad Aseel Elise}
for V in "$REPO"/voices/*/; do
  VN=$(basename "$V")
  if [ -z "${FASIH_ALL:-}" ]; then case " $CORE " in *" $VN "*) ;; *) continue;; esac; fi
  mkdir -p "$D/data/voices/$VN/24000" "$D/data/variants/$VN"
  cp "$V/voice.info" "$V/voice.params" "$D/data/voices/$VN/"
  cp "$V/24000/bpf.txt" "$D/data/voices/$VN/24000/"
  cp "$V"/variants/* "$D/data/variants/$VN/"
  # Sharpening variants double the add-on's size: left out unless FASIH_SHARP=1 (they stay in the repository)
  [ -n "${FASIH_SHARP:-}" ] || rm -f "$D/data/variants/$VN/sharp.patch"
  cp "$V/variants/base.data" "$D/data/voices/$VN/24000/voice.data"
  # modulation postfilter, on by default (the Modulation postfilter setting removes it)
  [ -s "$V/variants/ms_gain.txt" ] && cp "$V/variants/ms_gain.txt" "$D/data/voices/$VN/24000/"
done
mkdir -p "$D/data/voices/Bdl"; cp -r "$T/rhvoice-bdl/." "$D/data/voices/Bdl/"
sed -i 's/^gain=.*/gain=0.5/' "$D/data/voices/Bdl/voice.params"
grep -q '^gain=' "$D/data/voices/Bdl/voice.params" || echo "gain=0.5" >> "$D/data/voices/Bdl/voice.params"
printf 'clarity=50\nbreathiness=b0\nsharpening=0\nrepair=0\nms=1\n' > "$D/data/quality.txt"

# Arabic text pipeline: ClaritySynth modules (Riad Assoum) with Fasih's patches, PBT components
cp "$T"/claritysynth/*.py "$D/arabic/"; cp -r "$T/claritysynth/lib_pure" "$D/arabic/"
cp "$REPO/scripts/ar_prep.py" "$D/arabic/"; touch "$D/arabic/__init__.py"
cp "$HERE"/arabic/*.py "$D/arabic/"      # Fasih's versions of ClaritySynth's diacritizer and letter-to-sound modules
cp -r "$HERE/pbt" "$D/pbt"; touch "$D/pbt/__init__.py"

# Rawi diacritizer: separate helper process + ONNX Runtime + model
cp "$HERE/rawi_host/rawi_host.exe" "$D/arabic/lib/"
# second diacritizer: Barq, PlanetBlindTech's tashkeel engine (in process, one DLL + one model)
mkdir -p "$D/arabic/lib/tashkeel_engine"
cp "$T/tashkeel_engine/bin/tashkeel_engine.dll" "$T/tashkeel_engine/models/tashkeel_engine.tash" "$D/arabic/lib/tashkeel_engine/"
cp "$T"/onnxruntime/* "$D/arabic/lib/onnxruntime/capi/"
# the neural vocoder for premium voices is not bundled: it is its own add-on, downloaded with the
# first premium voice (tools/make_packs.py builds it as Fasih-vocoder.zip)
cp "$T/rawi/rawi_fasih.onnx" "$T/rawi/vocab.json" "$D/arabic/lib/vowelizers/"

cp "$REPO/README.md" "$B/"
find "$B" -name "__pycache__" -prune -exec rm -rf {} +
VOICES=$(for v in "$D"/data/voices/*/; do if grep -q '^language=Arabic' "$v/voice.info"; then basename "$v"; fi; done | paste -sd, - | sed 's/,/, /g')
cat > "$B/manifest.ini" <<EOF
name = fasih
summary = "Fasih (فصيح): Arabic RHVoice voices"
description = """Fasih (فصيح): Arabic voices for the RHVoice engine ($VOICES), plus Elise, the first French RHVoice voice, as a secondary voice; more Arabic voices can be downloaded from Tools > Fasih > Download voices: compact voices, and premium concatenative voices made of real recorded speech, smoothed by a neural vocoder. Compact voices use non-neural synthesis (HTS models, WORLD vocoder); diacritization is neural (Rawi), or Barq, PlanetBlindTech's much faster diacritizer."""
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
