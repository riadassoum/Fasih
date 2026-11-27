# Changelog

## 2.0 (in development)

### Voices

- **Premium voices**: a new kind of voice to download, Haytham Premium, Jamal Premium and Zayd Premium, made of real recorded speech joined together rather than generated, then smoothed by a neural vocoder. The download window marks each voice as premium or compact. The vocoder is a small add-on of its own that comes with your first premium voice and stays when Fasih is updated.
- **Haytham (هيثم), a new voice to download**: trained on seven and a half hours of natural, expressive speech, with breaths and the wide pitch range of a human reader. He is the most natural Fasih voice.
- **Bassel (باسل) and Rana (رنا)**: a male and a female voice from the same family as Sami and Nour, each trained on six to seven hours.
- **Hadi (هادي)**: a male voice from a professional speaker's recordings, the smallest Fasih voice (2.5 MB).
- **Listen from the internet**: voices added after your copy of Fasih was built play their sample from the internet in the download window, so every voice can be heard before you install it.
- **Only updated voices are offered as updates**: "Check for updates" no longer offers voices that have not changed.

### Reading

- **Normal voices with NVDA in French**: with NVDA's interface in French, and other languages that write decimals with a comma, every voice sounded high-pitched and fast. They now sound as they should.
- **No more silent sentences**: a sentence that contained a word the voices could not pronounce (for example پ before الـ, as in پِالجبوب, or some rarely used letters) was not spoken at all. Now such a word is left out and the rest of the sentence is read; پ چ ژ ڤ ڨ گ ک ی are read as letters, with their vowels.
- **Barq 3**: Fasih now uses Barq 3, the newest version of PlanetBlindTech's engine by Shamseddin. In my tests it makes fewer mistakes than 1.3.1's Barq on classical text, Rawi's own test, news and the independent SadeedDiac-25 benchmark, and about as many on modern sentences.

### Help

- **Up-to-date help**: the English and Arabic help pages now always match the current version.

## 1.3.1

- **Barq is more accurate and faster**: Fasih now uses the newest version of Barq, PlanetBlindTech's engine by Shamseddin, with tighter grammar rules and wider word context. In my tests it makes about a third fewer mistakes than 1.3's Barq on modern sentences and news, and fewer on classical text, where it now does better than Rawi; it is also several times faster than 1.3's Barq. Choose it under Diacritizer in NVDA's voice settings.
- **Aisha (عائشة), a new voice to download**: a female voice trained by PlanetBlindTech with Fasih's recipe, matched to the other voices' pace and loudness. Find her in Tools > Fasih > Download voices > "Arabic: Fasih voices".

## 1.3

### Voices

- **Smaller add-on, voices on demand.** The add-on now carries Aseel (the new default voice), Riad, the English voice Bdl and the French voice Elise. Every other Fasih voice is installed from Tools > Fasih > Download voices > "Arabic: Fasih voices", each as its own small add-on that NVDA keeps across Fasih updates and that follows Fasih's settings. Each one can be heard first with the Listen button, and an installed voice with a newer version shows "Update available".
- **Updating from 1.2.** Jamal, Nour, Sami, Faris, Huda, Karim, Rima, Lamees, Zayd and Reem are now downloads. If you used one of them, Fasih speaks with Aseel and tells you once where to get your voice back, with a button that opens Download voices. A secondary voice that is no longer there is replaced by Bdl.
- **Four new voices to download:**
  - **Nabil (نبيل)**, a clear male voice in Modern Standard Arabic, for non-commercial use.
  - **Amjad (أمجد)**, a male voice in Modern Standard Arabic.
  - **Shahrazad (شهرزاد)**, a female storyteller's voice made from a real reader's audiobook, Aïda Boumaza's LibriVox recording of Alf Layla wa Layla.
  - **Evan**, a male English voice you can choose as Fasih's English voice instead of Bdl. Evan was trained on speech from OpenEVV's Adult Male 1, so his voice data is IBM's, and he is for personal, non-commercial use only; the notice in his pack explains why.

  Nabil, Amjad and Shahrazad read الله with the heavy lam after a or u and the light lam after i.
