# Fasih's version of ClaritySynth's ar_tashkeel.py (third_party/claritysynth, Riad Assoum), edited directly since Fasih 2.0;
# it includes the Rawi client (Rawi runs in rawi_host) and Fasih's fixes.
# -*- coding: utf-8 -*-
"""Arabic diacritization (tashkeel) backends for ClaritySynth.

Two engines are offered:

  * libtashkeel - the libtashkeel engine (compiled), fast and robust
  * rawi        - the Rawi ensemble ONNX diacritizer
  * off         - no automatic diacritization

CRITICAL: listing the backends must NEVER load them. NVDA builds the
synthesizer settings (and the Tashkeel combo box) on its main GUI thread
when the user presses Ctrl+NVDA+S. Loading a 25 MB compiled extension and
ONNX sessions there froze NVDA for many seconds and could take it down
entirely. So `available()` is a pure, static list, and a backend is only
ever loaded lazily, off the GUI thread, the first time it is actually used
to speak.

libtashkeel and the Rawi model are bundled from the NabraTTS add-on by
"pbt", shared by Ilyas Dragonoid.
"""
import json
import os
import re
import threading
import unicodedata

def _import_data_paths():
    """Import the data_paths module whether we are loaded as part of the
    claritysynth package (normal NVDA) or as a loose module (tests)."""
    try:
        from . import data_paths as _dp
        return _dp
    except Exception:
        import os as _os, sys as _sys
        _d = _os.path.dirname(_os.path.abspath(__file__))
        if _d not in _sys.path:
            _sys.path.insert(0, _d)
        import data_paths as _dp
        return _dp



_here = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.join(_here, "lib")
_VOWELIZERS = os.path.join(_LIB, "vowelizers")


def _find_tashkeel_file(fname):
    """Locate a tashkeel model file, checking the persistent user data dir
    (downloads that survive updates) before the bundled locations."""
    cands = [os.path.join(_VOWELIZERS, fname), os.path.join(_LIB, "tts_arabic", "data", fname)]
    try:
        data_paths = _import_data_paths()
        for d in data_paths.search_dirs("vowelizers"):
            cands.append(os.path.join(d, fname))
        for d in data_paths.search_dirs(os.path.join("tts_arabic", "data")):
            cands.append(os.path.join(d, fname))
    except Exception:
        pass
    cands.append(os.path.join(_VOWELIZERS, fname))
    cands.append(os.path.join(_LIB, "tts_arabic", "data", fname))
    for c in cands:
        if os.path.exists(c):
            return c
    return None

BACKENDS = ("libtashkeel", "rawi", "shams", "catt", "catt_ed", "shakkelha", "shakkala", "off")
def _pick_default_backend():
    """CATT is the recommended default (bundled, high quality AND fast, never
    damages a sentence). Fall back to libtashkeel only if CATT's model file is
    somehow missing, so the add-on always has a working diacritizer."""
    try:
        if _find_tashkeel_file("catt_eo.onnx"):
            return "catt"
    except Exception:
        pass
    return "libtashkeel"


DEFAULT_BACKEND = _pick_default_backend()

_lock = threading.Lock()
_cache = {}          # backend id -> callable(text) -> str, or False if broken
_current = DEFAULT_BACKEND

_DIAC_RE = re.compile(r"[\u064B-\u065F\u0670\u0640]|[\u0610-\u061A]"
                      r"|[\u06D6-\u06ED]")


def available():
    """The selectable backends. libtashkeel + rawi are always offered
    (bundled); CATT is offered only if its model file is present (it is an
    optional download). This does at most a cheap os.path.exists — it never
    imports an extension or starts an ONNX session, so it is safe to call
    while NVDA builds its settings dialog on the GUI thread."""
    out = ["libtashkeel", "rawi"]
    if _Shams.available():
        out.append("shams")
    try:
        if _find_tashkeel_file("catt_eo.onnx"):
            out.append("catt")
    except Exception:
        pass
    # CATT Encoder-Decoder: the higher-accuracy CATT model. It ships as TWO
    # files (encoder + decoder), so offer it only when BOTH are present.
    try:
        if (_find_tashkeel_file("catt_ed_encoder.onnx")
                and _find_tashkeel_file("catt_ed_decoder.onnx")):
            out.append("catt_ed")
    except Exception:
        pass
    # Shakkelha / Shakkala are optional neural diacritizer downloads
    try:
        if _find_tashkeel_file("shakkelha.onnx"):
            out.append("shakkelha")
        if _find_tashkeel_file("shakkala.onnx"):
            out.append("shakkala")
    except Exception:
        pass
    out.append("off")
    return out


def set_backend(name):
    global _current
    if name in BACKENDS:
        _current = name


def get_backend():
    return _current


def _strip(text):
    return _DIAC_RE.sub("", text)


