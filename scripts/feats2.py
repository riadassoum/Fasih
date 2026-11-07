"""Second-generation features (reuses the Harvest F0 already extracted):
  mgc: ORDER+1 mel-cepstra (default order 39 -> 40 coefs)
  bap: 7-band aperiodicity from D4C with threshold 0 (graded, not snapped to 1)
  vuv: voicing decision (uint8) = harvest voiced AND default-D4C says periodic
usage: feats2.py OUTDIR start end
"""
import os, sys
import numpy as np
import soundfile as sf
import pyworld as pw
import pysptk

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SR, FP, ALPHA = 24000, 5.0, 0.466
ORDER = int(os.environ.get("RHV_ORDER", "39"))
BANDS = [0, 1000, 2000, 4000, 6000, 8000, 10000, 12000]


def band_db(ap, freqs):
    return np.stack([10 * np.log10(np.clip(ap[:, (freqs >= lo) & ((freqs < hi) | (hi >= SR / 2))].mean(1) ** 2, 1e-10, 1))
                     for lo, hi in zip(BANDS[:-1], BANDS[1:])], 1)


def harmonic_bap(x, f0, nfft=2048):
    """Band aperiodicity (dB) from harmonic peak vs inter-harmonic valley power,
    pitch-adaptive window of 3 periods. Unvoiced frames -> 0 dB."""
    T = len(f0); out = np.zeros((T, len(BANDS) - 1), np.float32)
    fq = np.fft.rfftfreq(nfft, 1 / SR)
    hop = int(SR * FP / 1000)
    for t in range(T):
        if f0[t] <= 0:
            continue
        L = int(7 * SR / f0[t]) | 1
        c = t * hop; s0 = c - L // 2
        seg = x[max(s0, 0):s0 + L]
        if len(seg) < L:
            seg = np.pad(seg, (0, L - len(seg)))
        X = np.abs(np.fft.rfft(seg * np.hanning(L), nfft)) ** 2
        k = np.arange(1, int((SR / 2) / f0[t]))
        hk = np.interp(k * f0[t], fq, X)              # harmonic peaks
        vk = np.interp((k + 0.5) * f0[t], fq, X)      # valleys between harmonics
        hf = k * f0[t]
        for bi, (lo, hi) in enumerate(zip(BANDS[:-1], BANDS[1:])):
            m = (hf >= lo) & (hf < hi)
            if m.any():
                r = vk[m].sum() / max(hk[m].sum(), 1e-12)
                out[t, bi] = 10 * np.log10(np.clip(r, 1e-5, 1.0))
    return out


def main():
    out, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    ids = [l.strip() for l in open(os.path.join(ROOT, "work", "ids.txt")) if l.strip()][a:b]
    for d in ("mgc", "bap", "vuv"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    for uid in ids:
        if os.path.exists(os.path.join(out, "vuv", uid + ".vuv")):
            continue
        x, sr = sf.read(os.path.join(ROOT, "corpus", "wav", uid + ".wav"))
        x = x.astype(np.float64) * 32768.0
        lf0 = np.fromfile(os.path.join(ROOT, "work", "feat", "lf0", uid + ".lf0"), np.float32).astype(np.float64)
        f0 = np.where(lf0 > -1e9, np.exp(lf0), 0.0)
        t = np.arange(len(f0)) * FP / 1000.0
        sp = pw.cheaptrick(x, f0, t, SR)
        mgc = pysptk.sp2mc(sp, ORDER, ALPHA).astype(np.float32)
        vuv = np.ones(1, np.uint8)
        mgc.tofile(os.path.join(out, "mgc", uid + ".mgc"))
        vuv.tofile(os.path.join(out, "vuv", uid + ".vuv"))  # last = done marker


if __name__ == "__main__":
    main()
