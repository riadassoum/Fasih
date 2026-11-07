"""Acoustic features for the RHVoice voice (24 kHz, 5 ms frames).
mgc: 31 mel-cepstral coefs (alpha 0.466), lf0: log F0 (-1e10 unvoiced), bap: 7 band aperiodicities in dB.
usage: feats.py WAVDIR OUTDIR [start end]   (resumable; skips done files)
       feats.py --bpf OUT.txt               (write our band-pass filter set)
"""
import os, sys
import numpy as np

SR, FP_MS, ORDER, ALPHA = 24000, 5.0, 30, 0.466
BANDS = [0, 1000, 2000, 4000, 6000, 8000, 10000, 12000]
F0_MIN, F0_MAX = 60.0, 400.0


def write_bpf(path, taps=89):
    from scipy.signal import firwin
    # complementary bank: differences of linear-phase low-pass filters (sums to a delta)
    lows = [np.zeros(taps)] + [firwin(taps, f, fs=SR) for f in BANDS[1:-1]]
    d = np.zeros(taps); d[taps // 2] = 1.0
    lows.append(d)
    filt = [lows[i + 1] - lows[i] for i in range(len(BANDS) - 1)]
    with open(path, "w") as f:
        f.write(f"{len(filt)}\n{taps}\n")
        for h in filt:
            f.write("\n".join(repr(float(v)) for v in h) + "\n")


def analyze(x):
    import pyworld as pw, pysptk
    x = x.astype(np.float64) * 32768.0  # RHVoice vocoder works in int16 units
    f0, t = pw.harvest(x, SR, f0_floor=F0_MIN, f0_ceil=F0_MAX, frame_period=FP_MS)
    sp = pw.cheaptrick(x, f0, t, SR)
    ap = pw.d4c(x, f0, t, SR)
    mgc = pysptk.sp2mc(sp, ORDER, ALPHA)
    lf0 = np.where(f0 > 0, np.log(np.maximum(f0, 1e-3)), -1e10)
    freqs = np.linspace(0, SR / 2, sp.shape[1])
    bap = np.stack([10 * np.log10(np.clip(ap[:, (freqs >= lo) & (freqs < hi if hi < SR / 2 else freqs <= hi)].mean(1) ** 2, 1e-10, 1))
                    for lo, hi in zip(BANDS[:-1], BANDS[1:])], 1)
    return mgc.astype(np.float32), lf0.astype(np.float32), bap.astype(np.float32)


def main():
    if sys.argv[1] == "--bpf":
        write_bpf(sys.argv[2]); return
    import soundfile as sf
    wavdir, out = sys.argv[1], sys.argv[2]
    names = sorted(n[:-4] for n in os.listdir(wavdir) if n.endswith(".wav"))
    if len(sys.argv) > 4:
        names = names[int(sys.argv[3]):int(sys.argv[4])]
    for d in ("mgc", "lf0", "bap"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    for n in names:
        if os.path.exists(os.path.join(out, "bap", n + ".bap")):
            continue
        x, sr = sf.read(os.path.join(wavdir, n + ".wav"))
        assert sr == SR
        mgc, lf0, bap = analyze(x)
        mgc.tofile(os.path.join(out, "mgc", n + ".mgc"))
        lf0.tofile(os.path.join(out, "lf0", n + ".lf0"))
        bap.tofile(os.path.join(out, "bap", n + ".bap"))  # written last = done marker


if __name__ == "__main__":
    main()