_HAMZA_FORMS = "\u0621\u0622\u0623\u0624\u0625\u0626"   # ء آ أ إ ؤ ئ
_HAMZA_BASE = {
    "\u0622": "\u0627",   # آ -> ا
    "\u0623": "\u0627",   # أ -> ا
    "\u0625": "\u0627",   # إ -> ا
    "\u0624": "\u0648",   # ؤ -> و
    "\u0626": "\u064A",   # ئ -> ي
}


def _is_malformed_diacritization(orig_word, diac_word):
    """True when a diacritizer produced an impossible mark placement for a
    word — the tell-tale of CATT (and others) being fed too little context,
    which happens with single isolated words. Examples actually observed:
    ما -> مِاِ, هو -> هِوِ. In such cases a short vowel lands directly
    AFTER a bare alif (ا cannot carry a following short vowel), or a kasra
    lands on a final long waw. When detected we throw the diacritization away
    and let the phonemizer read the BARE word, which its own rules pronounce
    correctly. Conservative: only structurally impossible placements fire, so
    correct words are never touched."""
    if not diac_word:
        return False
    ALIF = "\u0627"
    WAW = "\u0648"
    YEH = "\u064A"
    DAMMA = "\u064F"
    KASRA = "\u0650"
    SHORT = "\u064E\u064F\u0650"
    for i, ch in enumerate(diac_word):
        nxt = diac_word[i + 1] if i + 1 < len(diac_word) else ""
        # a short vowel directly after a bare alif is impossible (مِاِ)
        if ch == ALIF and nxt and nxt in SHORT:
            return True
        # kasra on a waw — weak-letter mismatch (هِوِ)
        if ch == WAW and nxt == KASRA:
            return True
    # a final glide carrying a doubled/mismatched short vowel: هُوُ (damma on
    # the waw) or ...يِ (kasra on the yeh) — the word was mis-ended
    if len(diac_word) >= 2 and diac_word[-2] == WAW and diac_word[-1] == DAMMA:
        return True
    if len(diac_word) >= 2 and diac_word[-2] == YEH and diac_word[-1] == KASRA:
        return True
    return False


def _reject_malformed_words(original, diacritized):
    """Word-by-word: replace a malformed diacritized word with the ORIGINAL
    bare word so the phonemizer handles it. Aligned by whitespace; if word
    counts differ, the text is left unchanged."""
    if not original or not diacritized:
        return diacritized
    o = original.split(" ")
    d = diacritized.split(" ")
    if len(o) != len(d):
        return diacritized
    out = []
    for ow, dw in zip(o, d):
        out.append(ow if _is_malformed_diacritization(ow, dw) else dw)
    return " ".join(out)


def _fix_naa_pronoun(original, diacritized):
    """Fix the 1st-person-plural ـنا ending (بيتنا, صديقنا, كتابنا) when a
    diacritizer mistakes it for a tanween-bearing alif.

    Tanween-fath is written mark+alif (ـًا), which to a model can look like a
    word that ends in a bare alif. So a word whose ORIGINAL (undiacritized)
    form ends in ن+ا (nun + alif) sometimes comes back with a TANWEEN on the
    letter before the alif and the نـ swallowed — صديقنا read as صديقًا
    ('sadeeqan') or صديقٌ instead of صديقُنا ('sadeequnaa'). Every backend
    except Libtashkeel has shown this.

    The rule is safe and purely corrective: only when the raw word truly ends
    in ـنا do we (a) strip any tanween that landed on the tail, and (b)
    guarantee an explicit ن + ا with a fatha on the ن if it lost its vowel.
    Words that do not end in ـنا are left completely untouched."""
    if not original or not diacritized:
        return diacritized
    NUN = "\u0646"
    ALIF = "\u0627"
    FATHA = "\u064E"
    TANWEENS = ("\u064B", "\u064C", "\u064D")
    MARKS = set("\u064B\u064C\u064D\u064E\u064F\u0650\u0651\u0652\u0670")

    def _bare(s):
        return "".join(c for c in s if c not in MARKS)

    def fix_word(o, d):
        ob = _bare(o)
        # must end in nun + alif in the original skeleton
        if not (ob.endswith(NUN + ALIF)):
            return d
        chars = list(d)
        # find index of the last ALIF
        try:
            ai = len(chars) - 1 - chars[::-1].index(ALIF)
        except ValueError:
            return d
        # Anything AFTER that final alif is a stray mark the model misplaced
        # (e.g. عَنْدَنْاَ where a fatha landed after the alif) — drop it.
        tail_after_alif = [c for c in chars[ai + 1:] if c not in MARKS]
        # collect the marks immediately before that alif
        j = ai - 1
        marks_before_alif = []
        while j >= 0 and chars[j] in MARKS:
            marks_before_alif.append(chars[j])
            j -= 1
        if j < 0:
            return d
        if chars[j] != NUN:
            # the nun was swallowed / replaced — likely a tanween-alif.
            # Strip trailing tanweens on the tail letter and append نَا.
            head = [c for c in chars[:j + 1] if c not in TANWEENS]
            return "".join(head) + NUN + FATHA + ALIF + "".join(tail_after_alif)
        # The nun IS present. Canonicalise the ending to exactly ن + fatha +
        # alif. This fixes BOTH failure modes: a tanween mistaken for ـنا
        # (صديقًا) AND a scrambled/rogue mark order such as نْاَ (nun+sukun,
        # alif+fatha) seen in عَنْدَنْاَ / كِتَاِبِنَاِ, which made the ending
        # sound wrong or drop. The vowel on the nun must be a fatha (نَا), the
        # letter before the nun must not carry a tanween, and nothing may sit
        # on or after the alif.
        pre = [c for c in chars[:j] if c not in TANWEENS]
        new = pre + [NUN, FATHA, ALIF] + tail_after_alif
        return "".join(new)

    # apply word by word, aligning on whitespace
    o_words = original.split(" ")
    d_words = diacritized.split(" ")
    if len(o_words) != len(d_words):
        # counts differ (diacritizer merged/split) — fall back to a global
        # single-word fix only when the whole thing is one token
        if len(o_words) == 1 and len(d_words) == 1:
            return fix_word(original, diacritized)
        return diacritized
    return " ".join(fix_word(o, d) for o, d in zip(o_words, d_words))


