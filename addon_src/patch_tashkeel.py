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
compile(s, p, "exec")
open(p, "w", encoding="utf-8").write(s)
