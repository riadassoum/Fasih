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
options = {"read_emojis": True, "pausal_ending": True}


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
            ar_tashkeel.set_backend("rawi")
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
                ar_tashkeel.diacritize_strict("بسم", backend="rawi")
            except Exception:
                pass
    threading.Thread(target=run, daemon=True).start()


_TAN_AFTER_ALIF = re.compile("\u0627\u064b")                        # اً -> ًا (standard spelling)
_FATHA_TAN = re.compile("\u064e+(?=[\u0627\u0649]?\u064b)|(?<=\u064b)\u064e+")  # fatha next to tanween
_DUP = re.compile("([\u064b-\u0652])\\1+")                          # the same mark twice


def _tanween_pre(text):
    return _TAN_AFTER_ALIF.sub("\u064b\u0627", text)


def _tanween_post(text):
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


def _step(fn, text):
    try:
        out = fn(text)
        return out if isinstance(out, str) and out.strip() else text
    except Exception:
        return text


def process(text, diacritize=True):
    if not text or not text.strip():
        return text
    from .pbt import dictionary_manager as dm
    from .pbt import pbt_textnorm as pt
    from .arabic import ar_prep
    char_request = not diacritize
    out = _step(dm.apply_user_dictionary, text)
    out = _step(dm.apply_default_rules, out)
    # single characters (cursor movement, spelling): name them
    if (char_request or len(out.strip()) == 1) and not re.fullmatch(r"\s*[A-Za-z0-9]\s*", out):
        # Arabic letters, harakat and symbols get spoken names; Latin letters stay for the English voice
        name = _step(pt.get_single_char_name, out) if out.strip() else out
        if name != out:
            # a name is said on its own, so in its pausal form (شَدَّة, not شَدَّةٌ)
            return _step(ar_prep.mark_wasl, _step(ar_prep.pausal_ending, name))
    if options.get("read_emojis", True):
        out = _step(dm.replace_emojis, out)
    if not _AR.search(out):
        # numbers on their own (no Latin letters) are read as Arabic words
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
    out = _step(pt.normalize_presentation_forms, out)
    try:
        from .arabic import ar_textnorm
        out = _step(ar_textnorm.normalize, out)
    except Exception:
        pass
    if pt.is_pre_diacritized_spelling(out):
        return _step(ar_prep.mark_wasl, out)
    out = _step(_tanween_pre, out)
    if diacritize and _boot():
        try:
            from .arabic import ar_tashkeel
            bare, had = _strip_lone_tanween(out)
            with _lock:
                d = ar_tashkeel.diacritize_strict(bare, backend="rawi")
            if d and d.strip():
                out = _restore_tanween(d, had)
        except Exception:
            pass
        out = _step(pt.dedupe_tanween, out)
        out = _step(pt.fix_bare_hamza_vowel, out)
        out = _step(pt.swap_tanween_fatha_alif, out)
    out = _step(_tanween_post, out)
    if options.get("pausal_ending", True):
        out = _step(ar_prep.pausal_ending, out)  # our own waqf (PBT's dropped teh marbuta)
        out = _step(_tanween_post, out)
    return _step(ar_prep.mark_wasl, out)
