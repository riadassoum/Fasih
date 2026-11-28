"""Changes a finished compact voice's level and pace in place (voice.data and its variant patches).

usage: adjust_voice.py VOICE_DIR LEVEL_DB DUR_SCALE
LEVEL_DB is added to the loudness term (c0) of every spectrum model, as RHV_LEVEL_DB does when a voice is
written (RHVoice's own gain stops at 0.5); DUR_SCALE multiplies every duration model's means (and its
variances by the square), as RHV_DUR_SCALE does. The variants (variants/*.patch, which are byte patches
against base.data) are rebuilt so that every one of them carries the same change.
"""
import os, re, struct, sys, glob
import numpy as np

def blocks(data):
    head = data[:data.index(b"[DATA]\n") + 7].decode()
    base = len(head)
    pos = dict(re.findall(r"^(\w+(?:\[\w+\])?):(\d+-\d+)", head, re.M))
    ns = int(re.search(r"NUM_STATES:(\d+)", head).group(1))
    dm = int(re.search(r"VECTOR_LENGTH\[MCP\]:(\d+)", head).group(1))
    rng = lambda k: tuple(base + int(x) for x in pos[k].split("-"))
    return ns, dm, rng("STREAM_PDF[MCP]"), rng("DURATION_PDF")

def adjust(buf, level_db, dscale):
    a = bytearray(buf); ns, dm, (m0, m1), (d0, d1) = blocks(bytes(a))
    counts = struct.unpack_from("<%dI" % ns, a, m0)
    p = np.frombuffer(a, "<f4", sum(counts) * 6 * dm, m0 + 4 * ns).reshape(-1, 6 * dm).copy()
    p[:, 0] += level_db / 8.686
    a[m0 + 4 * ns:m0 + 4 * ns + p.nbytes] = p.tobytes()
    n = struct.unpack_from("<I", a, d0)[0]
    q = np.frombuffer(a, "<f4", n * 2 * ns, d0 + 4).reshape(n, 2 * ns).copy()
    q[:, :ns] = np.maximum(q[:, :ns] * dscale, 1.0); q[:, ns:] *= dscale * dscale
    a[d0 + 4:d0 + 4 + q.nbytes] = q.tobytes()
    return bytes(a)

def apply_patch(base, patch):
    b = bytearray(base); d = open(patch, "rb").read()
    assert d[:4] == b"FPAT"; n = struct.unpack_from("<I", d, 4)[0]; p = 8
    for _ in range(n):
        off, ln = struct.unpack_from("<QI", d, p); p += 12; b[off:off + ln] = d[p:p + ln]; p += ln
    return bytes(b)

def make_patch(base, other):
    a = np.frombuffer(base, np.uint8); b = np.frombuffer(other, np.uint8); d = np.flatnonzero(a != b)
    runs = []
    if len(d):
        brk = np.flatnonzero(np.diff(d) > 4); s = np.r_[d[0], d[brk + 1]]; e = np.r_[d[brk], d[-1]] + 1
        runs = list(zip(s.tolist(), e.tolist()))
    out = b"FPAT" + struct.pack("<I", len(runs))
    for s_, e_ in runs: out += struct.pack("<QI", s_, e_ - s_) + other[s_:e_]
    return out

vd, level_db, dscale = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
vdir = os.path.join(vd, "variants"); base = open(os.path.join(vdir, "base.data"), "rb").read()
variants = {p: apply_patch(base, p) for p in sorted(glob.glob(os.path.join(vdir, "*.patch")))}
nbase = adjust(base, level_db, dscale)
open(os.path.join(vdir, "base.data"), "wb").write(nbase)
for p, v in variants.items():
    open(p, "wb").write(make_patch(nbase, adjust(v, level_db, dscale)))
print("level %+.1f dB, durations x%.3f: base.data and %d variants" % (level_db, dscale, len(variants)))
