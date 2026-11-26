# -*- coding: utf-8 -*-
"""Arabic text pipeline run (on the TTS thread) before RHVoice sees the text.

Combines the best processing of three add-ons:
  * ClaritySynth 3.0 (Riad Assoum): number/date/currency reading (ar_textnorm),
    Rawi diacritization that keeps the user's own harakat (ar_tashkeel + ar_g2p).
  * PlanetBlindTech's RHVoice Arabic (GPL-3.0): user dictionary and default
    pronunciation rules, emoji/symbol names, single-character names,
    presentation-form cleanup, post-diacritization fixes and pausal sukoon.
  * This add-on: connected-speech hamzat al-wasl marking (ar_prep), matching training.
Any failure falls back to the text so far, so speech never stops.
"""
import re
import threading

_AR = re.compile(r"[؀-ۿ]")
_lock = threading.Lock()
_boot_lock = threading.Lock()
_ready = None

# user-adjustable (set by the driver)
options = {"read_emojis": True, "pausal_ending": True, "full_case": False, "mixed_priority": "auto"}


def _log_error(msg):
    try:
        from logHandler import log
        log.error(msg, exc_info=True)
    except Exception:
        pass


def _boot():
    """Load Rawi once; thread-safe (preload thread and TTS thread may race)."""
    global _ready
    if _ready is not None:
        return _ready
    with _boot_lock:
        if _ready is not None:
            return _ready
        try:
            from .arabic import ar_tashkeel
            ar_tashkeel.set_backend(options.get("diacritizer", "rawi"))
            _ready = True
        except Exception:
            _log_error("Fasih: tashkeel unavailable")
            _ready = False
    return _ready


def init_dictionaries(config_dir):
    try:
        from .pbt import dictionary_manager as dm
        dm.load_default()
        dm.load_user_dictionary(config_dir)
    except Exception:
        _log_error("Fasih: dictionaries unavailable")


def preload():
    """Load Rawi in the background so the first utterance is not slow."""
    def run():
        if _boot():
            try:
                from .arabic import ar_tashkeel
                ar_tashkeel.diacritize_strict("بسم", backend=ar_tashkeel.get_backend())
            except Exception:
                pass
    threading.Thread(target=run, daemon=True).start()


_TAN_AFTER_ALIF = re.compile("\u0627\u064b")                        # اً -> ًا (standard spelling)
_FATHA_TAN = re.compile("\u064e+(?=[\u0627\u0649]?\u064b)|(?<=\u064b)\u064e+")  # fatha next to tanween
_DUP = re.compile("([\u064b-\u0652])\\1+")                          # the same mark twice


def _tanween_pre(text):
    return _TAN_AFTER_ALIF.sub("\u064b\u0627", text)


def _alif_fatha(text):
    # a fatha written on the alif instead of the letter before it (لاَم): the voice would read
    # the alif as a hamza (لَآم). Put the fatha back on the letter: لَام.
    return re.sub("([\u0628-\u064a])\u0627\u064e", "\\1\u064e\u0627", text)


# invisible format characters (ZWNJ, ZWJ, zero-width space, direction marks, BOM, soft hyphen,
# tatweel): the voice read them as breaks inside a word
_INVISIBLE = re.compile("[\u00ad\u061c\u200b-\u200f\u202a-\u202e\u2060-\u2064\u2066-\u2069\ufeff]")
_TATWEEL = "\u0640"
# names said when the cursor lands on one of them alone (sent raw, they hung the engine)
_INV_NAMES = {"\u200c": "مَانِعُ ٱلْوَصْل", "\u200d": "وَاصِل", "\u200b": "مَسَافَةٌ صِفْرِيَّة",
              "\u200e": "عَلَامَةُ ٱلْيَسَار", "\u200f": "عَلَامَةُ ٱلْيَمِين", "\u061c": "عَلَامَةٌ عَرَبِيَّة",
              "\u00ad": "شَرْطَةٌ مَخْفِيَّة", "\ufeff": "رَمْزٌ خَفِيّ", _TATWEEL: "تَطْوِيل"}


def _clean_invisible(text):
    """None if the text is only invisible characters / tatweel (then it is named or skipped)."""
    s = text.strip()
    if len(s) == 1 and (s in _INV_NAMES or _INVISIBLE.match(s)):
        return _INV_NAMES.get(s, "رَمْزٌ خَفِيّ")
    text = _INVISIBLE.sub("", text)
    if _TATWEEL in text:
        rest = text.replace(_TATWEEL, "")
        # inside or next to words the tatweel is only stretching: dropped (Rawi never sees it);
        # tatweels on their own are named
        text = rest if rest.strip() else _INV_NAMES[_TATWEEL]
    return text if text.strip() else " "


