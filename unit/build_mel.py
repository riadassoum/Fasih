"""Spectrum voices: store each recording as a compact mel spectrum instead of audio.

usage: build_mel.py VOICE_DIR [K] [STEP]
Reads VOICE_DIR/audio.pcm and the recording table of units.bin and writes VOICE_DIR/mel.bin: for every
recording the 100-band log-mel spectrum the Vocos vocoder takes (24 kHz, hop 256), kept as its first K
DCT coefficients (default 60), quantized with STEP (default 0.25), delta-coded in time, zigzag-varint
packed and deflated, so the engine can decode any recording on its own. A voice with mel.bin speaks
through the vocoder (installed with the first premium voice); audio.opa is then not needed.
"""
import os, struct, sys, zlib
import numpy as np
from multiprocessing import Pool
from scipy.fft import dct

N = 1024; H = 256; NB = 513; NM = 100
WIN = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(N) / N)).astype(np.float32)
HERE = os.path.dirname(os.path.abspath(__file__))
FB = np.fromfile(os.path.join(HERE, "..", "third_party", "vocos", "melfb.f32"), np.float32).reshape(NB, NM)


def mel(x):
    p = np.pad(x, (N // 2, N // 2), mode="reflect"); T = 1 + (len(p) - N) // H
    fr = np.lib.stride_tricks.sliding_window_view(p, N)[::H][:T] * WIN
    return np.log(np.maximum(np.abs(np.fft.rfft(fr, axis=1)) @ FB, 1e-7)).astype(np.float32)


def varint(z):
    out = bytearray()
    for v in z.tolist():
        while v >= 128:
            out.append((v & 127) | 128); v >>= 7
        out.append(v)
    return bytes(out)


def encode(args):
    path, off, n, K, q = args
    x = np.fromfile(path, dtype="<i2", count=n, offset=2 * off).astype(np.float32) / 32768
    M = mel(x)
    Q = np.round(dct(M, type=2, norm="ortho", axis=1)[:, :K] / q).astype(np.int32)
    D = np.diff(Q, axis=0, prepend=0)
    z = ((D << 1) ^ (D >> 31)).astype(np.uint32).ravel()
    return len(M), zlib.compress(varint(z), 9), M[:3].mean(0)


def read_recordings(units):
    f = open(units, "rb"); magic = f.read(8)
    nu, nutt, jd, nc, sr = struct.unpack("<IIIII", f.read(20)); f.read(4 * jd)
    return [struct.unpack("<QIff", f.read(20))[:2] for _ in range(nutt)]


def main():
    vd = sys.argv[1]; K = int(sys.argv[2]) if len(sys.argv) > 2 else 60; q = float(sys.argv[3]) if len(sys.argv) > 3 else 0.25
    recs = read_recordings(os.path.join(vd, "units.bin"))
    pcm = os.path.join(vd, "audio.pcm")
    with Pool(int(os.environ.get("WORKERS", "2"))) as p:
        res = p.map(encode, [(pcm, o, n, K, q) for o, n in recs], 16)
    sil = np.mean([r[2] for r in res[:200]], 0).astype(np.float32)     # leading silence of the recordings
    with open(os.path.join(vd, "mel.bin"), "wb") as f:
        f.write(b"FSMEL001"); f.write(struct.pack("<IIfI", len(res), K, q, H)); f.write(sil.tobytes())
        base = f.tell() + 16 * len(res); off = 0
        for T, c, _ in res:
            f.write(struct.pack("<QII", base + off, T, len(c))); off += len(c)
        for T, c, _ in res:
            f.write(c)
    secs = sum(n for o, n in recs) / 24000
    size = os.path.getsize(os.path.join(vd, "mel.bin"))
    print("mel.bin: %.1f MB, %.1f kbps (%d recordings, %.1f hours)" % (size / 1e6, size * 8 / secs / 1000, len(res), secs / 3600))


if __name__ == "__main__":
    main()
