# Fasih (فصيح)

Fasih is a set of Arabic voices for NVDA, built on the RHVoice engine: non-neural speech synthesis (statistical HTS models and the WORLD vocoder) with neural diacritization (Rawi). I made it because I wanted Arabic speech that is clear, responsive and light on the computer, without a neural model generating every sound and without relying on Festival. Everything is open source: the voices, the Arabic language rules for RHVoice, the training scripts, the engine changes and the add-on itself are all in this repository.

Developer: Riad Assoum ([github.com/riadassoum](https://github.com/riadassoum)).

## The voices

- **Jamal**: a male voice in Classical Arabic, trained on the ClArTTS corpus (MBZUAI), about 11 hours of speech.
- **Nour**: a female voice, trained on the female part of NeoBoy/arabic-tts-wav-24k (MIT), about 5.5 hours. That audio was itself produced by a neural TTS (Nourhann's Arabic-Diacritized-TTS / nipponjo tts_arabic).
- **Sami**: a male voice in Modern Standard Arabic with a Damascene colour, trained on Nawar Halabi's Arabic Speech Corpus (CC BY 4.0), about 4 hours.
- **Faris**: a male voice in Modern Standard Arabic, trained on about 4.8 hours of the synthetic part of ArVoice (MBZUAI, CC BY 4.0). That audio was produced by Google's ar-XA-Wavenet-B voice.
- **Huda**: a female voice in Modern Standard Arabic, trained the same way on about 4.7 hours of ArVoice's ar-XA-Wavenet-A audio.

Known limits: Jamal reads in a Classical style; Nour, Faris and Huda learned from synthetic speech, so they inherit its character; Rawi sometimes picks the wrong vowel on a word read alone (for example the passive حُبِسَتِ). Add your own harakat, or a user-dictionary entry, to correct a word.

## Arabic and English together

Each piece of text is split by script, the way I do it in ClaritySynth. Arabic goes to the Arabic voice you choose, and everything else goes to the secondary voice. By default the secondary voice is chosen automatically as the first non-Arabic RHVoice voice found, which is the bundled English voice SLT unless you have installed others. Voices installed with Kvark's RHVoice Voice Manager are picked up too. The Arabic and English voices are matched in loudness, so switching between them doesn't jump in volume, and the output is about as loud as other screen-reader voices: a look-ahead limiter lowers the gain on the loudest peaks instead of clipping them.

## Diacritization

Text is diacritized with Rawi before it is spoken, and any harakat you typed yourself are kept. Rawi runs in its own small helper program (rawi_host.exe), so NVDA itself never loads numpy or onnxruntime. Many add-ons ship their own copies of those libraries, and loading two copies into NVDA can freeze it. If the helper gets stuck, NVDA waits for it at most three seconds the first time and one second after that; the helper is then restarted and the sentence is spoken without the added diacritics. Speech starts on the first finished part of an utterance while the rest is still being prepared, and pressing Control stops the preparation as well as the sound.

## Settings

These are in NVDA's voice settings. The secondary voice, Clarity, Breathiness, Sharpening and Mixed text are also in the settings ring.

- **Secondary voice**: which voice reads non-Arabic text (Automatic, None, or any installed voice).
- **Clarity**: how strongly the voice is sharpened by the postfilter.
- **Breathiness**: from natural down to very low breath noise.
- **Sharpening**: a crisper, more detailed spectrum.
- **Read emojis and symbols by name**.
- **Pausal ending (waqf)**: the last word of a sentence is read in its pausal form.
- **Mixed Arabic and Latin text**: Automatic (Latin text goes to the secondary voice exactly as written, with a number just before a Latin word, as in 5 PM), Arabic first (numbers, symbols and Roman numerals read in Arabic, as in 1.0), or Secondary voice first (numbers too).

## User dictionary

Put a file called `user-dic.json` in `%APPDATA%\nvda\Fasih-config\`. It is a list of `{"original": "...", "replacement": "..."}` entries, and the replacement may be fully diacritized. Restart NVDA or select the synthesizer again to apply it.

## How it is made

- **Language**: I wrote a data-only Arabic language for RHVoice with foma rules. It covers shadda, sun and moon letters, hamzat al-wasl in connected speech, tanween, ta marbuta, long vowels including the hidden ones, Classical stress and word classes.
- **Training**: the voices were trained with my own HTS-style trainer, without HTK. It uses WORLD analysis, 5-state alignment with pauses, MDL decision trees and re-alignment with context-dependent models, with 40 mel-cepstral coefficients. The averaging that makes HMM voices sound muffled is countered by variance scaling: the spectrum is moved away from the voice's average by a measured amount per coefficient, which cannot click the way global variance did.
- **Engine**: RHVoice is built from source with the WORLD vocoder (Masanori Morise, BSD) added for these voices, so the breath noise is generated pulse by pulse, as in a real voice.

## Credits

- **ClaritySynth 3.0** (my own add-on): Arabic text normalization for numbers, dates and currency, Rawi diacritization, and keeping the user's own harakat.
- **PlanetBlindTech** (rhvoice-arabic, GPL-3.0), with their blessing: the default pronunciation dictionary, the user dictionary, emoji and symbol names, single-character names, presentation-form cleanup, post-diacritization fixes and the design of the audio post-filter (the DC/rumble high-pass Fasih still uses; its limiter is now a look-ahead gain limiter of mine). Their project also showed me the data-only language approach and the stock-RHVoice packaging that Fasih follows. Thank you.
- **RHVoice**: Olga Yakovleva and contributors (LGPL). **WORLD**: Masanori Morise (BSD). **foma**: Mans Hulden. **ONNX Runtime**: Microsoft (MIT).
- **Corpora**: ClArTTS (MBZUAI); the Arabic Speech Corpus (Nawar Halabi, CC BY 4.0); NeoBoy/arabic-tts-wav-24k (MIT); ArVoice (Toyin, Marew, Alblooshi, Magdy and Aldarmaki, MBZUAI, Interspeech 2025, CC BY 4.0).

## What is in this repository

- `voices/`: the five trained voices, ready for RHVoice. Each has its `voice.data` and, in `variants/`, the base file with small patches used by the Breathiness and Sharpening settings.
- `lang/`: the Arabic language for RHVoice. `build_lang.sh` regenerates it from the generators (it needs foma and RHVoice's att2bin and dtree2bin).
- `scripts/`: corpus download, WORLD feature extraction, alignment, labels and training (`run_voice.sh`). The corpora themselves are not included; the scripts download them from their original sources.
- `engine/`: `rhvoice-world-vocoder.patch` for RHVoice (at the commit in `RHVOICE_BASE_COMMIT`) and the WORLD sources to copy into `src/core/world/`. I cross-built the Windows DLLs with zig.
- `addon_src/`: the NVDA driver patches, the Arabic text pipeline and the Rawi helper (`rawi_host/`, C source and Windows build). `build_release.sh VERSION` builds the add-on from a clean checkout of this repository with nothing else installed but bash, python3 and zip. `build_addon.sh` is the development build, which writes the voices from training output.
- `third_party/`: what the release build packages from other projects: RHVoice's NVDA driver and Windows libraries (built from source with the WORLD patch), RHVoice's English language and the SLT voice, ClaritySynth's Arabic modules, ONNX Runtime and the Rawi model.
- `tests/test_arabic.py`: Arabic regression tests (waqf, hamzat al-wasl, helping vowels, merging your own harakat, script splitting, and the full pipeline when Rawi is available).

See [CHANGELOG.md](CHANGELOG.md) for what changed in each version. The add-on is available in English and Arabic.

## Requirements

NVDA 2026.1 or later (64-bit). Fasih ships only 64-bit libraries.

## Install

Download `Fasih-<version>.nvda-addon` from the Releases page and open it with NVDA.