def _alif_marks(text):
    # a haraka on a bare alif (اُ، اِ، اْ، اّ) or on آ (which already holds its vowel) was read as an
    # extra vowel or a stutter: the mark is dropped (the alif's fathatan, اً, stays)
    text = re.sub("\u0627[\u064c-\u0652]+", "\u0627", text)
    return re.sub("\u0622[\u064b-\u0652]+", "\u0622", text)


def _tanween_post(text):
    text = _alif_marks(_alif_fatha(text))
    text = re.sub("\u0625[\u064e\u064f]", "\u0625\u0650", text)   # إ always takes kasra
    text = re.sub("(?<=\u064a\u0652)\u0643\u064e(?=\u0645)", "\u0643\u064f", text)  # عَلَيْكُم
    text = re.sub("(?<=\u064a\u0652)\u0647[\u064e\u064f](?=\u0645)", "\u0647\u0650", text)  # عَلَيْهِم
    text = _TAN_AFTER_ALIF.sub("\u064b\u0627", text)
    text = _FATHA_TAN.sub("", text)
    return _DUP.sub("\\1", text)


_MARKS_RE = re.compile("[\u064b-\u0652]")


def _strip_lone_tanween(text):
    """A word whose only mark is a final tanween (e.g. منبعًا) would be kept as
    'already diacritized' and left bare. Drop that tanween so Rawi vowels the word,
    and remember which words had it. Returns (text, set of word indexes)."""
    words = text.split(" ")
    had = set()
    for i, w in enumerate(words):
        m = re.search("([\u064b-\u064d])(\u0627|\u0649)?([^\u0621-\u064a]*)$", w)
        if m and len(_MARKS_RE.findall(w)) == 1:
            words[i] = w[:m.start()] + (m.group(2) or "") + m.group(3)
            had.add((i, m.group(1)))
    return " ".join(words), had


def _restore_tanween(text, had):
    if not had:
        return text
    words = text.split(" ")
    for i, mark in had:
        if i >= len(words):
            continue
        w = words[i]
        m = re.search("[\u064b-\u0650]?(\u0627|\u0649)?([^\u0621-\u064a\u064b-\u0652]*)$", w)
        if m:
            words[i] = w[:m.start()] + mark + (m.group(1) or "") + m.group(2)
    return " ".join(words)


# --- mixed Arabic / Latin text ---------------------------------------------------------
# A Latin island is a run of Latin words with what is attached to them (Ctrl+C, e-mail
# addresses, 5 PM, v2.0). In "auto" and "secondary" modes it is hidden from the Arabic
# normalizer and diacritizer, so the secondary voice gets it exactly as written
# (before, "I like playing" became "أول like playing": I was taken for a Roman numeral).
_LAT_WORD = r"[A-Za-z][A-Za-z0-9@._+\-/'\u2019&#%]*"
_ISLAND = re.compile(r"(?:\d+(?:[.,:]\d+)*[ \t]*)?" + _LAT_WORD + r"(?:[ \t]*[.,:;!?]?[ \t]+(?=[A-Za-z0-9])(?:\d+(?:[.,:]\d+)*|" + _LAT_WORD + r"))*")
_ROMAN_OK = re.compile(r"(?:X{0,2})(?:IX|IV|V?I{0,3})")
_PUA = 0xE000


def _protect(text, mode):
    if mode == "arabic":
        return text, []
    keep = []

    def repl(m):
        tok = m.group(0)
        if mode == "auto" and len(tok) >= 2 and _ROMAN_OK.fullmatch(tok):
            # a Roman numeral right after an Arabic word (الفصل IV) is read in Arabic
            before = text[:m.start()].rstrip()
            if before and _AR.search(before[-1]):
                return tok
        keep.append(tok)
        return chr(_PUA + len(keep) - 1)
    out = _ISLAND.sub(repl, text)
    if mode == "secondary":
        def num(m):
            keep.append(m.group(0))
            return chr(_PUA + len(keep) - 1)
        out = re.sub(r"[0-9\u0660-\u0669]+(?:[.,:/][0-9\u0660-\u0669]+)*", num, out)
    return out, keep


def _unprotect(text, keep):
    if not keep:
        return text
    return re.sub("[\uE000-\uF8FF]", lambda m: keep[ord(m.group(0)) - _PUA] if ord(m.group(0)) - _PUA < len(keep) else "", text)


def _step(fn, text):
    try:
        out = fn(text)
        return out if isinstance(out, str) and out.strip() else text
    except Exception:
        return text


