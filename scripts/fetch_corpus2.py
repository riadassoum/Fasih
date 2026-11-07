"""Download + convert a second corpus to corpus/wav (24 kHz PCM16) + corpus/text.tsv
usage: fetch_corpus2.py neoboy_female|halabi OUTROOT
"""
import io, os, sys, json, subprocess
import numpy as np, soundfile as sf
import pyarrow.parquet as pq
from scipy.signal import resample_poly

kind, root = sys.argv[1], sys.argv[2]
os.makedirs(f"{root}/corpus/wav", exist_ok=True)
done_f = f"{root}/corpus/done.json"
done = set(json.load(open(done_f))) if os.path.exists(done_f) else set()
if kind == "neoboy_female":
    base = "https://huggingface.co/datasets/NeoBoy/arabic-tts-wav-24k/resolve/main/data/"
    shards = [f"train-{i:05d}-of-00007.parquet" for i in range(7)] + ["test-00000-of-00001.parquet"]
    prefix = "neo"
else:
    base = "https://huggingface.co/datasets/tunis-ai/arabic_speech_corpus/resolve/main/data/"
    shards = [f"train-{i:05d}-of-00003.parquet" for i in range(3)] + ["test-00000-of-00001.parquet"]
    prefix = "asc"

# Buckwalter -> Arabic (Halabi corpus)
BW = {"'": "ء", "|": "آ", ">": "أ", "&": "ؤ", "<": "إ", "}": "ئ", "A": "ا",
      "b": "ب", "p": "ة", "t": "ت", "v": "ث", "j": "ج", "H": "ح", "x": "خ",
      "d": "د", "*": "ذ", "r": "ر", "z": "ز", "s": "س", "$": "ش", "S": "ص",
      "D": "ض", "T": "ط", "Z": "ظ", "E": "ع", "g": "غ", "_": "ـ", "f": "ف",
      "q": "ق", "k": "ك", "l": "ل", "m": "م", "n": "ن", "h": "ه", "w": "و",
      "Y": "ى", "y": "ي", "F": "ً", "N": "ٌ", "K": "ٍ", "a": "َ", "u": "ُ",
      "i": "ِ", "~": "ّ", "o": "ْ", "`": "ٰ", "{": "ٱ"}


def bw2ar(s):
    return "".join(BW.get(c, c) for c in s)


def audio_of(a):
    if isinstance(a, dict) and a.get("bytes"):
        x, sr = sf.read(io.BytesIO(a["bytes"]))
    elif isinstance(a, dict) and "array" in a:
        x, sr = np.asarray(a["array"], float), a["sampling_rate"]
    else:
        raise ValueError("unknown audio format")
    if x.ndim > 1:
        x = x.mean(1)
    if sr != 24000:
        g = np.gcd(24000, sr); x = resample_poly(x, 24000 // g, sr // g)
    return np.clip(x, -1, 1)


n_total = 0
for si, name in enumerate(shards):
    if name in done:
        continue
    tmp = f"{root}/{name}"
    subprocess.run(["curl", "-sSL", "--retry", "3", "-o", tmp, base + name], check=True)
    t = pq.read_table(tmp)
    cols = t.schema.names
    kept = 0
    with open(f"{root}/corpus/text.tsv", "a", encoding="utf-8") as tf:
        for j, row in enumerate(t.to_pylist()):
            if kind == "neoboy_female" and row.get("gender") != "female":
                continue
            txt = row["text"].strip()
            if kind == "halabi":
                txt = bw2ar(txt)
            x = audio_of(row["audio"])
            uid = f"{prefix}_{si:02d}_{j:05d}"
            sf.write(f"{root}/corpus/wav/{uid}.wav", x, 24000, subtype="PCM_16")
            tf.write(f"{uid}\t{len(x) / 24000:.3f}\t{txt}\n")
            kept += 1
    os.remove(tmp)
    done.add(name); json.dump(sorted(done), open(done_f, "w"))
    n_total += kept
    print("done", name, "kept", kept, flush=True)