- **A clearer English voice.** Bdl replaces SLT. In my tests with speech recognition at the same speaking rate, Bdl was understood with about half of SLT's errors at normal speed, and with fewer errors at fast rates. SLT can still be downloaded from Download voices.
- **English as loud as Arabic.** Bdl is matched to Aseel's loudness, so switching between Arabic and English no longer jumps in volume.
- **Modulation postfilter** (on by default; a new setting turns it off). HMM voices change their sound too smoothly from moment to moment, which makes them a little muffled. Fasih now restores part of the natural movement measured in each voice's recordings (the time-invariant modulation spectrum postfilter of Takamichi et al., at half strength). Pitch, timing and loudness are unchanged.
- **Riad** now has the same gentle tone correction as the other voices, with his pitch unchanged.

### Speech

- **Faster response.** Speech starts about 40 ms sooner after each key press, because the silence the voices put before their first sound is trimmed to 5 ms. To get the old behaviour, turn off "Start speaking without leading silence" in NVDA's voice settings.
- **Steadier speech in long passages.** Each sentence is spoken as its own utterance, so a voice no longer turns rough or shaky partway through a long paragraph.
- **Everyday reading by default.** In the middle of a phrase, case endings (tanween and a final damma or kasra) are no longer pronounced, as most people read, and a comma is a pause like a full stop. Endings that carry meaning stay (قرأتُ، أنتِ، لكِ), as does the vowel that joins a word to a following ال. Turn on "Full case endings (classical reading)" in the voice settings for full i'rab.
- **Arabic questions sound like questions.** ؟ and ؛ only got a comma's pause; they now end the sentence like ? and ;, with question intonation.
- **Text in a marked language goes to a voice of that language.** When a web page or document marks a passage as French, English or another language (and NVDA's automatic language switching is on), an installed voice of that language reads it. Without such a mark, or without such a voice, the secondary voice reads it as before.

### Diacritization

- **Rawi is more accurate and twice as fast.** Fasih now uses its own fine-tuned Rawi: Rawi v3, trained further on modern Arabic sentences, fully vowelled books from Shamela and QCRI's diacritized Arabic Wikipedia. Against 1.2's Rawi, measured by the share of words with a wrong vowel:
  - modern news (WikiNews): 12.9%, down from 17.3%
  - other modern sentences: 16.6%, down from 20.4%
  - classical book text: 19.7%, down from 22.2%

  The training scripts and results are in tools/rawi_ft.
- **A second diacritizer to choose from: Barq (برق)**, PlanetBlindTech's tashkeel engine by Shamseddin, fine-tuned for Fasih on 9.5 million vowelled sentences. It is about thirty times faster than Rawi, nearly as accurate on modern news and a little more accurate on classical text. Rawi stays the default. Choose under Diacritizer in NVDA's voice settings or in the synth settings ring.
- **Diacritize text** in Tools > Fasih adds harakat to Arabic text of any length, typed, pasted or opened from a text or Word file. You choose Rawi or Barq in the window itself. Long texts show their progress and can be stopped, and the result can be copied or saved.
- **No invented hamza.** The diacritizer sometimes put a hamza on a plain و or ي (البور read as البؤر, كاين as كائن, الوادي as الوادئ); the letters are now spoken as written. Impossible vowels on أ and إ, and a doubled ئ in مئة, are corrected.

### Arabic text

- **Screen-reader text reads better:**
  - زر، ملف، مجلد and مربع (button, file, folder, check box) are vowelled correctly on their own.
  - لديك is no longer read as feminine.
  - "..." is a pause, instead of being named in the middle of a sentence.
  - Durations are read as durations: in رسالة صوتية، 0:45 is خمس وأربعون ثانية, and a video's 12:34 is minutes and seconds.
  - "1 - 20 من 350" is a range, read with the right case.
  - Prices take the genitive (بدلا من أربعين دولار).
  - Version numbers (الإصدار 2026.3), units (كم/س، كغ، GB), list letters (أ) ب)), file names with underscores, "رقم #" and © next to "محفوظة" are read naturally.
  - Digits in phone numbers are spoken cleanly.