def process(text, diacritize=True, is_cancelled=None):
    """is_cancelled, if given, is polled by the diacritizer between sentences."""
    if not text or not text.strip():
        return text
    from .pbt import dictionary_manager as dm
    from .pbt import pbt_textnorm as pt
    from .arabic import ar_prep
    char_request = not diacritize
    if _INVISIBLE.search(text) or _TATWEEL in text:
        new = _clean_invisible(text)
        if new != text and not new.strip():
            return new
        if new in _INV_NAMES.values():
            return _engine_safe(_step(ar_prep.mark_wasl, new))
        text = new
    out = _step(dm.apply_user_dictionary, text)
    out = _step(dm.apply_default_rules, out)
    # single characters (cursor movement, spelling): name them
    if (char_request or len(out.strip()) == 1) and not re.fullmatch(r"\s*[A-Za-z0-9]\s*", out):
        # Arabic letters, harakat and symbols get spoken names; Latin letters stay for the English voice
        name = _step(pt.get_single_char_name, out) if out.strip() else out
        if name != out:
            # a name is said on its own, so in its pausal form (شَدَّة, not شَدَّةٌ)
            return _engine_safe(_step(ar_prep.mark_wasl, _step(ar_prep.pausal_ending, _alif_fatha(name))))
    if options.get("read_emojis", True):
        out = _step(dm.replace_emojis, out)
    if not _AR.search(out):
        # numbers on their own (no Latin letters) are read as Arabic words
        # ("secondary voice first" leaves them to the secondary voice, as the setting says)
        if options.get("mixed_priority", "auto") == "secondary":
            return out
        if re.search(r"[0-9\u0660-\u0669]", out) and not re.search(r"[A-Za-z]", out):
            try:
                from .arabic import ar_textnorm
                num = ar_textnorm.normalize(out)
            except Exception:
                num = out
            if not _AR.search(num):
                return out
            out = num  # continue: diacritize the number words like any Arabic text
            diacritize = True
        else:
            return out  # pure Latin/other text: the English voice reads it
    out, keep = _protect(out, options.get("mixed_priority", "auto"))
    try:
        return _engine_punct(_unprotect(_arabic_core(out, diacritize, pt, ar_prep, is_cancelled), keep))
    except Exception:
        return _engine_punct(_unprotect(out, keep))


# The engine only knows Latin ? and ; as sentence ends: with ؟ and ؛ a
# question got a comma's pause and no question intonation.
_PUNCT = str.maketrans({"\u061F": "?", "\u061B": ";", "\u2026": "..."})


# Inputs the engine cannot pronounce, which make it drop the WHOLE sentence, not just the word
# (found by trying every letter and mark pair): alef wasla straight after a letter that has no
# vowel (پٱلجبوب), tatweel next to some letters, and the rare letters U+063B-U+063F.
_RARE = str.maketrans({"\u063B": "\u0643", "\u063C": "\u0643", "\u063D": "\u064A",
                       "\u063E": "\u064A", "\u063F": "\u064A", "\u0640": None})
_BAD_WASL = re.compile("(?<=[\u0621-\u064A\u0671-\u06D3])\u0671")


# ژ and ڨ also fail after an alef and under a shadda: there, use the nearest letters the engine
# always reads (ژ -> ج, ڨ -> گ).
_BAD_ZH = re.compile("(?<=[\u0627\u0671])[\u0698\u06A8]|[\u0698\u06A8](?=\u0651)")


def _engine_safe(s):
    if not s:
        return s
    s = _BAD_WASL.sub("\u0627", s.translate(_RARE))
    return _BAD_ZH.sub(lambda m: "\u062C" if m.group(0) == "\u0698" else "\u06AF", s)


def _engine_punct(s):
    return _engine_safe(s).translate(_PUNCT) if s else s


_ABD = re.compile("(?<![\u0621-\u064a])(\u0639\u0628\u062f)(?=\u0627\u0644[\u0621-\u064a]{2})")


def _arabic_core(out, diacritize, pt, ar_prep, is_cancelled=None):
    out = _step(pt.normalize_presentation_forms, out)
    out = _ABD.sub("\\1 ", out)        # names written joined: عبدالله -> عبد الله
    try:
        from .arabic import ar_textnorm
        out = _step(ar_textnorm.normalize, out)
    except Exception:
        pass
    if pt.is_pre_diacritized_spelling(out):
        return _step(ar_prep.mark_wasl, _alif_marks(_alif_fatha(out)))
    out = _step(_tanween_pre, out)
    if diacritize and _boot():
        try:
            from .arabic import ar_tashkeel
            bare, had = _strip_lone_tanween(out)
            with _lock:
                d = ar_tashkeel.diacritize_strict(bare, backend=ar_tashkeel.get_backend(), is_cancelled=is_cancelled)
            if d and d.strip():
                out = _restore_tanween(d, had)
        except Exception:
            pass
        out = _step(pt.dedupe_tanween, out)
        out = _step(pt.fix_bare_hamza_vowel, out)
        out = _step(pt.swap_tanween_fatha_alif, out)
    out = _step(_tanween_post, out)
    if not options.get("full_case", False):
        out = _step(ar_prep.light_case, out)     # everyday reading: no case endings mid-phrase
    if options.get("pausal_ending", True):
        out = _step(ar_prep.pausal_ending, out)  # our own waqf (PBT's dropped teh marbuta)
        out = _step(_tanween_post, out)
    return _step(ar_prep.mark_wasl, out)


