"""Time-to-first-audio and loudness per voice profile, through libRHVoice (as NVDA uses it).
usage: latency.py DATA_DIR CONFIG_DIR profile=text [profile=text ...]"""
import sys, time, threading
from ctypes import *
import numpy as np

lib = CDLL("/tmp/rhb2/src/lib/libRHVoice.so")
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

state = {}
def on_speech(samples, n, ud):
    if state["first"] is None:
        state["first"] = time.perf_counter()
    state["buf"].append(np.ctypeslib.as_array(samples, (n,)).copy())
    return 1
cbs = Callbacks(SR_CB(lambda *a: 1), SPEECH_CB(on_speech), MARK_CB(lambda *a: 1), WORD_CB(lambda *a: 1), WORD_CB(lambda *a: 1),
                WORD_CB(lambda *a: 1), WORD_CB(lambda *a: 1), AUDIO_CB(lambda *a: 1), DONE_CB(lambda *a: None))
data, cfg = sys.argv[1], sys.argv[2]
t0 = time.perf_counter()
eng = lib.RHVoice_new_tts_engine(byref(Init(data.encode(), cfg.encode(), (c_char_p * 1)(None), cbs, 0)))
print(f"engine init {1000*(time.perf_counter()-t0):.0f} ms")
for spec in sys.argv[3:]:
    prof, text = spec.split("=", 1)
    res = []
    for rep in range(4):
        state.update(first=None, buf=[])
        p = Params(prof.encode(), 0, 0, 0, 1, 1, 1, 0, None, 0, 0)
        b = text.encode()
        msg = lib.RHVoice_new_message(eng, b, len(b), 0, byref(p), None)
        t = time.perf_counter(); lib.RHVoice_speak(msg); total = time.perf_counter() - t
        lib.RHVoice_delete_message(msg)
        x = np.concatenate(state["buf"]).astype(float) / 32768
        res.append(((state["first"] - t) * 1000, total * 1000, len(x) / 24000))
    first = np.median([r[0] for r in res[1:]]); tot = np.median([r[1] for r in res[1:]]); dur = res[-1][2]
    xs = x[np.abs(x) > 1e-4]
    print(f"{prof:12s} first audio {first:6.1f} ms | whole {tot:6.0f} ms for {dur:4.1f} s audio (x{dur*1000/tot:4.0f} real time) | rms {np.sqrt((xs**2).mean()):.4f} peak {np.abs(x).max():.2f}")
