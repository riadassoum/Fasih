"""Swap ClaritySynth's in-process Rawi (numpy + onnxruntime) for the out-of-process client.
usage: patch_tashkeel.py ar_tashkeel.py rawi_client.py"""
import sys
p, c = sys.argv[1], sys.argv[2]
s = open(p, encoding="utf-8").read()
a = s.index("class _Rawi(object):"); b = s.index("def _load(name):")
s = s[:a] + open(c, encoding="utf-8").read() + s[b:]
old = "        from . import _libboot\n        _libboot.boot()\n"
assert old in s
s = s.replace(old, "        if name != \"rawi\":        # rawi runs out of process: no numpy/onnxruntime here\n            from . import _libboot\n            _libboot.boot()\n", 1)
# Punctuation and digits must not split a sentence into separately diacritized runs: before,
# "جميلا: كما" and "حق. ثم" sent "...جميلا" and "هذا حق" to the model without the rest of the
# sentence, so the word before the colon or full stop was vowelled as if the text ended there.
# Only a change of script (Latin and other letters) starts a new run now.
old_run = """        # whitespace joins whichever run it is in (keeps sentences whole)
        if ch.isspace():
            cur.append(ch)
            continue"""
assert old_run in s, "run splitter not found"
s = s.replace(old_run, """        # whitespace, punctuation and digits join whichever run they are in (keeps sentences whole)
        if not ch.isalpha() and unicodedata.category(ch)[0] != "M":
            cur.append(ch)
            continue""", 1)
# 1.1: the space fallback dropped the space at each cut, and one word longer than the
# limit went through whole. Keep every character (spaces stay on the piece before the
# cut) and cut any over-long word at the limit.
old_sp = """                    words = seg.split(" ")
                    p2 = ""
                    for w in words:
                        if p2 and len(p2) + 1 + len(w) > hard_limit:
                            out.append(p2)
                            p2 = w
                        else:
                            p2 = (p2 + " " + w) if p2 else w
                    piece = p2"""
assert old_sp in s, "space splitter not found"
s = s.replace(old_sp, """                    p2 = ""
                    for w in _re.findall(r"\\S+\\s*|\\s+", seg):
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
                    piece = p2""", 1)
# 1.1: look in Fasih's own bundled models first, so a ClaritySynth install (or its
# downloads folder) can never hand Fasih a different model file.
old_fp = """    cands = []
    try:
        data_paths = _import_data_paths()"""
assert old_fp in s, "model search not found"
s = s.replace(old_fp, """    cands = [os.path.join(_VOWELIZERS, fname), os.path.join(_LIB, "tts_arabic", "data", fname)]
    try:
        data_paths = _import_data_paths()""", 1)
compile(s, p, "exec")
open(p, "w", encoding="utf-8").write(s)
# 1.3: second diacritizer, Barq (برق), PlanetBlindTech's tashkeel engine (C++, GPL, by Shamseddin; fine-tuned
# for Fasih). Loaded in process through ctypes: one DLL and one model file, no numpy.
s = s.replace('BACKENDS = ("libtashkeel", "rawi",', 'BACKENDS = ("libtashkeel", "rawi", "shams",', 1)
old_b = '    out = ["libtashkeel", "rawi"]\n'
assert old_b in s, "backend list not found"
s = s.replace(old_b, '    out = ["libtashkeel", "rawi"]\n    if _Shams.available():\n        out.append("shams")\n', 1)
old_l = '    if name == "rawi":\n        path = _find_tashkeel_file("rawi_fasih.onnx")'
assert old_l in s, "rawi loader not found"
s = s.replace(old_l, '    if name == "shams":\n        r = _Shams()\n        r("نص")\n        return r\n\n' + old_l, 1)
a = s.index("def _load(name):")
s = s[:a] + '''class _Shams(object):
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


''' + s[a:]
compile(s, p, "exec")
open(p, "w", encoding="utf-8").write(s)
