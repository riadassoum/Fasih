#!/bin/bash
# Assemble the NVDA add-on.  usage: build_addon.sh VOICE_DIR VERSION OUT.nvda-addon
set -e
VOICE=$(cd "$1" && pwd); VER=$2; OUT=$(realpath -m "$3")
HERE=$(cd "$(dirname "$0")" && pwd); RHV=$(cd "$HERE/.." && pwd)
CS=/home/claude/salvage/cs/synthDrivers/claritysynth   # ClaritySynth 3.0 (Riad Assoum)
B=$RHV/build/addon; D=$B/synthDrivers/fasih
rm -rf "$RHV/build/addon"; mkdir -p "$D/lib/x64" "$D/data/languages" "$D/data/voices" "$D/arabic/lib/vowelizers"
python3 "$HERE/patch_driver.py" /tmp/rh/src/nvda-addon/synthDriver/__init__.py "$D/__init__.py"
cp "$HERE/arabic_pipeline.py" "$HERE/loudness.py" "$RHV/scripts/fr_prep.py" "$D/"
cp -r "$HERE/locale" "$B/"
mkdir -p "$B/globalPlugins"; cp "$HERE/globalPlugins/fasihVoices.py" "$B/globalPlugins/"
cp -r "$HERE/doc" "$B/"
cp /tmp/rhwin/src/lib/libRHVoice.dll "$D/lib/x64/RHVoice.dll"
cp /tmp/rhwin/src/core/libRHVoice_core.dll "$D/lib/x64/"
cp -r "$RHV/lang/Arabic" "$D/data/languages/"
# French (data-only language built by lang_fr/build_lang.sh; lexicon from ipa-dict, MIT)
[ -d "$RHV/lang_fr/French" ] && cp -r "$RHV/lang_fr/French" "$D/data/languages/"
FRENCH_VOICES=" Elise "
# VOICES="Name=project_root=train_dir=mgc_dir=mgc_dim ..." ; default = the single VOICE given
if [ -z "$VOICES" ]; then VOICES="$(basename "$VOICE")=$RHV=${RHV_TRAIN_DIR:-train}=${RHV_MGC_DIR:-feat}=${RHV_MGC_DIM:-31}"; fi
for spec in $VOICES; do
  IFS== read -r VN VROOT VTR VMG VDIM VLVL <<< "$spec"; VLVL=${VLVL:-0}; VGAIN=0.7
  mkdir -p "$D/data/voices/$VN/24000" "$D/data/variants/$VN"
  cp "$RHV/voice/bpf.txt" "$D/data/voices/$VN/24000/"
  VLANG=Arabic; case "$FRENCH_VOICES" in *" $VN "*) VLANG=French;; esac
  printf 'name=%s\nlanguage=%s\ngender=%s\nformat=4\nrevision=1\n' "$VN" "$VLANG" "$( [ "$VN" = Nour -o "$VN" = Huda -o "$VN" = Rima -o "$VN" = Lamees -o "$VLANG" = French ] && echo female || echo male )" > "$D/data/voices/$VN/voice.info"
  # One base file per voice, plus small patches for the Breathiness levels and for Sharpening
  # (the settings only change some numbers inside the file, never its size or layout).
  # "Consonant repair" (a setting, off by default): the edges of voiceless consonants rebuilt from
  # their noisy frames (Faris, Huda), a crisper ق (Huda), the final hamza as a short closure, and
  # lows turned down in unvoiced frames by the vocoder. Off is the sound before those changes.
  PAU=1; RP=""; LF=""; VW=${VSCALE_W:-0.7}
  # Zayd: spectral detail restored to his recordings' level (chosen by ear)
  [ "$VN" = Zayd ] && VW=1.0
  # the hamza as a brief closure (not a croak or a stutter): Karim and Rima have it always
  [ "$VN" = Karim -o "$VN" = Rima ] && LF="RHV_PHONE_C0=Q:1-3:-3 RHV_PAU_SILENT=1"
  # Lamees (pitch re-extracted 120-600 Hz, model train2v6): a light trim of her very high peaks
  [ "$VN" = Lamees ] && LF="RHV_LF0_COMPRESS=0.8 RHV_PAU_SILENT=1"
  # pauses made silent (a hum learned from the sound before them was heard as a «w»)
  [ "$VN" = Zayd ] && LF="RHV_PAU_SILENT=1"
  [ "$VN" = Huda ] && { PAU=0.35; RP="RHV_STOP_RELEASE=q RHV_FIX_VOICELESS=1"; }
  [ "$VN" = Faris -o "$VN" = Karim -o "$VN" = Rima ] && { PAU=0.62; RP="RHV_FIX_VOICELESS=1"; }
  # speaking rate and tone matched across the voices (measured on the same sentences)
  case "$VN" in Jamal) DS=1.0;; Nour) DS=0.818;; Sami) DS=0.708;; Faris) DS=0.946;; Huda) DS=1.003;; Karim) DS=1.03;;
    Rima) DS=1.038;; Lamees) DS=1.141;; Zayd) DS=0.924;; Riad) DS=1.187;; *) DS=1.0;; esac
  LF="$LF RHV_DUR_SCALE=$DS"; [ "$VLANG" = Arabic ] && LF="$LF RHV_ALLAH=1" || LF="$LF RHV_LANG=$VLANG"; [ -s "$HERE/eq/$VN.npy" ] && LF="$LF RHV_EQ_MC=$HERE/eq/$VN.npy"
  VS=""; [ -s "$VROOT/work/$VTR/vratio.npy" ] && VS="$VROOT/work/$VTR/vratio.npy"
  W="$D/data/variants/$VN"; TMPV=$(mktemp -d)
  wr() { (cd "$VROOT" && env $LF $3 RHV_PAU_SCALE=$PAU RHV_TRAIN_DIR=$VTR RHV_MGC_DIR=$VMG RHV_MGC_DIM=$VDIM RHV_VOCODER=world RHV_DUR_PCT=97 RHV_LEVEL_DB=$VLVL RHV_BAP_OFFSET_DB=$1 RHV_VSCALE=$VS RHV_VSCALE_W=$2 python3 scripts/train.py write "$4" >/dev/null); }
  wr 0 $VW "" "$W/base.data"
  for n in 6 12 20; do wr -$n $VW "" "$TMPV/b$n.data"; python3 "$HERE/mkpatch.py" "$W/base.data" "$TMPV/b$n.data" "$W/b$n.patch"; done
  # Sharpening variants double the add-on's size: built only with FASIH_SHARP=1 (they stay in the source)
  if [ -n "$VS" ] && [ -n "$FASIH_SHARP" ]; then wr 0 ${SHARP_W:-1.4} "" "$TMPV/sharp.data"; python3 "$HERE/mkpatch.py" "$W/base.data" "$TMPV/sharp.data" "$W/sharp.patch"; fi
  wr 0 $VW "$RP RHV_PHONE_C0=Q:1-3:-3 RHV_SOFT_UNVOICED=1" "$TMPV/repair.data"; python3 "$HERE/mkpatch.py" "$W/base.data" "$TMPV/repair.data" "$W/repair.patch"
  rm -rf "$TMPV"
  echo "$VGAIN" > "$W/gain.txt"
  cp "$W/base.data" "$D/data/voices/$VN/24000/voice.data"
  printf 'beta=0.300\ngain=%s\n' "$VGAIN" > "$D/data/voices/$VN/voice.params"
