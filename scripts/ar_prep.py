"""Arabic text preparation shared by training and the NVDA add-on.

Input: diacritized Arabic (from Rawi or the user). Output: text for the RHVoice
Arabic language. The one thing RHVoice's per-word rules cannot see is the
neighbouring word, so connected-speech hamzat al-wasl is marked here:
a word-initial bare alif (ا, no hamza) that follows another word in the same
phrase becomes U+0671 (ٱ) and loses any vowel mark on it.
"""
import re

ALEF, WASLA = "ا", "ٱ"
MARKS = "ً-ْٰ"
_word = re.compile(r"[ء-يٱً-ْٰـ]+")
_phrase_break = re.compile(r"[.,;:!?،؛؟۔()\[\]{}\"«»\n\-–—]")


_COMPOSE = [  # letter, marks..., combining hamza/madda  ->  precomposed letter, marks
    ("\u0627", "\u0654", "\u0623"), ("\u0627", "\u0655", "\u0625"), ("\u0627", "\u0653", "\u0622"),
    ("\u0648", "\u0654", "\u0624"), ("\u064A", "\u0654", "\u0626"), ("\u0649", "\u0654", "\u0626"),
]
_KEEP_MARKS = "\u064B-\u0652\u0670"


def normalize(text: str) -> str:
    for base, comb, pre in _COMPOSE:
        text = re.sub(base + "([" + _KEEP_MARKS + "]*)" + comb, pre + r"\1", text)
    # other combining marks (Quranic signs, stray hamza/madda) are not readable by the voice
    text = re.sub("[\u0610-\u061A\u0653-\u065F\u06D6-\u06ED]", "", text)
    text = text.replace("\u0640", "")  # tatweel
    # sukun written on the lam of the article before a sun letter (الْسَّلَام, لِلْتَّفَاعُل): the
    # lam is not pronounced, so drop the sukun and make sure the sun letter has its shadda.
    def _sun(m):
        marks = m.group(3)
        if "\u0651" not in marks:
            marks = "\u0651" + marks
        return m.group(1) + m.group(2) + marks
    text = re.sub("(?<![\u0621-\u064A])((?:[\u0648\u0641\u0628\u0643][\u064E\u0650]?)?(?:[\u0627\u0671][\u064E]?\u0644|\u0644\u0650?\u0644))\u0652"
                  "([\u062A\u062B\u062F\u0630\u0631\u0632\u0633\u0634\u0635\u0636\u0637\u0638\u0644\u0646])([\u064B-\u0652]*)", _sun, text)
    text = re.sub("(^|(?<=[^\u0621-\u064A\u0671\u064B-\u0652\u0670]))[\u064B-\u0652\u0670]+", "", text)  # marks with no letter
    # a word-final ي after a consonant with no mark is the long vowel ī (في، الذي، التي, left bare in
    # partly vowelled text): without the kasra it was read as the consonant y (في ٱلدّانمارك -> f-y-dd...).
    text = re.sub("([\u0628-\u063A\u0641-\u0648])(?=\u064A(?![\u0621-\u064A\u064B-\u0652\u0670]))", "\\1\u0650", text)
    return text


_END = re.compile(r"([\u0621-\u064A\u0671][\u0621-\u064A\u0671\u064B-\u0652\u0670]*)(?=\s*(?:[.!?\u061F\u06D4\n]|$))")


def _waqf_word(w: str) -> str:
    """Classical pause (waqf) on one word: drop the final short vowel or tanween;
    -atun/-atin/-atan -> -ah (bare teh marbuta is read h); -an (tanween fath + alif) -> -aa."""
    if re.search("\u064B\u0627$|\u0627\u064B$", w):
        return re.sub("(\u064B\u0627|\u0627\u064B)$", "\u064E\u0627", w)
    if re.search("\u064B\u0649$|\u0649\u064B$", w):          # فَتًى -> فَتَى
        return re.sub("(\u064B\u0649|\u0649\u064B)$", "\u064E\u0649", w)
    if re.search("\u0629[\u064B-\u0652]*$", w):
        return re.sub("\u0629[\u064B-\u0652]*$", "\u0629", w)
    # drop the final short vowel or tanween, whatever order it has with a shadda (حَقٌّ، ثُمَّ)
    return re.sub("[\u064B-\u0652]+$", lambda m: re.sub("[\u064B-\u0650]", "", m.group(0)), w)


def pausal_ending(text: str) -> str:
    return _END.sub(lambda m: _waqf_word(m.group(1)), text)


def _helping_vowel(word: str) -> str:
    """Two silent letters meet across hamzat al-wasl (مِنْ ٱلصَّبْرِ): the first takes a
    helping vowel. Fatha after مِن, damma after the plural مْ (هُمْ، كُمْ، تُمْ), kasra otherwise."""
    if not word.endswith("\u0652"):
        return word
    bare = re.sub("[" + MARKS + "]", "", word)
    if bare in ("من", "ومن", "فمن"):
        v = "\u064e"
    elif re.search("(هم|كم|تم)$", bare):
        v = "\u064f"
    else:
        v = "\u0650"
    return word[:-1] + v


def _short_before_wasl(word: str) -> str:
    """A long vowel at the end of a word is shortened before hamzat al-wasl, because two silent
    sounds cannot meet: بِهَذَا ٱلصَّوْتِ -> بِهَذَ ٱلصَّوْتِ (bihadha-ssawt), فِي ٱلْبَيْتِ -> فِ ٱلْبَيْتِ,
    عَلَى ٱلْ -> عَلَ ٱلْ, كَتَبُوا ٱلْ -> كَتَبُ ٱلْ. Only spoken form changes; tanween is left alone."""
    for pat in ("\u064e[\u0627\u0649]\u0652?$", "\u0650\u064a\u0652?$", "\u064f\u0648\u0627?\u0652?$"):
        m = re.search(pat, word)
        if m:
            return word[:m.start() + 1]
    return word


def mark_wasl(text: str) -> str:
    text = normalize(text)
    out = []
    prev_word = False
    last = None
    pos = 0
    for m in _word.finditer(text):
        gap = text[pos:m.start()]
        if _phrase_break.search(gap):
            prev_word = False
        w = m.group(0)
        if re.fullmatch("[" + ALEF + WASLA + MARKS + "\u0640]+", w):
            out.append(gap)  # a lone alif is a typo, not a word
            pos = m.end()
            continue
        if prev_word and w.startswith(ALEF):
            w = WASLA + re.sub("^[" + MARKS + "]+", "", w[1:])
            if last is not None:
                out[last] = _short_before_wasl(_helping_vowel(out[last]))
        out.append(gap)
        out.append(w)
        last = len(out) - 1
        prev_word = True
        pos = m.end()
    out.append(text[pos:])
    return "".join(out)


if __name__ == "__main__":
    import sys
    for line in sys.stdin:
        sys.stdout.write(mark_wasl(line))
