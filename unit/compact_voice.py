"""Lossless compaction of a finished unit voice (run last, before packing).

usage: compact_voice.py VOICE_DIR
units.bin (FSUV0002 -> FSUV0003): neighbouring units of a recording share their boundary, so the
  join features at a unit's end equal those at the next unit's start; they are stored once.
audio.opa (FSOP0001 -> FSOP0002): the per-frame 32-bit offset table becomes 16-bit packet sizes.
The engine reads both forms and synthesizes bit-identical audio from either.
"""
import os, struct, sys
import numpy as np


def compact_units(path):
    data = open(path, "rb").read()
    if data[:8] != b"FSUV0002":
        print("units.bin: already compact or unknown format"); return
    nu, nutt, jd, nc, sr = struct.unpack_from("<IIIII", data, 8)
    p = 28 + 4 * jd + 20 * nutt
    head = data[8:p]
    rec = 16 + nc + 4 * (jd + 1)
    raw = np.frombuffer(data, np.uint8, nu * rec, p).reshape(nu, rec)
    o = 16 + nc; h = 2 * (jd + 1)
    left = raw[:, o:o + h]; right = raw[:, o + h:o + 2 * h]
    utt = raw[:, 4:8].copy().view("<u4").ravel()
    a = raw[:, 8:12].copy().view("<u4").ravel(); b = raw[:, 12:16].copy().view("<u4").ravel()
    # unit i's right edge is shared when unit i+1 starts where it ends, in the same recording, bit for bit
    shared = np.zeros(nu, bool)
    shared[:-1] = (utt[1:] == utt[:-1]) & (a[1:] == b[:-1]) & np.all(right[:-1] == left[1:], axis=1)
    out = bytearray(b"FSUV0003"); out += head
    out += np.packbits(shared, bitorder="little").tobytes()
    fixed = np.concatenate([raw[:, :o], left], axis=1)            # header, context and left edge of every unit
    out += fixed.tobytes()
    out += right[~shared].tobytes()                               # right edges that are not shared
    open(path + ".tmp", "wb").write(out); os.replace(path + ".tmp", path)
    print("units.bin: %.1f -> %.1f MB (%.1f%% of right edges shared)" % (len(data) / 1e6, len(out) / 1e6, 100 * shared.mean()))


def compact_audio(path):
    data = open(path, "rb").read()
    if data[:8] != b"FSOP0001":
        print("audio.opa: already compact or unknown format"); return
    nutt, sr, fr, la = struct.unpack_from("<IIII", data, 8)
    tab = [struct.unpack_from("<QII", data, 24 + 16 * i) for i in range(nutt)]
    out = bytearray(b"FSOP0002"); out += struct.pack("<IIII", nutt, sr, fr, la)
    body = bytearray(); new_tab = []
    for off, nf, ns in tab:
        offs = np.frombuffer(data, "<u4", nf + 1, off)
        sizes = np.diff(offs.astype(np.int64))
        assert sizes.max() < 65536
        pk = data[off + 4 * (nf + 1): off + 4 * (nf + 1) + int(offs[-1])]
        new_tab.append((len(body), nf, ns))
        body += sizes.astype("<u2").tobytes() + pk
    base = 24 + 16 * nutt
    for off, nf, ns in new_tab:
        out += struct.pack("<QII", base + off, nf, ns)
    out += body
    open(path + ".tmp", "wb").write(out); os.replace(path + ".tmp", path)
    print("audio.opa: %.1f -> %.1f MB" % (len(data) / 1e6, len(out) / 1e6))


if __name__ == "__main__":
    vd = sys.argv[1]
    compact_units(os.path.join(vd, "units.bin"))
    if os.path.exists(os.path.join(vd, "audio.opa")):
        compact_audio(os.path.join(vd, "audio.opa"))
