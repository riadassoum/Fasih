"""Objective test of a voice on held-out recordings.
usage: eval_voice.py ROOT VOICE_NAME HOLDOUT_FILE [outdir]
Synthesizes each held-out sentence, aligns it to the recording (DTW) and reports
  MCD  : mel-cepstral distortion (dB, lower = closer to the speaker)
  LSD  : log-spectral distortion on the full envelope (dB)
  GV   : variance of the synthetic spectrum relative to natural (1.0 = as lively as the speaker)"""
import sys, os
from ctypes import *
import numpy as np, pyworld as pw, pysptk
from numba import njit
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
from ar_prep import mark_wasl
if os.environ.get("RHV_LANG") == "French":
    from fr_prep import prepare as mark_wasl
root, voice, hold = sys.argv[1:4]
outdir = sys.argv[4] if len(sys.argv) > 4 else None
lib = CDLL(os.environ.get("RHVOICE_LIB", os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))), "build", "rhvoice", "b", "src", "lib", "libRHVoice.so")))
SR_CB = CFUNCTYPE(c_int, c_int, c_void_p); SPEECH_CB = CFUNCTYPE(c_int, POINTER(c_short), c_uint, c_void_p)
MARK_CB = CFUNCTYPE(c_int, c_char_p, c_void_p); WORD_CB = CFUNCTYPE(c_int, c_uint, c_uint, c_void_p)
AUDIO_CB = CFUNCTYPE(c_int, c_char_p, c_void_p); DONE_CB = CFUNCTYPE(None, c_void_p)
class Callbacks(Structure):
    _fields_ = [("set_sample_rate", SR_CB), ("play_speech", SPEECH_CB), ("process_mark", MARK_CB), ("word_starts", WORD_CB),
                ("word_ends", WORD_CB), ("sentence_starts", WORD_CB), ("sentence_ends", WORD_CB), ("play_audio", AUDIO_CB), ("done", DONE_CB)]
class Init(Structure):
    _fields_ = [("data_path", c_char_p), ("config_path", c_char_p), ("resource_paths", POINTER(c_char_p)), ("callbacks", Callbacks), ("options", c_uint)]
class Params(Structure):
    _fields_ = [("voice_profile", c_char_p), ("absolute_rate", c_double), ("absolute_pitch", c_double), ("absolute_volume", c_double),
                ("relative_rate", c_double), ("relative_pitch", c_double), ("relative_volume", c_double), ("punctuation_mode", c_int),
                ("punctuation_list", c_char_p), ("capitals_mode", c_int), ("flags", c_int)]
lib.RHVoice_new_tts_engine.argtypes = (POINTER(Init),); lib.RHVoice_new_tts_engine.restype = c_void_p
lib.RHVoice_new_message.argtypes = (c_void_p, c_char_p, c_uint, c_int, POINTER(Params), c_void_p); lib.RHVoice_new_message.restype = c_void_p
lib.RHVoice_speak.argtypes = (c_void_p,); lib.RHVoice_delete_message.argtypes = (c_void_p,)
buf = []
def on_speech(samples, n, ud):
    buf.append(np.ctypeslib.as_array(samples, (n,)).copy()); return 1
cbs = Callbacks(SR_CB(lambda *a: 1), SPEECH_CB(on_speech), MARK_CB(lambda *a: 1), WORD_CB(lambda *a: 1), WORD_CB(lambda *a: 1),
                WORD_CB(lambda *a: 1), WORD_CB(lambda *a: 1), AUDIO_CB(lambda *a: 1), DONE_CB(lambda *a: None))
eng = lib.RHVoice_new_tts_engine(byref(Init(os.path.join(root, "data").encode(), __import__("tempfile").mkdtemp().encode(), (c_char_p * 1)(None), cbs, 0)))
assert eng

def synth(text):
    buf.clear()
    p = Params(voice.encode(), 0, 0, 0, 1, 1, 1, 0, None, 0, 0); b = text.encode()
    m = lib.RHVoice_new_message(eng, b, len(b), 0, byref(p), None); lib.RHVoice_speak(m); lib.RHVoice_delete_message(m)
    return np.concatenate(buf).astype(np.float64)

@njit(cache=True)
def dtw(D):
    n, m = D.shape
    C = np.full((n + 1, m + 1), 1e30); C[0, 0] = 0
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            C[i, j] = D[i - 1, j - 1] + min(C[i - 1, j], C[i, j - 1], C[i - 1, j - 1])
    i, j = n, m; pi = []; pj = []
    while i > 0 and j > 0:
        pi.append(i - 1); pj.append(j - 1)
        a, b, c = C[i - 1, j - 1], C[i - 1, j], C[i, j - 1]
        if a <= b and a <= c: i -= 1; j -= 1
        elif b <= c: i -= 1
        else: j -= 1
    return np.array(pi[::-1]), np.array(pj[::-1])

def analyse(x):
    f0, t = pw.dio(x, 24000, frame_period=5.0); f0 = pw.stonemask(x, f0, t, 24000)
    sp = pw.cheaptrick(x, f0, t, 24000)
    e = 10 * np.log10(sp.mean(1)); keep = e > e.max() - 45
    return pysptk.sp2mc(sp, 39, 0.466)[keep], 10 * np.log10(sp[keep]), f0[keep]

import soundfile as sf
text = {}
for line in open(os.path.join(root, "corpus", "text.tsv"), encoding="utf-8"):
    u, _d, t = line.rstrip("\n").split("\t"); text[u] = t
mcd, lsd, gvr, f0s, gvd = [], [], [], [], []
for uid in open(hold).read().split():
    y = synth(mark_wasl(text[uid]))
    if outdir:
        os.makedirs(outdir, exist_ok=True); sf.write(os.path.join(outdir, uid + ".wav"), y / 32768, 24000)
    x, _ = sf.read(os.path.join(root, "corpus", "wav", uid + ".wav")); x = x * 32768.0
    mn, sn, fn = analyse(x); ms, ss, fs = analyse(y)
    D = np.sqrt(((mn[:, None, 1:25] - ms[None, :, 1:25]) ** 2).sum(-1))
    pi, pj = dtw(D)
    d = mn[pi, 1:] - ms[pj, 1:]
    mcd.append((10 / np.log(10)) * np.sqrt(2 * (d ** 2).sum(1)).mean())
    a, b = sn[pi], ss[pj]
    dl = (a - a.mean(1, keepdims=True)) - (b - b.mean(1, keepdims=True))
    lsd.append(np.sqrt((dl ** 2).mean(1)).mean())
    gvr.append(np.exp(np.mean(np.log(ms[:, 1:].var(0) / mn[:, 1:].var(0)))))
    gvd.append(np.log(ms.var(0) / mn.var(0)))
    v = (fn[pi] > 0) & (fs[pj] > 0)
    f0s.append(np.sqrt(((1200 * np.log2(fn[pi][v] / fs[pj][v])) ** 2).mean()) if v.sum() > 20 else np.nan)
print(f"{voice}: MCD {np.mean(mcd):.3f} dB | LSD {np.mean(lsd):.3f} dB | GV ratio {np.mean(gvr):.3f} | F0 RMSE {np.nanmean(f0s):.0f} cents | {len(mcd)} utts")
if os.environ.get("EVAL_SAVE_GV"):
    np.save(os.environ["EVAL_SAVE_GV"], np.exp(np.mean(gvd, 0)))   # per-coefficient variance ratio synth/natural
