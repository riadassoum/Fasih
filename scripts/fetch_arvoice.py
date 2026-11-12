"""Collect ArVoice synthetic voices: up to CAP hours per speaker, 24 kHz originals. Resumable per shard."""
import pyarrow.parquet as pq, io, soundfile as sf, os, json, subprocess, sys
CAP = 6.0 * 3600
st = json.load(open("state.json")) if os.path.exists("state.json") else {"done": [], "dur": {}, "n": {}}
shards = [f"Synthetic/train-{i:05d}-of-00033.parquet" for i in range(33)]
for sh in shards:
    if sh in st["done"]: continue
    if all(v >= CAP for v in st["dur"].values()) and len(st["dur"]) >= 4: break
    subprocess.run(["curl", "-sSL", "-m", "300", "-o", "cur.parquet", "https://huggingface.co/datasets/MBZUAI/ArVoice/resolve/main/" + sh], check=True)
    pf = pq.ParquetFile("cur.parquet")
    for batch in pf.iter_batches(batch_size=64, columns=["speaker_id", "transcription", "original_wav"]):
        for r in batch.to_pylist():
            sp = r["speaker_id"]
            if st["dur"].get(sp, 0) >= CAP: continue
            x, sr = sf.read(io.BytesIO(r["original_wav"]["bytes"]))
            if sr != 24000 or x.ndim != 1 or not (1.0 <= len(x) / sr <= 16.0): continue
            k = st["n"].get(sp, 0); uid = f"u{k:05d}"
            os.makedirs(f"{sp}/wav", exist_ok=True)
            sf.write(f"{sp}/wav/{uid}.wav", x, 24000, subtype="PCM_16")
            with open(f"{sp}/text.tsv", "a", encoding="utf-8") as f:
                f.write(f"{uid}\t{len(x)/sr:.3f}\t" + " ".join(r["transcription"].split()) + "\n")
            st["n"][sp] = k + 1; st["dur"][sp] = st["dur"].get(sp, 0) + len(x) / sr
    st["done"].append(sh); json.dump(st, open("state.json", "w"))
    print(sh, {k: round(v / 3600, 2) for k, v in st["dur"].items()}, flush=True)
os.path.exists("cur.parquet") and os.remove("cur.parquet")
print("FINISHED")