def _restore_hamza(original, diacritized):
    """Safety net for models whose vocabulary has no composed hamza letters
    (Rawi). Given full context they reproduce hamza correctly, but a short
    or isolated word can come back flattened (سأل -> سَالْ, مؤمن -> مومن).
    Walk both strings and, wherever the original had a hamza form and the
    output has its bare base letter, restore the original letter."""
    if not original or not diacritized:
        return diacritized
    # The reverse also happens: the model writes a hamza on a bare waw or yaa
    # that the writer never wrote (البور -> البؤر, كاين -> كائن). A bare
    # و / ي / ى in the text stays bare.
    orig_letters = [c for c in original
                    if not ("\u064B" <= c <= "\u0652" or c == "\u0670")]
    out = []
    oi = 0
    bare = False                    # the last letter was a hamza we took off
    for ch in diacritized:
        if "\u064B" <= ch <= "\u0652" or ch == "\u0670":
            if not bare:
                out.append(ch)      # a diacritic: keep
            continue                # (its mark was meant for the hamza: a long vowel has none)
        if ch in ("\u0654", "\u0655") and oi and orig_letters[oi - 1] in "\u0648\u064A\u0649":
            # a hamza added on a bare waw/yaa: drop it with its vowel mark
            while out and ("\u064B" <= out[-1] <= "\u0652"):
                out.pop()
            bare = True
            continue
        if ch in ("\u0653", "\u0654", "\u0655"):
            # stray combining hamza/maddah: the composed letter is already
            # restored, so dropping this avoids a doubled hamza (مُؤَٔمِن).
            continue
        bare = False
        if oi < len(orig_letters):
            o = orig_letters[oi]
            if o in _HAMZA_FORMS and (ch == _HAMZA_BASE.get(o) or ch == o):
                out.append(o)
            elif o in "\u0648\u064A\u0649" and ch in "\u0624\u0626":
                out.append(o)
                bare = True
            else:
                out.append(ch)
            oi += 1
        else:
            out.append(ch)
    res = unicodedata.normalize("NFC", "".join(out))
    # a hamza above alif never takes kasra, one below never takes fatha/damma
    res = re.sub("\u0623\u0650", "\u0623\u064E", res)
    res = re.sub("\u0625[\u064E\u064F]", "\u0625\u0650", res)
    # مئة never has a shadda (the model sometimes writes مِئَّة)
    res = re.sub("(\u0645\u0650?\u0626\u064E?)\u0651", "\\1", res)
    res = re.sub("(\u0645\u0650\u0626)\u0652(?=\u062A)", "\\1\u064E", res)     # مِئْتان -> مِئَتان
    # الاستخدام / الانتظار: the alif after al- is a hamzat wasl with kasra, never fatha
    res = re.sub("([\u0671\u0627]\u0644)\u064E(\u0627)(?=[\u0621-\u064A]\u0652)", "\\1\u0650\\2", res)
    return res


