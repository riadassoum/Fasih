"""freeze_stats.py TRAIN_DIR : replace a finished voice's statistics by the compact form the
writer needs (see _CompactRows in train.py). Run only on voices whose training is done."""
import sys, os, json, numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
TR = os.path.abspath(sys.argv[1])
from labels import LabelSet
LANG = os.path.join(TR, "..", "..", "lang", "Arabic")
codes = np.load(os.path.join(TR, "codes.npy")); vocab = json.load(open(os.path.join(TR, "vocab.json"), encoding="utf-8"))
flds = [f[0] for f in LabelSet(LANG).fields] if os.path.isdir(LANG) else list(vocab.keys())
nm = np.array(vocab["name"], dtype=object)[codes[:, flds.index("name")]]
KEEP = {"s", "S", "sh", "f", "th", "x", "H", "t", "T", "k", "q", "l", "a"}
keep = np.flatnonzero(np.isin(nm, list(KEEP)))
D = 40
for j in range(5):
    for kind in ("mgc", "bap"):
        f = os.path.join(TR, f"st_{kind}_{j}.npy")
        if not os.path.exists(f):
            continue
        S = np.load(f, mmap_mode="r")
        out = os.path.join(TR, f"st_{kind}_{j}.cmp.npz")
        if kind == "mgc":
            tot = np.zeros(S.shape[1]); 
            for a in range(0, len(S), 100000): tot += np.asarray(S[a:a + 100000]).sum(0, dtype=np.float64)
            np.savez(out, data=np.asarray(S[keep][:, :1 + D]), keep=keep, tot=tot, n=len(S))
        else:
            np.savez(out, data=np.asarray(S[:, :2]))
        real = os.path.realpath(f); os.remove(f) if os.path.islink(f) else None
        print(os.path.basename(out), "from", real)
