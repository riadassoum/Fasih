"""relf0.py ROOT PART NPARTS FMIN FMAX : re-extract only pitch (lf0) and aperiodicity (bap) with
another pitch range, into ROOT/work/feat/{lf0,bap}. For voices whose pitch leaves the default
60-400 Hz range (the tracker then locks onto half the pitch: octave leaps)."""
import sys, os, numpy as np, soundfile as sf
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import feats
R, part, nparts, fmin, fmax = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
feats.F0_MIN, feats.F0_MAX = fmin, fmax
W = os.path.join(R, "work")
ids = [l.strip() for l in open(os.path.join(W, "ids.txt")) if l.strip()][part::nparts]
for d in ("lf0", "bap"):
    os.makedirs(os.path.join(W, "feat", d), exist_ok=True)
for k, n in enumerate(ids):
    lp = os.path.join(W, "feat", "lf0", n + ".lf0")
    if os.path.exists(lp): continue
    x, sr = sf.read(os.path.join(R, "corpus", "wav", n + ".wav"))
    mgc, lf0, bap = feats.analyze(x.astype(np.float64))
    bap.tofile(os.path.join(W, "feat", "bap", n + ".bap"))
    lf0.tofile(lp + ".tmp"); os.replace(lp + ".tmp", lp)
    if k % 200 == 0: print(part, k, len(ids), flush=True)
print("DONE", part)