done
# voices delivered already built (PlanetBlindTech's Reem and Aseel, trained with Fasih's recipe):
# PREBUILT="Name=src_voice_dir=gender=rate_scale=level_db ..." ; matched in rate, level and tone
for spec in $PREBUILT; do
  IFS== read -r VN VSRC VGEN DS VLVL <<< "$spec"
  mkdir -p "$D/data/voices/$VN/24000" "$D/data/variants/$VN"
  cp "$VSRC/24000/bpf.txt" "$D/data/voices/$VN/24000/"
  printf 'name=%s\nlanguage=Arabic\ngender=%s\nformat=4\nrevision=1\n' "$VN" "$VGEN" > "$D/data/voices/$VN/voice.info"
  EQA=""; [ -s "$HERE/eq/$VN.npy" ] && EQA="$HERE/eq/$VN.npy"
  python3 "$RHV/tools/prebuilt.py" "$VSRC" "$D/data/variants/$VN" "$DS" "$VLVL" $EQA >/dev/null
  echo 0.7 > "$D/data/variants/$VN/gain.txt"
  cp "$D/data/variants/$VN/base.data" "$D/data/voices/$VN/24000/voice.data"
  printf 'beta=0.300\ngain=0.7\n' > "$D/data/voices/$VN/voice.params"
