# Changelog

## 1.1

### Voices

- **Two new voices: Faris (male) and Huda (female)**, trained on the synthetic part of ArVoice (MBZUAI, CC BY 4.0).
- **Clearer sound in all voices.** The spectrum models are about 2.5 times larger, and variance scaling restores the natural liveliness of the spectrum that statistical voices tend to flatten. Unlike the old global variance option, it cannot click.
- **ه and the hamza no longer sound like a dry cough.** They were forced to be voiceless, so they were synthesized as a burst of noise. They are now voiced where the speaker voiced them.
- **No more dragged sounds.** The last word of a long sentence could be stretched to 0.6 s per sound in Faris and Huda, and some sounds in rare contexts (such as the ه of يَهْوَاهُ in Jamal) were held far too long. No speech sound now lasts longer than twice its usual length, or than it ever does in nearly all of the recordings.
- **Less coughing on voiceless sounds.** Where a voiceless frame (the edges of ق ط ش next to a vowel) kept a vowel-like sound, its low frequencies are now turned down in the vocoder, and Huda's ق has a crisper release.
- **The hamza at the end of a word** (طاء، سماء) is a short closure instead of a croak.
- **Steadier pitch.** The pitch models are built from more examples each, so the pitch no longer jumps up and down between neighbouring parts of a syllable (the shake in رياض, العلم نور, وَمِنَ).
- **Louder.** Fasih now plays about 6 dB louder, so volume 100 matches other screen-reader voices. A look-ahead limiter turns the gain down on the loudest peaks instead of clipping them.

### Arabic text

- **Shadda:** a shadda you type without its vowel no longer removes the vowel (شدّ is read شَدَّ, محمّد is read مُحَمَّد).
- **Long vowels before hamzat al-wasl** are shortened as in real speech: بِهَذَا ٱلصَّوْتِ is read bihādha-ṣṣawt, فِي ٱلْبَيْتِ fi-lbayt, عَلَى ٱلْأَرْضِ ʿala-lʾarḍ.
- **Letter names:** ل is read لَام, not لَآم (a fatha written on the alif was read as a hamza).
- **Waqf:** فَتًى becomes فَتَى at a pause, and the names of single characters are said in pausal form (شَدَّة).
- **Mixed Arabic and English:** Latin text is passed to the secondary voice exactly as written (I like playing is no longer read with I as a Roman numeral, and Ctrl+C stays Ctrl+C). A number just before a Latin word goes with it (5 PM). A new setting chooses the priority.

### Speech and responsiveness

- **Diacritizer time limit:** once the Rawi helper is running, speech waits at most 1 s for it instead of 3 s.
- **Faster start:** speech begins on the first finished part of an utterance; the silence before each utterance is trimmed from about 40 ms to 8 ms; the gap between the Arabic and English parts of one utterance is shortened; voices are loaded in the background before their first use.
- **Interrupting** stops the text preparation as well as the sound.
- **Say All and index marks** stay in step across Arabic and English parts, and NVDA is told speech is finished only once, at the end.

### Settings

- **New: Mixed Arabic and Latin text** chooses which voice reads numbers, symbols and Roman numerals next to Latin text.
- **Settings ring:** Mixed text is in it too.
- **Quality settings** are written once, 1.5 s after the last change, and never switch you back to Fasih if you chose another synthesizer meanwhile.
- **Sharpening** is now clearly stronger than the default and no longer clicks.

### Add-on

- **Arabic translation** of the add-on.
- **Requires NVDA 2026.1 or later** (64-bit).
- **Rebuilding:** `addon_src/build_release.sh` builds the add-on from a clean checkout of the repository.
- **Tests:** Arabic regression tests in `tests/test_arabic.py`.

## 1.0

First release: Jamal, Nour and Sami, with Rawi diacritization (in a separate helper process), the WORLD vocoder, and primary and secondary voices for Arabic and English.
