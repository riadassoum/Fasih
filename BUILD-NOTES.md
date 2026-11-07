# Build notes

## Layout (/home/claude/rhv)
- corpus/: ClArTTS 9500 utts, 11.16 h, 24 kHz wav; text.tsv = id \t dur \t diacritized text
- lang/: our own Arabic data-only RHVoice language. gen_foma.py + gen_rest.py generate src/*.foma;
  build_lang.sh -> lang/Arabic (needs foma at /usr/local/bin, RHVoice src at /tmp/rh for att2bin/dtree2bin,
  /tmp/alb/labelling.xml as template). Phones: b t th j H x d dh r z s sh S D T Z E g f q k l m n h w y Q
  (+ v p G ch loans), vowels a i u aa ii uu, pau. Stress (Classical rule) in syl.foma via "_stress".
- scripts/ar_prep.py: normalize (compose hamza/madda marks) + mark_wasl (mid-phrase bare alif -> U+0671).
  Used IDENTICALLY in training labels and in the NVDA add-on.
- scripts/feats.py: WORLD harvest/cheaptrick/d4c -> mgc(31, a=.466), lf0, bap(7 bands, dB). Audio x32768 (int16 units).
  --bpf writes our own 7-band filter bank (voice/bpf.txt). Validated with tools/vocode (RHVoice HTS vocoder): RMS ratio 1.05.
- Voicing fix: frame voiced only if harvest>0 AND bap band0 < -10 dB (applied in train.py stats).
- scripts/align.py: monophone 5-state Viterbi training (numba). init / iter / iter --pauses / final --pauses
  -> work/align/*.npy (unit,state,start,end), work/mono/*.lab
- scripts/full_labels.py: RHVoice-make-hts-labels -l work/mono -> work/full/*.lab (pauses rephrased)
- scripts/train.py stats | trees [mdl] | write OUT/voice.data  (MDL decision trees, grouped-sum split search)
- Voice dir: data/voices/<Name>/{voice.info, voice.params, 24000/{voice.data,bpf.txt}}
- Test synth: RHVOICE_DATA_PATH=/home/claude/rhv/data RHVOICE_CONFIG_PATH=/tmp/labt /tmp/rhb2/src/test/RHVoice-test -p <Name> -o out.wav
- Linux RHVoice build: /tmp/rhb2 (boost include flags; tclap patched for C++20; data subdir disabled)
- Windows DLLs: /tmp/rhwin via zig (tools/zig/z*), flags -D_LIBCPP_ENABLE_CXX17_REMOVED_* -D_LIBCPP_ENABLE_CXX20_REMOVED_*
- Add-on: addon_src/build_addon.sh VOICE_DIR VERSION OUT.nvda-addon (forks official driver, adds arabic_pipeline:
  ClaritySynth ar_textnorm + ar_tashkeel(Rawi) + ar_g2p merge + ar_prep). ClaritySynth clone: /home/claude/riadassoum/claritysynth

## Status
- Smoke test voice (744 utts) trained, RHVoice loads + speaks it (data/voices/Riad-test).
- Full feature extraction running (two feats.py processes, resumable: rerun same command ranges 0-4750, 4750-9500).

## Next
1. when feats done: align init, iter x4, iter --pauses x2, final --pauses, full_labels, train stats, trees, write
2. synth tests, then build add-on with final voice name
3. later: CD re-alignment pass; tune MDL/min_occ; lexicon (هذا/ذلك/لكن/هؤلاء dagger alifs); English fallback
