"""Download ClArTTS train shards one at a time, extract to wav + text, delete parquet.
Resumable: skips shards already marked done. Usage: python fetch_clartts.py OUTDIR [max_shards]"""
import io, os, sys, subprocess, json
import pyarrow.parquet as pq
import soundfile as sf
import numpy as np
from scipy.signal import resample_poly

out = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 26
os.makedirs(f"{out}/wav", exist_ok=True)
done_f = f"{out}/done.json"
done = set(json.load(open(done_f))) if os.path.exists(done_f) else set()
base = "https://huggingface.co/datasets/MBZUAI/ClArTTS/resolve/main/data/"
n = 0
for i in range(26):
    name = f"train-{i:05d}-of-00026.parquet"
    if name in done:
        continue
    if n >= limit:
        break
    tmp = f"{out}/{name}"
    subprocess.run(["curl", "-sSL", "--retry", "3", "-o", tmp, base + name], check=True)
    t = pq.read_table(tmp)
    with open(f"{out}/text.tsv", "a", encoding="utf-8") as tf:
        for j, row in enumerate(t.to_pylist()):
            uid = f"cls_{i:02d}_{j:04d}"
            x = np.asarray(row["audio"], dtype=np.float64)
            sr = row["sampling_rate"]
            if sr != 24000:  # RHVoice voices run at 24 kHz
                g = np.gcd(24000, sr)
                x = resample_poly(x, 24000 // g, sr // g)
            sf.write(f"{out}/wav/{uid}.wav", np.clip(x, -1, 1), 24000, subtype="PCM_16")
            tf.write(f"{uid}\t{row['duration']}\t{row['text'].strip()}\n")
        n_rows = t.num_rows
    os.remove(tmp)
    done.add(name)
    json.dump(sorted(done), open(done_f, "w"))
    n += 1
    print("done", name, n_rows, flush=True)
