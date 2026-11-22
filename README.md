# Fasih (فصيح)

Fasih is a set of Arabic voices for NVDA, with a French voice as a companion, built on the RHVoice engine: non-neural speech synthesis (statistical HTS models and the WORLD vocoder) with neural diacritization (Rawi). I made it because I wanted Arabic speech that is clear, responsive and light on the computer, without a neural model generating every sound and without relying on Festival. Everything is open source: the voices, the Arabic and French language rules for RHVoice, the training scripts, the engine changes and the add-on itself are all in this repository.

Developer: Riad Assoum ([github.com/riadassoum](https://github.com/riadassoum)).

## The voices

The add-on comes with Riad and Aseel, the English voice Bdl and the French voice Elise. The other fifteen Arabic voices are voice packs: in NVDA's Tools menu, choose Fasih > Download voices, then "Arabic: Fasih voices", and install the ones you want (they become available after NVDA restarts). This keeps the add-on small.

All the Arabic voices speak at the same pace and the same loudness, and voices of the same gender have been brought close in bass and tone, so you can switch between them without readjusting NVDA.

- **Jamal**: a male voice in Classical Arabic, trained on the ClArTTS corpus (MBZUAI), about 11 hours of speech.
- **Nour**: a female voice, trained on the female part of NeoBoy/arabic-tts-wav-24k (MIT), about 5.5 hours. That audio was itself produced by a neural TTS (Nourhann's Arabic-Diacritized-TTS / nipponjo tts_arabic).
- **Sami**: a male voice in Modern Standard Arabic with a Damascene colour, trained on Nawar Halabi's Arabic Speech Corpus (CC BY 4.0), about 4 hours.
- **Faris**: a male voice in Modern Standard Arabic, trained on about 4.8 hours of the synthetic part of ArVoice (MBZUAI, CC BY 4.0). That audio was produced by Google's ar-XA-Wavenet-B voice.
- **Huda**: a female voice in Modern Standard Arabic, trained the same way on about 4.7 hours of ArVoice's ar-XA-Wavenet-A audio.
- **Karim**: a male voice, trained on about 4.8 hours of ArVoice's ar-XA-Wavenet-C audio.
- **Rima**: a female voice, trained on about 4.8 hours of ArVoice's ar-XA-Wavenet-D audio.
- **Lamees**: a young female voice, trained on about 4.7 hours I generated with Nabra-82M (oddadmix, Apache-2.0), an Arabic fine-tune of Kokoro-82M.
- **Zayd**: PlanetBlindTech's male voice. They asked me to adapt it for Fasih, so I retrained it with Fasih's recipe on their own corpus (10,023 clips, 12.5 hours) and matched its bass and loudness to the other voices.
- **Riad**: my own voice. I gave a clip of my voice as the reference, generated about 3.5 hours of it with SILMA TTS (Apache-2.0) and trained it like the others. I left its timbre and range exactly as they came out.
- **Reem**: a female voice trained by PlanetBlindTech with Fasih's recipe, on about 6.1 hours of synthetic speech from SILMA TTS.
- **Amjad**: a male voice in Modern Standard Arabic, trained on about 6.4 hours of the Arabic MSA 25K Saudi Male corpus (moesaif, CC BY 4.0). That audio was produced by Microsoft Azure's ar-SA-HamedNeural voice.
- **Evan** (English): a male English voice you can choose as Fasih's English voice instead of BDL, trained on 5.3 hours of speech from OpenEVV's Adult Male 1 (IBM's Embedded ViaVoice, the engine known as Eloquence). **Its voice data is IBM's, not covered by Fasih's licence**: it is a separate download for personal, non-commercial use, and voices/Evan/NOTICE.md explains why. It is less clear than BDL in my tests (8% of words misheard by speech recognition, against 3% for BDL).
- **Nabil**: a male voice, the clearest so far, trained on about 7.7 hours of sentences chosen for Fasih, read by the Piper voice Kareem mostly two to four at a time so that he stays steady through long passages, (ar_JO, by Michael Hansen for rhasspy, MIT), whose own training recordings come from github.com/AliMokhammad/arabicttstrain (Kamel Kilani's stories) with no stated license. For that reason Nabil is for non-commercial use only.
- **Aisha**: a female voice trained by PlanetBlindTech with Fasih's recipe and added at their request.
- **Hadi**: a male voice trained on speech cloned with OmniVoice (k2-fsa) from Yahya Alnwsany's Arabic professional voice dataset (NightPrince/Arabic-professional-voice, CC0).
- **Shahrazad**: a female storyteller's voice, trained on about 2.4 hours of Aïda Boumaza's LibriVox reading of Alf Layla wa Layla, Volume 1 (public domain), matched to the Hindawi edition of the text. A real human reader; because the recordings are an audiobook made at home, she is softer and less crisp than the studio voices.
- **Aseel**: a male voice trained by PlanetBlindTech with Fasih's recipe, on about 12 hours made from the voice of Mr. Aseel Bashir, with his consent.
- **Elise** (French): a young female voice, the first French voice for RHVoice, trained on the SIWIS French Speech Synthesis Database (Honnet, Lazaridis, Garner and Yamagishi, CC BY 4.0), about 9.2 hours of studio recordings. She is a secondary voice for French text.

Known limits: Jamal reads in a Classical style; the voices trained on synthetic speech inherit its character; Rawi sometimes picks the wrong vowel on a word read alone (for example the passive حُبِسَتِ). Add your own harakat, or a user-dictionary entry, to correct a word. Reem and Aseel came to me already built, so the correction of the heavy lam in الله could not reach them.

## Arabic with English or French

Each piece of text is split by script, the way I do it in ClaritySynth. Arabic goes to the Arabic voice you choose, and everything else goes to the secondary voice. By default the secondary voice is chosen automatically: a voice in NVDA's own language if there is one (Elise when NVDA is in French), otherwise an English voice (the bundled Bdl), otherwise the first non-Arabic RHVoice voice found. When a web page or document marks a passage with its language and NVDA's automatic language switching is on, that passage is read by an installed voice of that language. Choose Elise as the secondary voice to have Latin text read in French; Fasih then adds the liaisons (les‿amis, c'est‿un) and expands common abbreviations for her. Voices installed with Kvark's RHVoice Voice Manager are picked up too. The Arabic and English voices are matched in loudness, so switching between them doesn't jump in volume, and the output is about as loud as other screen-reader voices: a look-ahead limiter lowers the gain on the loudest peaks instead of clipping them.

## More voices

In NVDA's Tools menu, **Fasih > Download voices** lists the RHVoice voices of every language published for Alexander Linkov's (kvark128) RHVoice Voice Manager. Choose a language and a voice and press Install: a progress window shows the size, the speed and the time elapsed and remaining, and Cancel stops the download at once. Fasih's own Arabic voices are listed first, under "Arabic: Fasih voices": Listen plays a short sample of the selected voice before you download it, and when I publish a better version of a voice you already have, its status says "Update available" and the Install button becomes Update. The voice is installed as a standard RHVoice voice add-on, the same one the Voice Manager installs, and after NVDA restarts it can be chosen as Fasih's secondary voice (Automatic picks it when it is in NVDA's language). Remove uninstalls it.

## Diacritization

Text is diacritized with Rawi before it is spoken, and any harakat you typed yourself are kept. Rawi runs in its own small helper program (rawi_host.exe), so NVDA itself never loads numpy or onnxruntime. Many add-ons ship their own copies of those libraries, and loading two copies into NVDA can freeze it. If the helper gets stuck, NVDA waits for it at most three seconds the first time and one second after that; the helper is then restarted and the sentence is spoken without the added diacritics. The silence the voices put before their first sound (about 40 to 50 ms) is trimmed to 5 ms, so speech starts that much sooner after a key press; "Start speaking without leading silence" in the voice settings turns this off. A second diacritizer, Barq (برق), PlanetBlindTech's tashkeel engine, can be chosen instead under Diacritizer in NVDA's voice settings or in the synth settings ring. It runs inside NVDA as a small library with no numpy or onnxruntime and is many times faster than Rawi. In my tests it makes fewer mistakes than Rawi on modern sentences, news and classical text; Rawi still does better on its own kind of text, and stays the default for now. Speech starts on the first finished part of an utterance while the rest is still being prepared, and pressing Control stops the preparation as well as the sound.

## Settings

These are in NVDA's voice settings. The secondary voice, Clarity, Breathiness, Sharpening, Consonant repair and Mixed text are also in the settings ring.

- **Secondary voice**: which voice reads non-Arabic text (Automatic, or any installed voice). It cannot be turned off: if no other voice is installed at all, the Arabic voice reads Latin text in a rough Arabic transliteration instead of staying silent.
- **Clarity**: how strongly the voice is sharpened by the postfilter.
- **Breathiness**: from natural down to very low breath noise.
- **Sharpening**: a crisper, more detailed spectrum. To keep the add-on small, it appears only when the voices are written again from their training output with `FASIH_SHARP=1` (`addon_src/build_addon.sh`).
- **Modulation postfilter**: on by default. It restores part of the natural movement of the voice from moment to moment, measured on each voice's recordings, so the voices sound livelier and less muffled. Turn it off to hear the voices as trained.
- **Consonant repair**: removes the breathy cough some voices make on ق ط ش, at the cost of a somewhat muffled sound. Off by default.
- **Read emojis and symbols by name**.
- **Pausal ending (waqf)**: the last word of a sentence is read in its pausal form.
- **Full case endings (classical reading)**: off by default, so case endings are dropped in the middle of a phrase as most people read (a comma is a pause too); turn it on for full i'rab.
- **Mixed Arabic and Latin text**: Automatic (Latin text goes to the secondary voice exactly as written, with a number just before a Latin word, as in 5 PM), Arabic first (numbers, symbols and Roman numerals read in Arabic, as in 1.0), or Secondary voice first (numbers too).

## Pronunciation dictionary

In NVDA's Tools menu, **Fasih > Pronunciation dictionary** lists the words you want said differently. Add a word as it is written and how it should be said (diacritize it as much as you need), press Listen to hear it, and OK applies it at once. A word matches however it is diacritized and also with و ف ب ل ك attached (an entry for جبران also fixes لجبران), but never inside a longer word. The entries are kept in `%APPDATA%\nvda\Fasih-config\user-dic.json`.

## Diacritizing text

**Fasih > Diacritize text** adds harakat to Arabic text of any length, whichever synthesizer NVDA is using. Type or paste the text, or open a .txt or Word (.docx) file, choose Rawi or Barq, and press Diacritize. You can keep the harakat already in the text or have it vowelled afresh. Long texts are done paragraph by paragraph with a progress bar, Stop keeps what is finished, and the result can be copied or saved as a text file. The window remembers its own choice of diacritizer, separate from the one Fasih speaks with.

## Updates

**Fasih > Check for updates** looks for a newer Fasih on GitHub and shows what changed. You can download and install it straight away, skip that version, or be reminded later. The download is checked against GitHub's size and SHA-256 checksum before NVDA installs it. The same check also looks for newer versions of the voices you downloaded. Fasih checks by itself once a day when NVDA starts; turn that off with **Check for updates automatically** in the same menu. **Include beta versions** offers test versions too, and anyone already running a beta is offered newer betas.

## How it is made

- **Language**: I wrote a data-only Arabic language for RHVoice with foma rules. It covers shadda, sun and moon letters, hamzat al-wasl in connected speech, tanween, ta marbuta, long vowels including the hidden ones, Classical stress and word classes. The French language is built the same way, around a pronunciation lexicon of about 241,000 words with spelling rules for the words it lacks, elisions, numbers, syllables and stress.
- **Training**: the voices were trained with my own HTS-style trainer, without HTK. It uses WORLD analysis, 5-state alignment with pauses, MDL decision trees and re-alignment with context-dependent models, with 40 mel-cepstral coefficients. The averaging that makes HMM voices sound muffled is countered by variance scaling: the spectrum is moved away from the voice's average by a measured amount per coefficient, which cannot click the way global variance did.
- **Engine**: RHVoice is built from source with the WORLD vocoder (Masanori Morise, BSD) added for these voices, so the breath noise is generated pulse by pulse, as in a real voice.

## Credits

- **Rawi v3** by TigreGotico (Apache-2.0): the diacritizer, which I fine-tuned for Fasih on the ArVoice texts, Shamela books (ReligiousLLMs), Rawi's own data and QCRI's diacritized Arabic Wikipedia (Mohamed and Mubarak, Advancing Arabic Diacritization, EMNLP 2025); see tools/rawi_ft.
- **The CMU Pronouncing Dictionary** (Carnegie Mellon University, BSD licence): about 12,000 words added to RHVoice's English lexicon, so every English voice in Fasih says them correctly (tools/english_lexicon).
- **Evan's voice data** is IBM's: it was learned from speech made with OpenEVV (github.com/Mudb0y/openevv), whose language data is transcribed from IBM's Embedded ViaVoice. OpenEVV's engine code is MIT; IBM's data is licensed by no one here. See voices/Evan/NOTICE.md.
- **Barq (برق)** (GPL-3.0), PlanetBlindTech's tashkeel engine, written by Shamseddin of PlanetBlindTech and included with his agreement: the second diacritizer. Fasih 1.3.1 ships his newest Barq engine and model, built here from his source. For 1.3 I fine-tuned it, retraining it on 9.5 million vowelled sentences from QCRI's diacritized Arabic Wikipedia (Mohamed and Mubarak, Advancing Arabic Diacritization, EMNLP 2025), TigreGotico, ArVoice, Shamela, Abdou's Arabic tashkeel dataset (MIT) and Tashkeela; see third_party/tashkeel_engine.
- **ClaritySynth 3.0** (my own add-on): Arabic text normalization for numbers, dates and currency, Rawi diacritization, and keeping the user's own harakat.
- **PlanetBlindTech** (rhvoice-arabic, GPL-3.0), with their blessing: the default pronunciation dictionary, the user dictionary, emoji and symbol names, single-character names, presentation-form cleanup, post-diacritization fixes and the design of the audio post-filter (the DC/rumble high-pass Fasih still uses; its limiter is now a look-ahead gain limiter of mine). Their project also showed me the data-only language approach and the stock-RHVoice packaging that Fasih follows. Thank you.
- **RHVoice Voice Manager** by Alexander Linkov (kvark128, GPL): the voice catalogue and the packaging of its voices as NVDA add-ons, which Fasih's Download voices uses.
- **RHVoice**: Olga Yakovleva and contributors (LGPL), and the English voice Bdl (CMU ARCTIC, CMU license). **WORLD**: Masanori Morise (BSD). **foma**: Mans Hulden. **ONNX Runtime**: Microsoft (MIT).
- **PlanetBlindTech** also for **Zayd** (his corpus and their trust in letting me adapt him) and for **Reem** and **Aseel**, which their team trained with Fasih's recipe. Thanks to **Mr. Aseel Bashir** for allowing his voice to be used.
- **Corpora and models**: ClArTTS (MBZUAI); the Arabic Speech Corpus (Nawar Halabi, CC BY 4.0); NeoBoy/arabic-tts-wav-24k (MIT); ArVoice (Toyin, Marew, Alblooshi, Magdy and Aldarmaki, MBZUAI, Interspeech 2025, CC BY 4.0); Nabra-82M (oddadmix, Apache-2.0, built on Kokoro-82M by hexgrad); SILMA TTS (SILMA AI, Apache-2.0); the SIWIS French Speech Synthesis Database (Honnet, Lazaridis, Garner and Yamagishi, Idiap, CC BY 4.0); ipa-dict (open-dict-data, MIT) for the French lexicon; Arabic MSA 25K Saudi Male (moesaif, CC BY 4.0); Aïda Boumaza's reading of Alf Layla wa Layla for LibriVox (public domain), with the text of the Hindawi Foundation's 2022 edition; the Piper voice Kareem (Michael Hansen, rhasspy, MIT) and the arabicttstrain recordings by AliMokhammad behind it.

## What is in this repository

- `voices/`: the thirteen voices. Each has, in `variants/`, its base file (copied as `voice.data` by the build) with small patches used by the Breathiness and Consonant repair settings.
- `lang/`: the Arabic language for RHVoice. `build_lang.sh` regenerates it from the generators (it needs foma and RHVoice's att2bin and dtree2bin).
- `lang_fr/`: the French language for RHVoice (`French/`) and its generator (`gen_fr.py`, `build_lang.sh`).
- `scripts/`: corpus download, WORLD feature extraction, alignment, labels and training (`run_voice.sh`), and `fr_prep.py`, the French front end (liaison, abbreviations). The corpora themselves are not included; the scripts download them from their original sources.
- `engine/`: `rhvoice-world-vocoder.patch` for RHVoice (at the commit in `RHVOICE_BASE_COMMIT`) and the WORLD sources to copy into `src/core/world/`. I cross-built the Windows DLLs with zig.
- `addon_src/`: the NVDA driver patches, the voice downloader (`globalPlugins/fasihVoices.py`), the Arabic text pipeline and the Rawi helper (`rawi_host/`, C source and Windows build). `build_release.sh VERSION` builds the add-on from a clean checkout of this repository with nothing else installed but bash, python3 and zip. `build_addon.sh` is the development build, which writes the voices from training output.
- `third_party/`: what the release build packages from other projects: RHVoice's NVDA driver and Windows libraries (built from source with the WORLD patch), RHVoice's English language and the Bdl voice, ClaritySynth's Arabic modules, ONNX Runtime and the Rawi model.
- `tools/make_packs.py`: builds the voice packs and their list (`voices.json`) for the "voices" release.
- `tools/`: the training stages (`new_voice.sh`), calibration, `freeze_stats.py`, and `hts_edit.py` / `prebuilt.py`, which match a voice delivered already built.
- `tests/test_arabic.py`: Arabic regression tests (waqf, hamzat al-wasl, helping vowels, merging your own harakat, script splitting, and the full pipeline when Rawi is available).

See [CHANGELOG.md](CHANGELOG.md) for what changed in each version. The add-on is available in English and Arabic.

## Requirements

NVDA 2026.1 or later (64-bit). Fasih ships only 64-bit libraries.

## Install

Download `Fasih-<version>.nvda-addon` from the Releases page and open it with NVDA.