done
printf 'clarity=50\nbreathiness=b0\nsharpening=0\nrepair=0\n' > "$D/data/quality.txt"
# English (RHVoice language data, LGPL) + Bdl voice (CMU license), 24 kHz only
mkdir -p "$D/data/languages/English" "$D/data/voices/Bdl"
cp /tmp/rh_English/* "$D/data/languages/English/" 2>/dev/null || true
cp /tmp/rh_bdl-eng/voice.info /tmp/rh_bdl-eng/voice.params /tmp/rh_bdl-eng/README.md "$D/data/voices/Bdl/"
cp -r /tmp/rh_bdl-eng/24000 "$D/data/voices/Bdl/"
# loudness matched to the Arabic voices (RHVoice's lowest allowed gain is 0.5)
sed -i 's/^gain=.*/gain=0.5/' "$D/data/voices/Bdl/voice.params"; grep -q '^gain=' "$D/data/voices/Bdl/voice.params" || echo "gain=0.5" >> "$D/data/voices/Bdl/voice.params"
# Arabic text pipeline (from ClaritySynth, by Riad Assoum) + our wasl marker
for f in ar_tashkeel.py ar_g2p.py ar_textnorm.py _libboot.py data_paths.py ort_config.py thread_calibration.py; do cp "$CS/$f" "$D/arabic/"; done
cp -r "$CS/lib_pure" "$D/arabic/"
cp "$RHV/scripts/ar_prep.py" "$D/arabic/"
python3 "$HERE/patch_tashkeel.py" "$D/arabic/ar_tashkeel.py" "$HERE/rawi_client.py"
python3 "$HERE/patch_g2p.py" "$D/arabic/ar_g2p.py"
# PlanetBlindTech components (GPL-3.0): dictionaries, text fixes, audio post-filter
cp -r "$HERE/pbt" "$D/pbt"; touch "$D/pbt/__init__.py"
cp "$RHV/README.md" "$B/" 2>/dev/null || true
touch "$D/arabic/__init__.py"
# Rawi runs in its own process (rawi_host.exe + onnxruntime.dll); NVDA loads no numpy/onnxruntime
mkdir -p "$D/arabic/lib/onnxruntime/capi"
cp "$CS/lib/onnxruntime/capi/onnxruntime.dll" "$CS/lib/onnxruntime/capi/onnxruntime_providers_shared.dll" "$CS/lib/onnxruntime/capi/msvcp140.dll" "$CS/lib/onnxruntime/LICENSE" "$D/arabic/lib/onnxruntime/capi/"
cp "$HERE/rawi_host/rawi_host.exe" "$D/arabic/lib/"
cp "$HERE/../third_party/rawi/rawi_fasih.onnx" "$CS/lib/vowelizers/vocab.json" "$D/arabic/lib/vowelizers/"
find "$B" -name "__pycache__" -prune -exec rm -rf {} +
cat > "$B/manifest.ini" <<EOF
name = fasih
summary = "Fasih (فصيح): Arabic RHVoice voices"
description = """Fasih (فصيح): non-neural Arabic voices for the RHVoice engine (Jamal, Nour, Sami, Faris, Huda), with Rawi diacritization from ClaritySynth and the WORLD vocoder."""
author = "Riad Assoum <riad.assoum@gmail.com>"
url = https://github.com/riadassoum/Fasih
version = $VER
minimumNVDAVersion = 2026.1
lastTestedNVDAVersion = 2026.3
docFileName = readme.html
EOF
rm -f "$OUT"; (cd "$B" && zip -qr "$OUT" .)
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); print(len(z.namelist()),'files', z.testzip() or 'zip OK')" "$OUT"
ls -la "$OUT"
