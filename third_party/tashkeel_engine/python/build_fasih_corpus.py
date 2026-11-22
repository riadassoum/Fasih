"""Rebuild the corpus Barq v2 (models/tashkeel_engine.tash) was trained on.

usage: build_fasih_corpus.py QCRI_WIKIPEDIA.jsonl OUT.txt [--arvoice TEXT.tsv] [--exclude TEST.txt ...]

  QCRI_WIKIPEDIA.jsonl  Wikipedia_Diacritized_Corpus/Wikipedia_20240420.diac.jsonl from
                        https://github.com/qcri/advancing-arabic-diacritization
  --arvoice             a file of ArVoice sentences, one per line (the sentence is the last
                        tab-separated field), https://huggingface.co/datasets/MBZUAI/ArVoice
  --exclude             fully vowelled test sentences to keep out of training (one per line)

The Hugging Face datasets are downloaded one parquet file at a time and deleted after use:
TigreGotico/arabic_diacritized_text, ReligiousLLMs/shamela_all_diacritized_fully,
and Abdou/arabic-tashkeel-dataset. Needs curl and pyarrow.

Every sentence is cleaned the same way: hamzat al-wasl written without a vowel (QCRI's text
writes اَلْ, the others ال), lines without punctuation cut into 15-word pieces, and a line kept
only if it has 8 or more letters, at most 300 characters, at least 60% of its letters
vowelled, no Latin letters, and is not a duplicate or a test sentence. The random choices use
fixed seeds, so the same datasets give the same file.

Mix (about 9.5 million lines): QCRI Wikipedia x4, TigreGotico x2, about 1 million Shamela
lines, about 1.2 million Abdou lines, ArVoice x8. (Fasih 1.3 also had about 0.6 million lines of
arbml/tashkeela; it was dropped in 1.3.1 because that copy has lost its shaddas.)
Then train with:
  python3 train_pipeline.py --corpus OUT.txt --output barq.tash --max-vocab 600000 \\
    --max-bigrams 1200000 --max-tree-lines 600000 --max-tree-samples 1000000 \\
    --oov-trees 40 --irab-trees 40 --morph-trees 32 --max-depth 7
"""
import argparse, hashlib, json, os, random, re, subprocess, tempfile
import pyarrow.parquet as pq

D = re.compile("[ً-ْ]")
A = re.compile("[ء-ي]")
WASL = re.compile(r"(^|\s)([وفبلك]?)ا[َُِ]?ل")


def bare(t):
    return re.sub("[ً-ْٰ\\s]", "", t)


def key(t):
    return hashlib.md5(bare(t).encode()).digest()[:8]


def pieces(x):
    for p in re.split(r"(?<=[.!؟?:\n؛])\s*", x or ""):
        if len(p) > 300:
            w = p.split()
            for j in range(0, len(w), 15):
                yield " ".join(w[j:j + 15])
        else:
            yield p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("qcri"); ap.add_argument("out")
    ap.add_argument("--arvoice"); ap.add_argument("--exclude", nargs="*", default=[])
    a = ap.parse_args()
    random.seed(11)
    seen = set()
    for f in a.exclude:
        for l in open(f, encoding="utf-8"):
            if l.strip():
                seen.add(key(l))
    tests = set(seen)
    out = open(a.out, "w", encoding="utf-8")
    stats = {}

    def ok(t):
        t = WASL.sub(r"\1\2ال", re.sub(r"\s+", " ", t).strip())
        n = len(A.findall(t))
        if n < 8 or len(t) > 300 or len(D.findall(t)) < 0.6 * n or re.search("[a-zA-Z]", t):
            return None
        k = key(t)
        if k in seen:
            return None
        seen.add(k)
        return t

    def feed(src, texts, prob=1.0, copies=1):
        n = 0
        for x in texts:
            for s in pieces(x):
                if prob < 1 and random.random() > prob:
                    continue
                s = ok(s)
                if s:
                    for _ in range(copies):
                        out.write(s + "\n")
                    n += 1
        stats[src] = stats.get(src, 0) + n

    def hf(ds, col, prob=1.0, copies=1):
        tmp = os.path.join(tempfile.gettempdir(), "barq_corpus.parquet")
        i = 0
        while subprocess.run(["curl", "-sSfL", "-m", "1800", "-o", tmp,
                              "https://huggingface.co/api/datasets/%s/parquet/default/train/%d.parquet" % (ds, i)]).returncode == 0:
            for b in pq.read_table(tmp, columns=[col]).to_batches(20000):
                feed(ds, b.column(0).to_pylist(), prob, copies)
            os.remove(tmp); i += 1
            print(ds, "part", i, stats.get(ds), flush=True)

    for l in open(a.qcri, encoding="utf-8"):
        feed("qcri_wikipedia", [json.loads(l)["text"]], 1.0, 4)
    hf("TigreGotico/arabic_diacritized_text", "text", 1.0, 2)
    # Shamela: about 2.85 million lines pass at this rate; a second pass keeps about 1 million of
    # them (in Fasih's run, done on the finished file, with seed 3)
    rng = random.Random(3)
    keep = 1000000 / 2847431
    real_feed, kept = feed, []

    def feed_shamela(src, texts, prob=1.0, copies=1):
        before = stats.get(src, 0)
        buf = []
        for x in texts:
            for s in pieces(x):
                if random.random() > prob:
                    continue
                s = ok(s)
                if s:
                    buf.append(s)
        for s in buf:
            if rng.random() <= keep:
                out.write(s + "\n")
        stats[src] = before + len(buf)
    feed = feed_shamela
    hf("ReligiousLLMs/shamela_all_diacritized_fully", "text", 0.183)
    feed = real_feed
    hf("Abdou/arabic-tashkeel-dataset", "vocalized", 0.137)
    # arbml/tashkeela is left out since 1.3.1: that copy has lost almost all its shaddas (2 in
    # 18,692 marks, against about 7% of marks in every other source), which taught Barq to drop them.
    if a.arvoice:
        for l in open(a.arvoice, encoding="utf-8"):
            t = l.rstrip("\n").split("\t")[-1]
            if t and key(t) not in tests:
                for _ in range(8):
                    out.write(t + "\n")
    out.close()
    print("done", stats)


if __name__ == "__main__":
    main()
