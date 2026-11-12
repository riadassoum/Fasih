"""mkpatch.py BASE OTHER OUT : the bytes of OTHER that differ from BASE (files of equal size).
Format: b"FPAT", uint32 count, then per run: uint64 offset, uint32 length, bytes."""
import sys, struct
import numpy as np
a = np.fromfile(sys.argv[1], np.uint8); b = np.fromfile(sys.argv[2], np.uint8)
assert len(a) == len(b), "variants must have the same size"
d = np.flatnonzero(a != b)
runs = []
if len(d):
    brk = np.flatnonzero(np.diff(d) > 4)   # tight runs: patches for different settings must not overlap
    starts = np.r_[d[0], d[brk + 1]]; ends = np.r_[d[brk], d[-1]] + 1
    runs = list(zip(starts.tolist(), ends.tolist()))
with open(sys.argv[3], "wb") as f:
    f.write(b"FPAT" + struct.pack("<I", len(runs)))
    for s, e in runs:
        f.write(struct.pack("<QI", s, e - s)); f.write(b[s:e].tobytes())
print(sys.argv[3], len(runs), "runs", sum(e - s for s, e in runs), "bytes")
