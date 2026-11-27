"""Drops the Opus packets of a pruned voice that no kept unit can reach (lossless for what is kept).

usage: prune_audio.py VOICE_DIR [keep_ms]
After prune_voice.py has marked units removed (quality 255) in units.bin: every Opus frame of audio.opa
(FSOP0002) that lies farther than keep_ms (default 125: joins, ramps, onsets, releases and the decoder's
80 ms pre-roll) from every kept unit is stored as an empty packet. The engine never decodes those
frames; the packets that remain are the same bytes, so the kept audio is bit for bit what it was.
"""
import struct, sys, os
import numpy as np

vd = sys.argv[1]; keep_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 125.0
u = open(vd + "/units.bin", "rb").read()
assert u[:8] == b"FSUV0003"
nu, nutt, jd, nc, sr = struct.unpack_from("<IIIII", u, 8)
ut = np.frombuffer(u, "<u8", nutt * 0 + 0) if False else None
tab_u = 28 + 4 * jd
starts = [struct.unpack_from("<Q", u, tab_u + 20 * i)[0] for i in range(nutt)]
rec = 16 + nc + 2 * (jd + 1); h = tab_u + 20 * nutt + (nu + 7) // 8
raw = np.frombuffer(u, np.uint8, nu * rec, h).reshape(nu, rec)
q = raw[:, 3]; utt = raw[:, 4:8].copy().view("<u4").ravel()
a = raw[:, 8:12].copy().view("<u4").ravel().astype(np.int64); b = raw[:, 12:16].copy().view("<u4").ravel().astype(np.int64)
M = int(sr * keep_ms / 1000)
d = open(vd + "/audio.opa", "rb").read()
assert d[:8] == b"FSOP0002", "run compact_voice.py first"
n2, sr2, FR, la = struct.unpack_from("<IIII", d, 8)
assert n2 == nutt
tab = [struct.unpack_from("<QII", d, 24 + 16 * i) for i in range(nutt)]
kept_units = q != 255
by_utt = [[] for _ in range(nutt)]
for i in np.where(kept_units)[0]:
    by_utt[utt[i]].append((a[i] - M, b[i] + M))
out = bytearray(b"FSOP0002") + struct.pack("<IIII", nutt, sr2, FR, la)
body = bytearray(); newtab = []; before = after = 0
for i, (off, nf, ns) in enumerate(tab):
    sizes = np.frombuffer(d, "<u2", nf, off).astype(np.int64)
    pk = d[off + 2 * nf: off + 2 * nf + int(sizes.sum())]
    po = np.concatenate([[0], np.cumsum(sizes)])
    keep = np.zeros(nf, bool)
    for lo, hi in by_utt[i]:
        # frame f decodes stream samples [f*FR, (f+1)*FR), which are recording samples minus the lookahead
        f0 = max(0, int((lo + la) // FR)); f1 = min(nf, int((hi + la) // FR) + 1)
        keep[f0:f1] = True
    ns_ = np.where(keep, sizes, 0)
    newtab.append((len(body), nf, ns))
    body += ns_.astype("<u2").tobytes()
    for f in np.where(keep)[0]: body += pk[po[f]:po[f + 1]]
    before += int(sizes.sum()); after += int(ns_.sum())
base = 24 + 16 * nutt
for off, nf, ns in newtab: out += struct.pack("<QII", base + off, nf, ns)
out += body
open(vd + "/audio.opa.tmp", "wb").write(out); os.replace(vd + "/audio.opa.tmp", vd + "/audio.opa")
print("audio.opa: %.1f -> %.1f MB of packets (%.1f%% kept)" % (before / 1e6, after / 1e6, 100 * after / before))