- **Numbers read the way Arabic says them.** The number agrees with the counted noun (ثلاث سيارات، خمس دقائق، إحدى وعشرون طالبة، ثلاثة كتب), takes the genitive after prepositions and words like عام (خلال عشرين دقيقة، عام ألفين وستة وعشرين), and و is joined to the next number. Also new:
  - thousands separators (1,250,000) and negative numbers (سالب خمس درجات)
  - ranges (3-5 أيام) and simple fractions (نصف كوب)
  - version numbers (2.5.1)
  - ordinals up to 99 (الدرس الـ5، القرن الـ21), feminine after a feminine noun
  - dates with month names (2 مايو، 15 كانون الثاني 1990)
  - page and volume numbers (ص 45، ج 2)
  - times, which say صباحا or مساء once, with minutes counted properly
- **No more words glued to abbreviations.** A word ending in م or د before a full stop was read as an abbreviation (الرحيم. became الرحيمهندس, 632م. became …مهندس). Abbreviations now expand only as whole words, a one-letter title (د. م. أ.) only before a name, and a year followed by م or هـ is read ميلادي or هجري.
- **Honorific signs** (ﷺ, (ص), (رض), (ع)) are read in full, and emoji no longer leave stray symbols behind.
- **Spelling.** Moving by character over a letter with marks names the marks too (بَّ is باء شدة فتحة), and Persian letters (پ چ ڤ گ ی) and the lam-alef ligature have names.
- **Pronunciation dictionary** in Tools > Fasih: add, edit, listen to and remove your own pronunciations, applied at once. Entries also match with و ف ب ل ك attached.

### English and French

- **English words read correctly.** Every English voice (Bdl, Evan and any other RHVoice English voice) knows about 12,000 more words from the current CMU Pronouncing Dictionary, about half of which used to be guessed wrongly. "changer" no longer loses a sound, and "Nabil" keeps its l.
- **French numbers read correctly in more cases.** The zeros after the decimal comma are read (3,05 is trois virgule zéro cinq, no longer trois virgule cinq). Very long numbers and fractions are read digit by digit instead of being skipped. une is used after numbers ending in un before a feminine noun (quatre-vingt-une heures).
- **French liaison** is no longer made before plural or feminine forms of words that refuse it (les handicaps, les hautes).

### Add-on

- **Check for updates** in Tools > Fasih, and once a day automatically. It shows what is new, then downloads, verifies and installs the new version, or skips it. It also finds newer versions of your downloaded voices. Beta versions are offered only if you ask for them.
- **Cancelling a voice download** closes the window at once, even when the server has stopped answering, and a stalled download now gives up after a short time instead of waiting indefinitely.

## 1.2

### Voices

Fasih now has twelve Arabic voices and its first French one.

- **Karim (male) and Rima (female)**, trained on about 4.8 hours each of the synthetic part of ArVoice (MBZUAI, CC BY 4.0), from Google's ar-XA-Wavenet-C and ar-XA-Wavenet-D voices.
- **Lamees (female)**, a young voice trained on about 4.7 hours generated with Nabra-82M (oddadmix, Apache-2.0, a fine-tune of Kokoro-82M).
- **Zayd (male)**, PlanetBlindTech's voice, adapted at their request: retrained with Fasih's recipe on their whole corpus of 10,023 clips (12.5 hours).
- **Riad (male)**, my own voice. I gave a clip of my voice as the reference, and about 3.5 hours of my voice were generated from it with SILMA TTS (Apache-2.0) and trained like the other voices.
- **Reem (female) and Aseel (male)**, trained by PlanetBlindTech with Fasih's recipe and added at their request: Reem on about 6.1 hours of synthetic speech from SILMA TTS, Aseel on about 12 hours made from the voice of Mr. Aseel Bashir, with his consent.
- **Elise (female, French)**, the first French RHVoice voice, trained on the SIWIS French Speech Synthesis Database (9.2 hours of studio recordings, CC BY 4.0). It is a secondary voice: choose it under Secondary voice and Latin-script text is read in French.
- **Every voice speaks at the same rate.** Sami, for example, spoke much more slowly than the others; all voices now speak at Jamal's pace, with pauses left as they were. NVDA's rate setting therefore means the same thing on every voice.
- **Bass and tone brought closer together.** Each voice except Riad gets a gentle correction (at most about 4 dB) towards the average of the voices of its gender, so switching voices no longer changes the sound so much, and the loudness of every voice is matched again afterwards.
- **The name of God (الله) is pronounced with a heavy lam** after a or u (وَاللَّهُ، قَالَ اللَّهُ) and a light one after i (بِسْمِ اللَّهِ) in every voice.
- **Sharpening is no longer in the add-on,** to keep it small. The setting comes back when the voices are written again from their training output with `FASIH_SHARP=1` (`addon_src/build_addon.sh`).

