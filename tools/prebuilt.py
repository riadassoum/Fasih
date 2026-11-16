"""prebuilt.py SRC_VOICE_DIR OUT_VARIANT_DIR DUR LEVEL [EQ.npy]
A voice delivered already built (base.data + FPAT patches, as Fasih's build makes them, e.g.
PlanetBlindTech's Reem and Aseel): every variant is rebuilt whole, matched with hts_edit
(rate, level, tone) and turned back into patches against the matched base. Sharpening is left
out, like for the other voices."""
import os, sys, struct, subprocess
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hts_edit

src, out, dur, level = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
eq = np.load(sys.argv[5]) if len(sys.argv) > 5 else None
here = os.path.dirname(os.path.abspath(__file__))
mkpatch = os.path.join(here, "..", "addon_src", "mkpatch.py")


def apply(base, path):
    d = open(path, "rb").read(); b = bytearray(base)
    assert d[:4] == b"FPAT"
    n = struct.unpack("<I", d[4:8])[0]; i = 8
    for _ in range(n):
        off, ln = struct.unpack("<QI", d[i:i + 12]); i += 12
        b[off:off + ln] = d[i:i + ln]; i += ln
    return bytes(b)


def match(data, tmp):
    open(tmp, "wb").write(data)
    h, d, p = hts_edit.load(tmp)
    return hts_edit.edit(h, d, p, dur, level, eq)


os.makedirs(out, exist_ok=True)
vdir = os.path.join(src, "variants")
base = open(os.path.join(vdir, "base.data"), "rb").read()
tmp = os.path.join(out, ".tmp.data")
nb = match(base, tmp)
open(os.path.join(out, "base.data"), "wb").write(nb)
for name in ("b6", "b12", "b20", "repair"):
    pp = os.path.join(vdir, name + ".patch")
    if not os.path.exists(pp):
        continue
    v = match(apply(base, pp), tmp)
    open(tmp, "wb").write(v)
    subprocess.run([sys.executable, mkpatch, os.path.join(out, "base.data"), tmp, os.path.join(out, name + ".patch")],
                   check=True, stdout=subprocess.DEVNULL)
os.remove(tmp)
print(out, "ready")