# ---------------------------------------------------------------- Latin text without a secondary voice
# Only used when no non-Arabic RHVoice voice is installed at all: the Arabic voice then reads Latin
# text in a rough Arabic transliteration (fully vowelled, so it is spoken, not left silent or garbled).
_FATHA, _KASRA, _DAMMA, _SUKUN = "َ", "ِ", "ُ", "ْ"
_LAT_DI = {"sh": "ش", "ch": "تش", "th": "ث", "ph": "ف", "gh": "غ", "kh": "خ", "ck": "ك", "qu": "كو", "wh": "و"}
_LAT_C = {"b": "ب", "c": "ك", "d": "د", "f": "ف", "g": "گ", "h": "ه", "j": "ج", "k": "ك", "l": "ل", "m": "م",
          "n": "ن", "p": "پ", "q": "ك", "r": "ر", "s": "س", "t": "ت", "v": "ڤ", "w": "و", "x": "كْس", "z": "ز"}
# vowel letter -> (harakah on the consonant before, long-vowel letter or "")
_LAT_V = {"a": (_FATHA, "ا"), "e": (_KASRA, "ي"), "i": (_KASRA, "ي"), "o": (_DAMMA, "و"), "u": (_DAMMA, "و"),
          "y": (_KASRA, "ي"), "à": (_FATHA, "ا"), "é": (_KASRA, "ي"), "è": (_KASRA, "ي"), "ê": (_KASRA, "ي")}
_LAT_START = {_FATHA: "أَ", _KASRA: "إِ", _DAMMA: "أُ"}
LATIN_NAMES = {"a": "إِيه", "b": "بِي", "c": "سِي", "d": "دِي", "e": "إِي", "f": "إِف", "g": "جِي", "h": "إِتْش",
               "i": "آي", "j": "جِيه", "k": "كِيه", "l": "إِل", "m": "إِم", "n": "إِن", "o": "أُو", "p": "پِي",
               "q": "كْيُو", "r": "آر", "s": "إِس", "t": "تِي", "u": "يُو", "v": "ڤِي", "w": "دَبْلْيُو", "x": "إِكْس",
               "y": "وَاي", "z": "زِد"}


def _latin_word(w):
    if w.isupper() and 1 < len(w) <= 5:      # an acronym (NVDA, OK, PDF): letter by letter
        return " ".join(LATIN_NAMES.get(c, "") for c in w.lower())
    w = re.sub(r"(.)\1+", r"\1", w.lower())
    if len(w) > 3 and w.endswith("e") and w[-2] not in "aeiouy":
        w = w[:-1]                            # silent final e (file, make)
    if len(w) == 1 and w in LATIN_NAMES:
        return LATIN_NAMES[w]
    out, i = [], 0
    while i < len(w):
        two = w[i:i + 2]
        if two in _LAT_DI:
            cons, i = _LAT_DI[two], i + 2
        elif w[i] in _LAT_C:
            cons, i = _LAT_C[w[i]], i + 1
            if cons == "ك" and w[i - 1] == "c" and i < len(w) and w[i] in "eiy":
                cons = "س"
        elif w[i] in _LAT_V:
            h, long_ = _LAT_V[w[i]]; i += 1
            while i < len(w) and w[i] in _LAT_V:
                i += 1                        # a vowel group is one vowel
            out.append(_LAT_START[h] + (long_ if long_ != "ا" else "ا") if not out else long_)
            continue
        else:
            i += 1; continue
        # the consonant takes the vowel that follows it, or a sukun
        if i < len(w) and w[i] in _LAT_V:
            h, long_ = _LAT_V[w[i]]; i += 1
            while i < len(w) and w[i] in _LAT_V:
                i += 1
            last = i >= len(w)
            out.append(cons + h + (long_ if (last or len(w) <= 3) else ""))
        else:
            out.append(cons + _SUKUN)
    return "".join(out)


def latin_to_arabic(text):
    return re.sub(r"[A-Za-zÀ-ÿ]+", lambda m: _latin_word(m.group(0)), text)


def set_diacritizer(name):
    """Switch the diacritizer; falls back to Rawi if the other one cannot load."""
    options["diacritizer"] = name
    if _ready:
        try:
            from .arabic import ar_tashkeel
            if name != "rawi" and not ar_tashkeel._get(name):
                raise RuntimeError(name)
            ar_tashkeel.set_backend(name)
        except Exception:
            _log_error("Fasih: diacritizer %s unavailable, using Rawi" % name)
            options["diacritizer"] = "rawi"
            try:
                ar_tashkeel.set_backend("rawi")
            except Exception:
                pass
