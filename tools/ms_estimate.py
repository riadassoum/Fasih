"""ms_estimate.py VOICE DATA_DIR CORPUS_DIR OUT [N]
Modulation spectrum postfilter gains for one voice (time-invariant MS postfilter, Takamichi et
al. 2014): the modulation spectrum of each mel-cepstral coefficient's trajectory in the voice's
recordings, against the same in what the voice generates for the same sentences (taken straight
from the engine with RHVOICE_DUMP_MGC). Written as ms_gain.txt: "D K frame_rate", then D rows
of K gains for 0..frame_rate/2 Hz. Author: Riad Assoum project (written with Claude), GPL-3.0."""
import os, sys, subprocess, tempfile, json
import numpy as np
import soundfile as sf
import pyworld as pw
import pysptk

voice, data_dir, corpus, out = sys.argv[1:5]
N = int(sys.argv[5]) if len(sys.argv) > 5 else 80
SR, ALPHA, ORDER, FR = 24000, 0.466, 39, 200.0
L, HOP = 256, 128
K_WEIGHT = float(os.environ.get("MS_K", "0.85"))   # how far towards the recordings (1 = all the way)
MAX_GAIN = float(os.environ.get("MS_MAX", "2.5"))
F_TOP = float(os.environ.get("MS_FTOP", "50"))       # above this modulation frequency, back to 1

rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(corpus, "text.tsv"), encoding="utf-8")]
step = max(1, len(rows) // N)
pick = rows[::step][:N]


def ms(frames):
    """Mean log modulation power per coefficient and modulation-frequency bin."""
    acc, n = np.zeros((frames.shape[1], L // 2 + 1)), 0
    win = np.hanning(L)[:, None]
    for s in range(0, len(frames) - L + 1, HOP):
        seg = frames[s:s + L] - frames[s:s + L].mean(0)
        acc += np.log(np.abs(np.fft.rfft(seg * win, axis=0)) ** 2 + 1e-8).T
        n += 1
    return acc / max(n, 1)


# recordings
nat = []
for r in pick:
    x, sr = sf.read(os.path.join(corpus, "wav", r[0] + ".wav"))
    x = x.astype(np.float64) if x.ndim == 1 else x.mean(1).astype(np.float64)
    f0, t = pw.dio(x, SR, frame_period=5.0); f0 = pw.stonemask(x, f0, t, SR)
    sp = pw.cheaptrick(x, f0, t, SR)
    nat.append(pysptk.sp2mc(sp, ORDER, ALPHA))
nat = np.concatenate(nat)

# what the voice generates (no postfilter file in DATA_DIR while measuring)
assert not os.path.exists(os.path.join(data_dir, "voices", voice, "24000", "ms_gain.txt"))
dump = tempfile.mktemp(suffix=".f32")
texts = tempfile.mktemp(suffix=".json")
json.dump([r[-1] for r in pick], open(texts, "w", encoding="utf-8"), ensure_ascii=False)
here = os.path.dirname(os.path.abspath(__file__))
code = f'''
import sys, json
sys.argv = [sys.argv[0], "/x", {voice!r}, "/dev/null"]
src = open({os.path.join(here, "eval_voice.py")!r}).read().split("@njit")[0]
src = src.replace('os.path.join(root, "data").encode()', {repr(data_dir.encode())!r})
sys.path.insert(0, {os.path.join(here, "..", "scripts")!r})
exec(src)
voice = {voice!r}
for t in json.load(open({texts!r}, encoding="utf-8")):
    try: synth(mark_wasl(t))
    except ValueError: pass
'''
script = tempfile.mktemp(suffix=".py")
open(script, "w", encoding="utf-8").write(code)
subprocess.run([sys.executable, script], env=dict(os.environ, RHVOICE_DUMP_MGC=dump), cwd=here, check=True)
os.remove(script)
gen = np.fromfile(dump, np.float32).reshape(-1, ORDER + 1).astype(np.float64)
os.remove(dump); os.remove(texts)

mn, mg = ms(nat), ms(gen)
g = np.exp(0.5 * K_WEIGHT * (mn - mg))
f = np.arange(L // 2 + 1) * FR / L
g = np.clip(g, 1 / MAX_GAIN, MAX_GAIN)
for d in range(g.shape[0]):
    g[d] = np.convolve(np.pad(g[d], 2, mode="edge"), np.ones(5) / 5, "valid")
taper = np.clip((F_TOP * 1.5 - f) / (F_TOP * 0.5), 0, 1)       # 1 below F_TOP, back to 1 by 1.5*F_TOP
g = 1 + (g - 1) * taper[None, :]
g[:, 0] = 1.0
with open(out, "w") as fh:
    fh.write("%d %d %g\n" % (g.shape[0], g.shape[1], FR))
    for row in g:
        fh.write(" ".join("%.4f" % v for v in row) + "\n")
print(voice, "frames natural %d generated %d" % (len(nat), len(gen)),
      "mean gain c1-12 at 5/10/20/40 Hz:", " ".join("%.2f" % g[1:13, int(h * L / FR)].mean() for h in (5, 10, 20, 40)))