### French

- **A French language for RHVoice**: a pronunciation lexicon of about 241,000 words (ipa-dict, MIT) with spelling rules for words it lacks, elided words joined to the next word (l'ami, jusqu'à), numbers read in full up to the billions (soixante-dix, quatre-vingts, vingt et un), syllables and stress. It works in RHVoice itself, outside Fasih too.
- **Liaison** when Elise reads inside Fasih: after determiners, pronouns, prepositions and common adverbs (les‿amis, mon‿ami, chez‿eux, très‿important), in chains (c'est‿un‿honneur), before a mute h (un‿honneur, deux‿hommes), after plural adjectives (de petits‿enfants), in inversions (dit-‿il, vont-‿ils, allons-‿y) and in numbers (vingt‿et‿un, vingt-‿et-une heures, cent‿ans, neuf heures read with a v); never before an aspirated h (les héros) or after et.
- **Numbers, times and ordinals in words** for Elise: 21 heures and 21h30 (vingt et une heures, with une before feminine nouns), 1er, 2e, 21e (premier, deuxième, vingt et unième), decimals (3,5) and 1 000; common abbreviations (M., Mme, etc.) too.
- **Automatic secondary voice follows NVDA's language**: with NVDA in French, Latin text is read by Elise instead of the English voice (in 1.1 French menus and messages were read by the English voice, or by the Arabic voice when the secondary voice was off, and were unintelligible); otherwise an English voice is preferred.
- **The secondary voice can no longer be turned off.** A saved None becomes Automatic. If no other voice is installed at all, the Arabic voice reads Latin text in a rough Arabic transliteration, with acronyms spelled out (NVDA, PDF), instead of falling silent or garbling it.

### Arabic text

- **Tatweel** (ـ) is dropped inside a word before diacritization, so مـحـمـد is read محمد, and named تَطْوِيل when it is alone.
- **Invisible characters no longer freeze the voice.** The zero-width non-joiner, zero-width joiner, zero-width space, direction marks, BOM and soft hyphen sent alone to the engine made it hang for about 20 seconds while moving by character. Inside text they are removed; alone they are named (مَانِعُ ٱلْوَصْل، عَلَامَةُ ٱلْيَمِين...).
- **A haraka written on a bare alif or on آ** (اُ، اِ، اْ، آَ) is dropped, so it is no longer read as an extra vowel or a stutter. The fathatan of كِتَاباً is kept.
- **A final ي after an unvowelled letter** gets its kasra (في الدانمارك is read فِي ٱلدَّانِمَارْك, not as if the ي were a consonant).

### Add-on

- **Download more voices from NVDA's Tools menu** (Fasih > Download voices): every RHVoice voice published for Alexander Linkov's RHVoice Voice Manager, by language, with a real progress window (size, speed, time elapsed and remaining, Cancel) and a checksum check. Voices are installed as standard RHVoice voice add-ons, the same ones the Voice Manager installs, and become available as secondary voices after NVDA restarts.

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
