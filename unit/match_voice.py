"""Matches a premium voice to a compact Fasih voice: pace, loudness and tone.

usage: match_voice.py PREMIUM_SAMPLE.wav COMPACT_SAMPLE.wav VOICE_DIR [limit_db]
Both samples speak the same text at default settings. Writes into VOICE_DIR:
  weights.txt  "rate" (speech time, pauses excluded, made equal) and "level_db" (adjusted so the
               active-speech loudness is equal);
  eq.txt       RHVoice equalizer (v1): peaking filters at third-octave centres from 125 Hz to 10 kHz that
               bring the long-term spectrum of the premium voice to the compact one's, limited to +-limit_db (default 4)
               and smoothed across bands, so only the overall tone is matched, never the detail.
"""
import sys, os, re
import numpy as np, soundfile as sf, scipy.signal as ss

P, C, vd = sys.argv[1:4]
LIMIT = float(sys.argv[4]) if len(sys.argv) > 4 else 4.0   # dB per band
p, sr = sf.read(P); c, _ = sf.read(C)

def active(x):
    f = 240; n = len(x) // f; e = np.sqrt((x[:n * f].reshape(n, f) ** 2).mean(1) + 1e-12)
    return e > np.max(e) * 10 ** (-40 / 20), f        # frames within 40 dB of the loudest

def speech_time(x):
    a, f = active(x)
    return a.sum() * f / sr

def ltas(x):
    a, f = active(x)
    seg = np.concatenate([x[i * f:(i + 1) * f] for i in np.where(a)[0]])
    fr, P_ = ss.welch(seg, sr, nperseg=2048)
    return fr, P_, seg

rate = speech_time(p) / speech_time(c)              # > 1: the premium voice is slower, so speed it up
fr, Pp, sp = ltas(p); _, Pc, sc = ltas(c)
centres = 1000 * 2 ** (np.arange(-9, 11) / 3)        # 125 Hz .. 10 kHz
centres = centres[centres < sr / 2 * 0.9]
g = []
for fc in centres:
    m = (fr >= fc * 2 ** (-1 / 6)) & (fr < fc * 2 ** (1 / 6))
    g.append(10 * np.log10(Pc[m].mean() / Pp[m].mean()))
g = np.array(g); g -= np.average(g, weights=np.ones_like(g))  # overall level is handled by level_db
g = np.convolve(np.pad(g, 1, mode="edge"), [0.25, 0.5, 0.25], "valid")
g = np.clip(g, -LIMIT, LIMIT)

def peaking(fc, gain_db, q=4.3):                    # RBJ peaking EQ; Q 4.3 is about a third of an octave
    A = 10 ** (gain_db / 40); w = 2 * np.pi * fc / sr; al = np.sin(w) / (2 * q)
    return [1 + al * A, -2 * np.cos(w), 1 - al * A, 1 + al / A, -2 * np.cos(w), 1 - al / A]

sections = [peaking(fc, gd) for fc, gd in zip(centres, g) if abs(gd) >= 0.3]
spe = sp.copy()
for b in sections: spe = ss.lfilter(b[:3], b[3:], spe)
lvl = 10 * np.log10(np.mean(sc ** 2) / np.mean(spe ** 2))   # loudness after the equalizer
with open(os.path.join(vd, "eq.txt"), "w") as f:
    f.write("v1\n# matches this premium voice's long-term tone to the compact Fasih voices (unit/match_voice.py)\n")
    for fc, gd in zip(centres, g):
        if abs(gd) >= 0.3:
            f.write(" ".join("%.10g" % v for v in peaking(fc, gd)) + "\n")
w = [l for l in open(os.path.join(vd, "weights.txt")).read().splitlines() if l and not re.match(r"^(rate|level_db) ", l)]
old = 0.0
for l in open(os.path.join(vd, "weights.txt")):
    if l.startswith("level_db "): old = float(l.split()[1])
w += ["rate %.3f" % rate, "level_db %.1f" % (old + lvl)]
open(os.path.join(vd, "weights.txt"), "w").write("\n".join(w) + "\n")
print("%s: rate x%.3f, level %+.1f dB, tone %s dB" % (os.path.basename(vd), rate, lvl, " ".join("%+.1f" % x for x in g)))
