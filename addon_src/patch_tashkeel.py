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