class _Rawi(object):
    """Rawi ensemble diacritizer, run in a separate process (rawi_host.exe).

    NVDA never loads numpy or onnxruntime: other add-ons ship their own copies,
    and two copies of those native libraries in one process can hang the whole
    screen reader. The helper is started on first use, every call has a time
    limit, and if the helper hangs or dies it is killed and restarted later,
    while the text is spoken without added diacritics. So speech can never block."""

    TIMEOUT = 3.0            # first call (the model is still loading); 1 s afterwards

    def __init__(self, onnx_path):
        import threading
        vocab_path = os.path.join(os.path.dirname(onnx_path), "vocab.json")
        with open(vocab_path, "r", encoding="utf-8") as f:
            vocab = json.load(f)
        self.char_to_idx = vocab["char_to_idx"]
        self.diac_to_idx = vocab["diac_to_idx"]
        self.idx_to_diac = {v: k for k, v in self.diac_to_idx.items()}
        self._unk_id = self.char_to_idx.get("<UNK>", 1)
        self._model = onnx_path
        self._proc = None
        self._lock = threading.Lock()
        self._fails = 0
        self._res_cache = None
        self._start()

    def _paths(self):
        base = os.path.dirname(os.path.dirname(os.path.abspath(self._model)))   # arabic/lib
        if os.name == "nt":
            return (os.path.join(base, "rawi_host.exe"),
                    os.path.join(base, "onnxruntime", "capi", "onnxruntime.dll"))
        return (os.path.join(base, "rawi_host"),
                os.environ.get("RAWI_ORT_LIB", "libonnxruntime.so"))

    def _start(self):
        import subprocess
        exe, lib = self._paths()
        flags = 0x08000000 if os.name == "nt" else 0          # CREATE_NO_WINDOW
        self._proc = subprocess.Popen(
            [exe, lib, self._model], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, creationflags=flags, bufsize=0)

    def _kill(self):
        self._warm = False
        p, self._proc = self._proc, None
        if p is not None:
            try:
                p.kill()
            except Exception:
                pass

    def _exchange(self, payload, n):
        """Send one request and read the reply on a helper thread, with a time limit."""
        import threading
        import struct
        box = {}
        proc = self._proc

        def io():
            try:
                proc.stdin.write(payload)
                proc.stdin.flush()
                head = self._read(proc.stdout, 4)
                (m,) = struct.unpack("<I", head)
                if m == 0xFFFFFFFF or m != n:
                    box["err"] = True
                    return
                box["res"] = struct.unpack("<%di" % n, self._read(proc.stdout, 4 * n)) if n else ()
            except Exception:
                box["err"] = True
        t = threading.Thread(target=io, daemon=True)
        t.start()
        t.join(self.TIMEOUT if not getattr(self, "_warm", False) else 1.0)
        if t.is_alive() or "res" not in box:
            self._kill()                  # hung or broken: drop it, the io thread ends with the pipe
            return None
        return box["res"]

    @staticmethod
    def _read(f, n):
        buf = b""
        while len(buf) < n:
            chunk = f.read(n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return buf

    def _classes(self, ids):
        import struct
        with self._lock:
            if self._proc is None or self._proc.poll() is not None:
                if self._fails >= 5:
                    return None          # helper keeps failing: speak without added diacritics
                try:
                    self._start()
                except Exception:
                    self._fails += 1
                    return None
            res = self._exchange(struct.pack("<I%dq" % len(ids), len(ids), *ids), len(ids))
            if res is None:
                self._fails += 1
            else:
                self._fails = 0
                self._warm = True
            return res

    def _encode(self, text):
        ids = []
        for ch in text:
            idx = self.char_to_idx.get(ch)
            if idx is None:
                nfc = unicodedata.normalize("NFC", ch)
                idx = self.char_to_idx.get(nfc, self._unk_id)
            ids.append(idx)
        return ids

    def _decode(self, base, cls):
        out = []
        for i, ch in enumerate(base):
            out.append(ch)
            if i < len(cls):
                d = int(cls[i])
                if d:
                    s = self.idx_to_diac.get(d, "")
                    if s:
                        out.append(s)
        return "".join(out)

    def __call__(self, text):
        if not text or not text.strip():
            return text
        cache = self._res_cache
        if cache is None:
            from collections import OrderedDict
            cache = self._res_cache = OrderedDict()
        hit = cache.get(text)
        if hit is not None:
            cache.move_to_end(text)
            return hit
        stripped = _strip(unicodedata.normalize("NFD", text))
        if not stripped:
            return text
        ids = self._encode(stripped)
        if not ids:
            return text
        cls = self._classes(ids)
        if cls is None:
            return text
        result = _restore_hamza(text, self._decode(stripped, cls))
        if len(text) <= 200:
            cache[text] = result
            while len(cache) > 512:
                cache.popitem(last=False)
        return result


class _Shams(object):
    """Barq, PlanetBlindTech's tashkeel engine: lexicon + word bigrams + small trees, very fast."""
    DIR = os.path.join(_LIB, "tashkeel_engine")
    LIBNAME = "tashkeel_engine.dll" if os.name == "nt" else "libtashkeel.so"
    MODEL = "tashkeel_engine.tash"

    @classmethod
    def available(cls):
        return os.path.exists(os.path.join(cls.DIR, cls.LIBNAME)) and os.path.exists(os.path.join(cls.DIR, cls.MODEL))

    def __init__(self):
        import ctypes, threading
        self._ct = ctypes
        L = ctypes.CDLL(os.path.join(self.DIR, self.LIBNAME))
        L.tashkeel_create.restype = ctypes.c_void_p
        L.tashkeel_create.argtypes = [ctypes.c_char_p]
        L.tashkeel_diacritize.restype = ctypes.c_void_p
        L.tashkeel_diacritize.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        L.tashkeel_free_str.argtypes = [ctypes.c_void_p]
        L.tashkeel_set_restore_spelling.argtypes = [ctypes.c_void_p, ctypes.c_int]
        h = L.tashkeel_create(os.path.join(self.DIR, self.MODEL).encode("utf-8"))
        if not h:
            raise RuntimeError("tashkeel engine: model did not load")
        L.tashkeel_set_restore_spelling(h, 0)     # never change letters, only add marks
        self._L, self._h, self._lock = L, h, threading.Lock()

    def __call__(self, text):
        b = text.encode("utf-8"); n = self._ct.c_size_t()
        with self._lock:
            p = self._L.tashkeel_diacritize(self._h, b, len(b), self._ct.byref(n))
            if not p:
                return text
            try:
                return self._ct.string_at(p, n.value).decode("utf-8", "replace")
            finally:
                self._L.tashkeel_free_str(p)


def _load(name):
    """Load a backend lazily. Returns a callable, or False if unusable.
    Never called from the GUI thread."""
    try:
        if name != "rawi":        # rawi runs out of process: no numpy/onnxruntime here
            from . import _libboot
            _libboot.boot()
    except Exception:
        pass

    if name == "libtashkeel":
        import sys as _sys
        if _LIB not in _sys.path:
            _sys.path.append(_LIB)
        if hasattr(os, "add_dll_directory") and os.path.isdir(_LIB):
            try:
                os.add_dll_directory(_LIB)
            except Exception:
                pass
        import pylibtashkeel
        model = os.path.join(_LIB, "libtashkeel_model.bin")
        if os.path.exists(model):
            for loader in ("tashkeel_load", "load", "load_model"):
                fn = getattr(pylibtashkeel, loader, None)
                if callable(fn):
                    try:
                        fn(model)
                        break
                    except Exception:
                        pass
        _fn = None
        for cand in ("tashkeel", "diacritize", "vocalize"):
            f = getattr(pylibtashkeel, cand, None)
            if callable(f):
                _fn = f
                break
        if _fn is None:
            return False

        def _run(text):
            return _fn(text)
        out = _run("نص")            # surface a failure now, not mid-speech
        if not isinstance(out, str):
            return False
        return _run

    if name == "shams":
        r = _Shams()
        r("نص")
        return r

    if name == "rawi":
        path = _find_tashkeel_file("rawi_fasih.onnx")
        if not path:
            return False
        r = _Rawi(path)
        r("نص")
        return r

    if name == "catt":
        # CATT is a transformer with its OWN tokenizer — NabraTTS broke it by
        # loading it as a Rawi model. We use the proper CATT class bundled in
        # tts_arabic, so it actually works.
        path = _find_tashkeel_file("catt_eo.onnx")
        if not path:
            return False
        try:
            import sys as _sys
            if _LIB not in _sys.path:
                _sys.path.append(_LIB)
            from tts_arabic.vocalizer.models.catt.network import CATTModel
        except Exception:
            return False
        model = CATTModel(sd_path=path)

        def _run(text):
            return model.predict(text)
        out = _run("نص")
        if not isinstance(out, str):
            return False
        return _run

    if name == "catt_ed":
        # CATT Encoder-Decoder — the higher-accuracy CATT model. Unlike the
        # encoder-only catt (a single ONNX), the ED model is a pair of ONNX
        # files (encoder + decoder) run with an autoregressive decode loop.
        # The inference is pure numpy + onnxruntime (no PyTorch), so it runs
        # in NVDA. Both files must be present (bundled or downloaded).
        enc = _find_tashkeel_file("catt_ed_encoder.onnx")
        dec = _find_tashkeel_file("catt_ed_decoder.onnx")
        if not (enc and dec):
            return False
        try:
            import sys as _sys
            if _LIB not in _sys.path:
                _sys.path.append(_LIB)
            # Import the ED runner DIRECTLY from its file rather than via
            # "tts_arabic.vocalizer.models.catt...". The dotted import
            # executes tts_arabic/__init__ -> models -> vocalizer.models ->
            # shakkelha -> numpy, so ANY failure anywhere in that chain made
            # CATT ED silently unavailable and every sentence came back
            # undiacritized. The ED code itself needs only numpy +
            # onnxruntime, so it is loaded as its own small package.
            import importlib.util as _ilu
            import os as _os
            _catt_dir = _os.path.join(_LIB, "tts_arabic", "vocalizer",
                                      "models", "catt")
            _pkg = "claritysynth_catt_ed"
            if _pkg not in _sys.modules:
                _spec = _ilu.spec_from_file_location(
                    _pkg, _os.path.join(_catt_dir, "__init__.py"),
                    submodule_search_locations=[_catt_dir])
                _mod = _ilu.module_from_spec(_spec)
                _sys.modules[_pkg] = _mod
                _spec.loader.exec_module(_mod)
            _sub = _ilu.spec_from_file_location(
                _pkg + ".catt_onnx_ed",
                _os.path.join(_catt_dir, "catt_onnx_ed.py"))
            _m = _ilu.module_from_spec(_sub)
            _sys.modules[_pkg + ".catt_onnx_ed"] = _m
            _sub.loader.exec_module(_m)
            CATTEncoderDecoder = _m.CATTEncoderDecoder
        except Exception:
            return False
        try:
            model = CATTEncoderDecoder(encoder_path=enc, decoder_path=dec)
        except Exception:
            return False

        def _run(text):
            # CATT ED decodes autoregressively: each character triggers a full
            # decoder pass over the growing sequence, so cost grows with the
            # SQUARE of the input length. A long paragraph therefore took many
            # seconds and, because the loop holds Python's GIL, NVDA appeared
            # to freeze and stopped responding even to arrow keys. Splitting
            # the text into shorter pieces (on punctuation, then by length)
            # keeps every call small and bounded, so it stays responsive. The
            # pieces are diacritized independently and rejoined; ED is robust
            # at the clause level, so quality is unaffected.
            if not text:
                return text
            import re as _re
            _LIMIT = 50          # max characters handed to one ED decode (O(n^2))
            # split on Arabic/Latin sentence punctuation, keeping delimiters
            parts = _re.split(r"([\.!\?؟؛;،\n]+)", text)
            pieces = []
            for part in parts:
                if not part:
                    continue
                if len(part) <= _LIMIT:
                    pieces.append(part)
                else:
                    # still too long (no punctuation): break on spaces so no
                    # single ED call exceeds the limit
                    words = part.split(" ")
                    buf = ""
                    for w in words:
                        if buf and len(buf) + 1 + len(w) > _LIMIT:
                            pieces.append(buf)
                            buf = w
                        else:
                            buf = (buf + " " + w) if buf else w
                    if buf:
                        pieces.append(buf)
            out = []
            for piece in pieces:
                if piece.strip() and any("\u0621" <= c <= "\u064A"
                                         for c in piece):
                    try:
                        out.append(model.do_tashkeel(piece))
                    except Exception:
                        out.append(piece)
                else:
                    out.append(piece)     # punctuation / whitespace as-is
            return "".join(out)
        try:
            probe = _run("نص")
        except Exception:
            return False
        if not isinstance(probe, str):
            return False
        return _run

    if name in ("shakkelha", "shakkala"):
        # neural diacritizers in the arabic_vocalizer package; the model file
        # must be present (bundled or downloaded)
        fname = name + ".onnx"
        if not _find_tashkeel_file(fname):
            return False
        try:
            import sys as _sys
            if _LIB not in _sys.path:
                _sys.path.append(_LIB)
            from arabic_vocalizer import vocalize as _v
        except Exception:
            return False

        def _run(text):
            return _v(text, model=name)
        try:
            probe = _run("نص")
        except Exception:
            return False
        if not isinstance(probe, str):
            return False
        return _run

    return False


def _get(name):
    if name in _cache:
        return _cache[name]
    with _lock:
        if name in _cache:
            return _cache[name]
        try:
            _cache[name] = _load(name)
        except Exception:
            _cache[name] = False
        return _cache[name]


def preload():
    """Warm the selected backend off the GUI thread, so the first spoken
    word is not delayed by a cold model."""
    if _current != "off":
        _get(_current)


def _split_sentences(text, hard_limit=800):
    """Split Arabic text into full SENTENCES for diacritization.

    A sentence ends at . ! ? or their Arabic equivalents (؟ ! 。) plus any
    trailing quote/space. Commas and semicolons do NOT split — a whole
    sentence is diacritized together so it sounds right, which is what the
    diacritizer needs for correct case endings. Only if a single sentence is
    longer than hard_limit (a very long comma-only run with no full stop) do
    we fall back to splitting that one sentence on commas/semicolons, and if
    it is STILL too long, on spaces — so no single call is unbounded. The
    pieces reassemble into exactly the original text (all delimiters kept)."""
    import re as _re
    # keep the sentence-ending punctuation attached to its sentence
    parts = _re.split(r"([\.!\?؟。]+[\"'\u00bb\u201d\)\]]*\s*)", text)
    # re.split with one capture group yields [body, delim, body, delim, ...]
    sentences = []
    buf = ""
    for i, seg in enumerate(parts):
        if seg is None:
            continue
        buf += seg
        # odd indices are the delimiters -> a sentence just ended
        if i % 2 == 1:
            if buf:
                sentences.append(buf)
            buf = ""
    if buf:
        sentences.append(buf)
    # now enforce the hard limit on any over-long sentence
    out = []
    for sent in sentences:
        if len(sent) <= hard_limit:
            out.append(sent)
            continue
        # too long and has no full stop: fall back to comma/semicolon
        sub = _re.split(r"([،؛,;]+\s*)", sent)
        piece = ""
        for j, seg in enumerate(sub):
            if not seg:
                continue
            if len(piece) + len(seg) <= hard_limit:
                piece += seg
            else:
                if piece:
                    out.append(piece)
                # the seg itself may still be too long -> split on spaces
                if len(seg) > hard_limit:
                    p2 = ""
                    for w in _re.findall(r"\S+\s*|\s+", seg):
                        while len(w) > hard_limit:
                            if p2:
                                out.append(p2)
                                p2 = ""
                            out.append(w[:hard_limit])
                            w = w[hard_limit:]
                        if p2 and len(p2) + len(w) > hard_limit:
                            out.append(p2)
                            p2 = w
                        else:
                            p2 += w
                    piece = p2
                else:
                    piece = seg
        if piece:
            out.append(piece)
    return out


def _diac_run_chunked(fn, run_text, limit=None, is_cancelled=None):
    """Diacritize one Arabic run, one full SENTENCE at a time.

    Splitting per sentence (not per fixed character window) means each call to
    the model gets a complete sentence for correct context, the whole
    paragraph is covered start to finish (no mid-paragraph drop), and — for
    the neural voice — the first sentence is ready quickly instead of waiting
    for the entire paragraph. Very long comma-only sentences are split further
    only as a safety valve (see _split_sentences).

    is_cancelled, if given, is polled between sentences so a slow diacritizer
    (CATT ED especially) stops promptly when the user moves on — otherwise a
    burst of arrow keys would queue behind an un-abortable diacritization and
    freeze the reader."""
    if not run_text or not run_text.strip():
        return run_text
    # A short run (a single sentence or less) goes straight to the model.
    if len(run_text) <= 800 and not any(
            c in ".!?؟。" for c in run_text[:-2]):
        return fn(run_text)
    out = []
    for piece in _split_sentences(run_text, hard_limit=800):
        if is_cancelled is not None and is_cancelled():
            # stop early; return what we have plus the rest un-diacritized so
            # the text is never lost, just not vocalized
            out.append(piece)
            continue
        if piece.strip() and any("\u0621" <= c <= "\u064A" for c in piece):
            try:
                r = fn(piece)
                out.append(r if (r and r.strip()) else piece)
            except Exception:
                out.append(piece)
        else:
            out.append(piece)
    return "".join(out)


def diacritize_strict(text, backend=None, is_cancelled=None):
    """Diacritize with the chosen backend, then apply ClaritySynth's shared
    post-diacritization corrections (bracket cleanup, correct الله vowelling,
    impossible-shadda removal) so EVERY consumer — the speech engine, the
    Tools-menu window, and NVDA+J live tashkeel — produces byte-for-byte the
    same text. This is the single choke point that keeps them from diverging."""
    _g2p = None
    try:
        from . import ar_g2p as _g2p
    except Exception:
        try:
            import ar_g2p as _g2p
        except Exception:
            _g2p = None
    # pre-diacritization fixes (leading-zero digit runs) so the Tools window
    # and NVDA+J read 0404 as "صفر أربعة صفر أربعة" like the speech path
    if _g2p is not None and hasattr(_g2p, "apply_pre_diacritization_fixes"):
        try:
            text = _g2p.apply_pre_diacritization_fixes(text)
        except Exception:
            pass
    out = _diacritize_strict_raw(text, backend, is_cancelled)
    if _g2p is not None and hasattr(_g2p, "apply_post_diacritization_fixes"):
        try:
            fixed = _g2p.apply_post_diacritization_fixes(out)
            if fixed and fixed.strip():
                return fixed
        except Exception:
            pass
    return out


def _diacritize_strict_raw(text, backend=None, is_cancelled=None):
    """Fully diacritize `text` using ONLY `backend` (no fallback to a
    different library), exactly the way the speech engine would read it.

    The text is split into contiguous ARABIC runs and non-Arabic runs. Each
    Arabic run is diacritized AS A WHOLE (full sentence context — critical
    for CATT and the neural diacritizers, and the reason word-by-word gave
    incomplete/garbled output). Non-Arabic runs (English, digits, symbols)
    pass through untouched. The result matches what the chosen voice would
    pronounce, in text form. This is what the Tools-menu window uses."""
    if not text:
        return text
    # A single isolated Arabic LETTER (character navigation, "spell current
    # character", one-letter input) must NOT be diacritized — a diacritizer
    # given one letter with no context invents a random mark (ا -> اْ, ب ->
    # بَ). Leave a lone letter exactly as written; the synth reads the bare
    # letter name correctly. NOTE: this is deliberately limited to a SINGLE
    # letter. Short whole WORDS (هو, ما, نحن) are NOT skipped here — they are
    # diacritized correctly by routing short runs to Rawi below, which is the
    # root fix for the old "isolated words read without tashkeel" behaviour.
    _letters_only = [c for c in text if "\u0621" <= c <= "\u064A"]
    if len(_letters_only) <= 1 and len(text.strip()) <= 1:
        return text
    name = backend or _current
    if name == "off":
        return text
    fn = _get(name)
    if not fn:
        return text

    import re
    # split into runs that are either mostly-Arabic or non-Arabic, keeping
    # everything so the output reassembles exactly. A run boundary is where
    # we cross between Arabic-script characters and non-Arabic ones (spaces
    # and Arabic punctuation stay attached to the Arabic side so context is
    # preserved).
    def _is_ar(ch):
        return ("\u0600" <= ch <= "\u06FF") or ("\u0750" <= ch <= "\u077F") \
            or ("\uFB50" <= ch <= "\uFDFF") or ("\uFE70" <= ch <= "\uFEFF")

    runs = []
    cur = []
    cur_ar = None
    for ch in text:
        # whitespace, punctuation and digits join whichever run they are in (keeps sentences whole)
        if not ch.isalpha() and unicodedata.category(ch)[0] != "M":
            cur.append(ch)
            continue
        a = _is_ar(ch)
        if cur_ar is None:
            cur_ar = a
            cur.append(ch)
        elif a == cur_ar:
            cur.append(ch)
        else:
            runs.append((cur_ar, "".join(cur)))
            cur = [ch]
            cur_ar = a
    if cur:
        runs.append((bool(cur_ar), "".join(cur)))

    out = []
    # ROOT FIX for isolated / short words: CATT (and CATT-ED) reliably mangle
    # a run of one or two words because they have no sentence context to lean
    # on — هو->هِوِ, ما->مِاِ, نحن->نْحْنَ. Rawi, by contrast, diacritizes short
    # words CORRECTLY (هو->هُوَ, ما->مَا, نحن->نَحْنَ). So rather than blank the
    # marks (the old band-aid) or feed fake context, we diacritize short Arabic
    # runs with Rawi and long runs with the user's chosen backend. This removes
    # the word-by-word-without-tashkeel behaviour at its source.
    _rawi_fn = None
    if name in ("catt", "catt_ed"):
        try:
            _rawi_fn = _get("rawi")
        except Exception:
            _rawi_fn = None

    def _word_count(s):
        return len([w for w in s.split() if w.strip()])

    for is_ar_run, chunk in runs:
        if not is_ar_run or not chunk.strip():
            out.append(chunk)             # keep non-Arabic exactly
            continue
        run_fn = fn
        # short run + a mangling backend + Rawi present -> use Rawi for it
        if _rawi_fn is not None and _word_count(chunk) <= 2:
            run_fn = _rawi_fn
        try:
            d = _diac_run_chunked(run_fn, chunk, is_cancelled=is_cancelled)
        except Exception:
            d = None
        if d and isinstance(d, str) and d.strip():
            d = _fix_naa_pronoun(chunk, d)
            # Respect the user's OWN harakat: where they marked a letter, keep
            # their mark; the model only fills in what they left bare. So
            # "أُكلت التفاحةُ" keeps the damma the user typed instead of being
            # re-read from scratch. Same behaviour as the speech path.
            try:
                try:
                    from . import ar_g2p
                except Exception:
                    import ar_g2p
                if hasattr(ar_g2p, "_merge_user_marks"):
                    merged = []
                    o_words = chunk.split(" ")
                    d_words = d.split(" ")
                    if len(o_words) == len(d_words):
                        for ow, dw in zip(o_words, d_words):
                            merged.append(
                                ar_g2p._merge_user_marks(ow, dw))
                        d = " ".join(merged)
            except Exception:
                pass
            out.append(d)
        else:
            out.append(chunk)
    return "".join(out)


def diacritize_text(text):
    """Diacritize with the selected backend, falling back to the other
    working one if it fails. Returns None if nothing works."""
    if _current == "off" or not text:
        return None
    order = [_current] + [b for b in BACKENDS if b not in (_current, "off")]
    for name in order:
        fn = _get(name)
        if not fn:
            continue
        try:
            out = fn(text)
            if out and any(("\u064B" <= c <= "\u0652") or c == "\u0670"
                           or ("\u0653" <= c <= "\u065F") for c in out):
                out = _fix_naa_pronoun(text, out)
                out = _reject_malformed_words(text, out)
                return out
        except Exception:
            _cache[name] = False      # never retry a broken backend
    return None


def reset_sessions():
    """Drop cached diacritizer backends so the next call rebuilds them with
    the current thread settings (used after speed recalibration)."""
    global _cache
    try:
        _cache.clear()
    except Exception:
        _cache = {}
