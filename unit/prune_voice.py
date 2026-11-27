"""Usage-based pruning of a finished premium voice (run after compact_voice.py, before packing).

usage: prune_voice.py VOICE_DIR USES.u32 [neighbours]
USES.u32: how often each unit was chosen when synthesizing a large text the voice never recorded
(fasih-us-usage ... OUT_UNITS). Units never chosen, and not next to a chosen one in their recording, are
marked removed (quality byte 255): the engine skips them, and fasih-us-encode stores the audio no kept unit
can reach as digital silence, which costs almost nothing. Nothing is deleted from the file layout, so
the pack format is unchanged. Following Black & Taylor (1997) and Rutten et al. (2002): units the
selector itself never picks can go without an audible difference.
"""
import struct, sys
import numpy as np

vd = sys.argv[1]; uses = np.fromfile(sys.argv[2], "<u4"); K = int(sys.argv[3]) if len(sys.argv) > 3 else 1
p = vd + "/units.bin"; d = bytearray(open(p, "rb").read())
assert d[:8] == b"FSUV0003", "run compact_voice.py first"
nu, nutt, jd, nc, sr = struct.unpack_from("<IIIII", d, 8)
rec = 16 + nc + 2 * (jd + 1); h = 28 + 4 * jd + 20 * nutt + (nu + 7) // 8
assert len(uses) == nu, "usage file does not match this voice"
raw = np.frombuffer(bytes(d[h:h + nu * rec]), np.uint8).reshape(nu, rec)
utt = raw[:, 4:8].copy().view("<u4").ravel()
keep = uses > 0
k0 = keep.copy()
for s in range(1, K + 1):          # neighbours in the same recording keep natural runs and joins intact
    keep[:-s] |= k0[s:] & (utt[:-s] == utt[s:])
    keep[s:] |= k0[:-s] & (utt[s:] == utt[:-s])
q = raw[:, 3].copy(); q[~keep] = 255
for i in np.where(~keep)[0]: d[h + i * rec + 3] = 255
open(p + ".tmp", "wb").write(d)
import os; os.replace(p + ".tmp", p)
dur = (raw[:, 12:16].copy().view("<u4").ravel() - raw[:, 8:12].copy().view("<u4").ravel()).astype(np.int64)
print("kept %d of %d units (%.1f%%), %.1f%% of the unit audio" % (keep.sum(), nu, 100 * keep.mean(), 100 * dur[keep].sum() / dur.sum()))
