# How each voice is written

Every voice is trained with `tools/new_voice.sh` and then written from its training output with `scripts/train.py write OUT/voice.data`. The options are environment variables. These are the values behind the voices in `voices/`, so a voice can be rewritten exactly (for example to change one setting).

## Common to every Arabic voice

`RHV_TRAIN_DIR=train2v5 RHV_MGC_DIR=feat2 RHV_MGC_DIM=40 RHV_VOCODER=world RHV_DUR_PCT=97 RHV_ALLAH=1 RHV_PAU_SCALE=1 RHV_VSCALE=work/train2v5/vratio.npy RHV_VSCALE_W=0.7`, unless a line below says otherwise. Older voices (Jamal to Riad) used `train`/`feat`/31 dimensions; their training directories are named in their own build.

Variants, written from the same model and stored as patches of `base.data` (`addon_src/mkpatch.py`):

- `b6`, `b12`, `b20`: `RHV_BAP_OFFSET_DB=-6`, `-12`, `-20` (the Breathiness setting).
- `repair`: `RHV_PHONE_C0=Q:1-3:-3 RHV_SOFT_UNVOICED=1`, plus the voice's own repair options (Consonant repair).
- `sharp`: `RHV_VSCALE_W=1.4`, only when `FASIH_SHARP=1` (Sharpening; left out of releases to keep them small).
- `gain.txt`: 0.7. `ms_gain.txt`: `tools/ms_estimate.py VOICE DATA_DIR CORPUS_DIR OUT` (the modulation postfilter).

## Per voice

| Voice | Rate (`RHV_DUR_SCALE`) | Level dB | Other |
|---|---|---|---|
| Jamal | 1.0 | | |
| Nour | 0.818 | | tone filter `addon_src/eq/Nour.npy` |
| Sami | 0.708 | | tone filter |
| Faris | 0.946 | | `RHV_PAU_SCALE=0.62`, repair adds `RHV_FIX_VOICELESS=1`, tone filter |
| Huda | 1.003 | | `RHV_PAU_SCALE=0.35`, repair adds `RHV_STOP_RELEASE=q RHV_FIX_VOICELESS=1`, tone filter |
| Karim | 1.03 | | `RHV_PHONE_C0=Q:1-3:-3 RHV_PAU_SILENT=1`, `RHV_PAU_SCALE=0.62`, repair adds `RHV_FIX_VOICELESS=1` |
| Rima | 1.038 | | as Karim |
| Lamees | 1.141 | | `RHV_LF0_COMPRESS=0.8 RHV_PAU_SILENT=1` (model `train2v6`, pitch 120-600 Hz) |
| Zayd | 0.924 | | `RHV_VSCALE_W=1.0 RHV_PAU_SILENT=1`, tone filter |
| Riad | 1.187 | | tone filter; his pitch is never changed |
| Hadi | 1.035 | -10.6 | |
| Bassel | 1.0 | -4.1 | |
| Rana | 0.94 | -6.2 | |
| Haytham | 0.88 | 1.2 | `RHV_VSCALE_W=1.0`, tone filter `RHV_EQ_MC` from `tools/eq_match.py` against the male voices (±4 dB); 12 mismatched sentences left out of training |

Voices made from a finished RHVoice voice (Reem, Aseel, Aisha, Amjad, Shahrazad, Nabil, Evan, Elise) are normalised with `tools/prebuilt.py SRC OUT DUR LEVEL [EQ.npy]`.

Rates and levels are matched by synthesizing the same passage with every voice (`tools/eval_voice.py`): the passage takes about 31 seconds at about -26 dB RMS for the male voices.
