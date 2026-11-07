# Fasih (فصيح)

Fasih is a set of non-neural Arabic voices for NVDA, built on the RHVoice engine. I made it because I wanted Arabic speech that is clear, responsive and light on the computer, without a neural model behind every word and without relying on Festival. Everything is open source: the voices, the Arabic language rules for RHVoice, the training scripts, the engine changes and the add-on itself are all in this repository.

Developer: Riad Assoum ([github.com/riadassoum](https://github.com/riadassoum)).

## The voices

- **Jamal**: a male voice in Classical Arabic, trained on the ClArTTS corpus (MBZUAI), about 11 hours of speech.
- **Nour**: a female voice, trained on the female part of NeoBoy/arabic-tts-wav-24k (MIT), about 5.5 hours. That audio was itself produced by a neural TTS (Nourhann's Arabic-Diacritized-TTS / nipponjo tts_arabic).
- **Sami**: a male voice in Modern Standard Arabic with a Damascene colour, trained on Nawar Halabi's Arabic Speech Corpus (CC BY 4.0), about 4 hours.

## Arabic and English together

Each piece of text is split by script, the way I do it in ClaritySynth. Arabic goes to the Arabic voice you choose, and everything else goes to the secondary voice. By default the secondary voice is chosen automatically as the first non-Arabic RHVoice voice found, which is the bundled English voice SLT unless you have installed others. Voices installed with Kvark's RHVoice Voice Manager are picked up too. The Arabic and English voices are matched in loudness, so switching between them doesn't jump in volume.

## Diacritization

Text is diacritized with Rawi before it is spoken, and any harakat you typed yourself are kept. Rawi runs in its own small helper program (rawi_host.exe), so NVDA itself never loads numpy or onnxruntime. Many add-ons ship their own copies of those libraries, and loading two copies into NVDA can freeze it. If the helper ever gets stuck, it is restarted after three seconds and the sentence is still spoken, so speech never stops.

## Settings

These are in NVDA's voice settings. The secondary voice, Clarity, Breathiness and Sharpening are also in the settings ring.

- **Secondary voice**: which voice reads non-Arabic text (Automatic, None, or any installed voice).
- **Clarity**: how strongly the voice is sharpened by the postfilter.
- **Breathiness**: from natural down to very low breath noise.
- **Sharpening**: a crisper spectrum, which may click on some syllables.
- **Read emojis and symbols by name**.
- **Pausal ending (waqf)**: the last word of a sentence is read in its pausal form.

## User dictionary

Put a file called `user-dic.json` in `%APPDATA%\nvda\Fasih-config\`. It is a list of `{"original": "...", "replacement": "..."}` entries, and the replacement may be fully diacritized. Restart NVDA or select the synthesizer again to apply it.

## How it is made

- **Language**: I wrote a data-only Arabic language for RHVoice with foma rules. It covers shadda, sun and moon letters, hamzat al-wasl in connected speech, tanween, ta marbuta, long vowels including the hidden ones, Classical stress and word classes.
- **Training**: the voices were trained with my own HTS-style trainer, without HTK. It uses WORLD analysis, 5-state alignment with pauses, MDL decision trees and re-alignment with context-dependent models, with 40 mel-cepstral coefficients.
- **Engine**: RHVoice is built from source with the WORLD vocoder (Masanori Morise, BSD) added for these voices, so the breath noise is generated pulse by pulse, as in a real voice.

## Credits

- **ClaritySynth 3.0** (my own add-on): Arabic text normalization for numbers, dates and currency, Rawi diacritization, and keeping the user's own harakat.
- **PlanetBlindTech** (rhvoice-arabic, GPL-3.0), with their blessing: the default pronunciation dictionary, the user dictionary, emoji and symbol names, single-character names, presentation-form cleanup, post-diacritization fixes and the real-time audio post-filter (DC/rumble high-pass and soft limiter). Their project also showed me the data-only language approach and the stock-RHVoice packaging that Fasih follows. Thank you.
- **RHVoice**: Olga Yakovleva and contributors (LGPL). **WORLD**: Masanori Morise (BSD). **foma**: Mans Hulden. **ONNX Runtime**: Microsoft (MIT).
- **Corpora**: ClArTTS (MBZUAI); the Arabic Speech Corpus (Nawar Halabi, CC BY 4.0); NeoBoy/arabic-tts-wav-24k (MIT).

## What is in this repository

- `voices/`: the three trained voices, ready for RHVoice. Each has its `voice.data` and, in `variants/`, the versions used by the Breathiness setting.
- `lang/`: the Arabic language for RHVoice. `build_lang.sh` regenerates it from the generators (it needs foma and RHVoice's att2bin and dtree2bin).
- `scripts/`: corpus download, WORLD feature extraction, alignment, labels and training (`run_voice.sh`). The corpora themselves are not included; the scripts download them from their original sources.
- `engine/`: `rhvoice-world-vocoder.patch` for RHVoice (at the commit in `RHVOICE_BASE_COMMIT`) and the WORLD sources to copy into `src/core/world/`. I cross-built the Windows DLLs with zig.
- `addon_src/`: the NVDA driver, the Arabic text pipeline, the Rawi helper (`rawi_host/`, C source and Windows build) and `build_addon.sh`, which assembles the add-on.

## Install

Download `Fasih-<version>.nvda-addon` from the Releases page and open it with NVDA.
