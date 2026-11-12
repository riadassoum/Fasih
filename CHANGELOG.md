# Changelog

## 1.1

### Voices

- **Two new voices: Faris (male) and Huda (female)**, trained on the synthetic part of ArVoice (MBZUAI, CC BY 4.0).
- **Clearer sound in all voices.** The spectrum models are about 2.5 times larger, and variance scaling restores the natural liveliness of the spectrum that statistical voices tend to flatten. Unlike the old global variance option, it cannot click.
- **ه and the hamza no longer sound like a dry cough.** They were forced to be voiceless, so they were synthesized as a burst of noise. They are now voiced where the speaker voiced them.
- **No more dragged sounds.** The last word of a long sentence could be stretched to 0.6 s per sound in Faris and Huda, and some sounds in rare contexts (such as the ه of يَهْوَاهُ in Jamal) were held far too long. No speech sound now lasts longer than twice its usual length, or than it ever does in nearly all of the recordings.
- **New setting, Consonant repair (off by default).** It turns down the vowel-like low frequencies that the edges of ق ط ش sometimes keep, gives Huda's ق a crisper release and rebuilds Faris's and Huda's voiceless consonants from their own noise, which removes the breathy cough on those letters. It also makes the voices sound somewhat muffled, so it is off by default and the voices sound as they did before; turn it on in the voice settings or the settings ring if the cough bothers you more than the muffling.
- **Long texts start speaking sooner and stop at once.** A long text (say all, a whole page) is now prepared and spoken a few hundred characters at a time, cut after a sentence or a line break, or at a comma or space when there is none, instead of all at once. Speech starts after the first piece, and pressing Control also stops the diacritizer between sentences instead of waiting for it to finish.
- **Louder.** Fasih now plays about 6 dB louder, so volume 100 matches other screen-reader voices. A look-ahead limiter turns the gain down on the loudest peaks instead of clipping them.

### Arabic text

- **Shadda:** a shadda you type without its vowel no longer removes the vowel (شدّ is read شَدَّ, محمّد is read مُحَمَّد).
- **Punctuation no longer breaks the reading of a sentence.** A colon, full stop, bracket or quotation mark used to cut the sentence into pieces that were diacritized separately, so the word before it was vowelled as if the text ended there (جَمِيلَا: instead of جَمِيلًا:، حَقْ. instead of حَقّ.). Measured on 400 human-vowelled sentences, accuracy is now the same with or without punctuation.
- **A word you vowelled partly keeps the rest of its harakat before punctuation** (الإسلامية before a comma lost its ة).
- **PlanetBlindTech's dictionary adapted to Fasih:** single ordinary words are now read from the sentence (they forced one reading: سجل، فعل، علم، قبل، تعالى...), half-vowelled hints are dropped, and a sukun written on the lam of the article before a sun letter is no longer pronounced (الْسَّلَام was read with an L). Names and phrases are kept.
- **Waqf on a word ending in shadda** (حَقّ، ثُمّ) now works whatever order the marks are in.
- **Joined names** such as عبدالله and عبدالرحمن are read as two words.
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
- **No clipping at the start of an utterance.** The limiter started each utterance at full gain, so a loud first sound could clip; it now starts at a safe gain.
- **Very long sentences** (no full stop for more than 800 characters) are cut for the diacritizer without losing the spaces at the cuts, and a single extremely long word is cut too instead of being sent whole.
- **Secondary voice first** now leaves a number on its own (123) to the secondary voice, as the setting says, instead of reading it in Arabic.
- **Changing Clarity, Breathiness, Sharpening or Consonant repair** no longer freezes NVDA for a moment. The voice files are prepared by one background worker and each is replaced in one step, so a voice is never left half written, and changing several settings quickly never loses one of them.
- **Fasih always uses its own Rawi model,** even when ClaritySynth is installed with its own downloaded models.

### Add-on

- **Arabic translation** of the add-on.
- **Requires NVDA 2026.1 or later** (64-bit).
- **Rebuilding:** `addon_src/build_release.sh` builds the add-on from a clean checkout of the repository.
- **Tests:** Arabic regression tests in `tests/test_arabic.py`.

## 1.0

First release: Jamal, Nour and Sami, with Rawi diacritization (in a separate helper process), the WORLD vocoder, and primary and secondary voices for Arabic and English.
