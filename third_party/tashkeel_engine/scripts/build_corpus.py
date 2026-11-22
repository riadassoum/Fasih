import argparse
import hashlib
import json
import os
import re
import sys
import time
import urllib.request
from typing import Dict, Iterable, List, Optional, Set, Tuple

AR_LETTERS_RE = re.compile(r"[\u0621-\u063A\u0641-\u064A\u0671]")
DIAC_SET = frozenset("\u064B\u064C\u064D\u064E\u064F\u0650\u0651\u0652\u0670")
STRIP_DIAC_RE = re.compile(r"[\u064B-\u0652\u0670\u0640]")
WASL_PROCLITIC_RE = re.compile(
    r"(^|[\s\"'«»()\[\]{}.,:;!؟،؛\-])((?:[وف][\u064E\u0650]?)?(?:[بكل][\u064E\u0650]?)?)\u0627[\u064E\u064F\u0650\u0652]+"
)
SENT_PUNCT_RE = re.compile(r"[.؟!؛،:\n]")
MULTI_SPACE_RE = re.compile(r"\s+")
BARE_HAMZA_B_RE = re.compile(r"\u0625(?![\u064B-\u0652])")

QCRI_BENCH_URLS = [
    "https://raw.githubusercontent.com/qcri/advancing-arabic-diacritization/main/WikiNews_Benchmarks/WikiNews_2014.txt.diac",
    "https://raw.githubusercontent.com/qcri/advancing-arabic-diacritization/main/WikiNews_Benchmarks/WikiNews_2014_Multi_Ref.txt.diac",
    "https://raw.githubusercontent.com/qcri/advancing-arabic-diacritization/main/WikiNews_Benchmarks/WikiNews_2024_Multi_Ref.txt.diac",
]


def strip_diacritics_norm(text: str) -> str:
    s = text.replace("\u0671", "\u0627")
    s = STRIP_DIAC_RE.sub("", s)
    s = MULTI_SPACE_RE.sub(" ", s).strip()
    return s


def normalize_wasl(text: str, fix_hamza_kasra: bool = False) -> str:
    s = text.replace("\u0671", "\u0627")
    s = WASL_PROCLITIC_RE.sub(lambda m: f"{m.group(1)}{m.group(2)}\u0627", s)
    return BARE_HAMZA_B_RE.sub("\u0625\u0650", s) if fix_hamza_kasra else s


def diacritization_ratio(text: str) -> Tuple[int, float]:
    n_letters = 0
    n_diac_letters = 0
    prev_is_ar = False
    has_diac = False
    for ch in text:
        if "\u0621" <= ch <= "\u063A" or "\u0641" <= ch <= "\u064A" or ch == "\u0671":
            if prev_is_ar and has_diac:
                n_diac_letters += 1
            n_letters += 1
            prev_is_ar = True
            has_diac = False
        elif ch in DIAC_SET:
            if prev_is_ar:
                has_diac = True
        elif ch == "\u0640":
            continue
        else:
            if prev_is_ar and has_diac:
                n_diac_letters += 1
            prev_is_ar = False
            has_diac = False
    if prev_is_ar and has_diac:
        n_diac_letters += 1
    if n_letters == 0:
        return 0, 0.0
    return n_letters, n_diac_letters / n_letters


def line_digest(norm_plain: str) -> bytes:
    return hashlib.blake2b(norm_plain.encode("utf-8"), digest_size=12).digest()


def load_blocked_test_hashes(local_bench_dir: Optional[str], fetch_qcri: bool) -> Set[bytes]:
    blocked: Set[bytes] = set()

    def add_line(raw: str) -> None:
        raw = raw.strip()
        if not raw:
            return
        np = strip_diacritics_norm(raw)
        if len(np) >= 12:
            blocked.add(line_digest(np))
        for part in SENT_PUNCT_RE.split(raw):
            pp = strip_diacritics_norm(part)
            if len(pp) >= 20:
                blocked.add(line_digest(pp))

    if local_bench_dir and os.path.isdir(local_bench_dir):
        for fname in sorted(os.listdir(local_bench_dir)):
            if not fname.endswith(".txt"):
                continue
            fpath = os.path.join(local_bench_dir, fname)
            with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    add_line(line)

    if fetch_qcri:
        for url in QCRI_BENCH_URLS:
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
                with urllib.request.urlopen(req, timeout=15) as resp:
                    text = resp.read().decode("utf-8", errors="ignore")
                for line in text.splitlines():
                    add_line(line)
            except Exception as exc:
                print(f"[warn] Could not fetch QCRI benchmark {url}: {exc}", file=sys.stderr)

    return blocked


def split_long_unpunctuated(line: str, max_words: int = 15, min_chunk_words: int = 3) -> List[str]:
    line = MULTI_SPACE_RE.sub(" ", line).strip()
    if not line:
        return []
    words = line.split(" ")
    if len(words) <= max_words:
        return [line]
    if SENT_PUNCT_RE.search(line):
        # Respect punctuation first; only chunk sub-segments lacking punctuation > max_words
        out: List[str] = []
        buf: List[str] = []
        for w in words:
            buf.append(w)
            if SENT_PUNCT_RE.search(w) or len(buf) >= max_words:
                seg = " ".join(buf).strip()
                if len(seg.split(" ")) >= min_chunk_words:
                    out.append(seg)
                buf.clear()
        if buf:
            if len(buf) >= min_chunk_words or not out:
                out.append(" ".join(buf))
            else:
                out[-1] = out[-1] + " " + " ".join(buf)
        return out
    out = []
    for i in range(0, len(words), max_words):
        chunk = words[i : i + max_words]
        if len(chunk) < min_chunk_words and out:
            out[-1] = out[-1] + " " + " ".join(chunk)
        else:
            out.append(" ".join(chunk))
    return out


def clean_and_yield_lines(
    raw_texts: Iterable[str],
    blocked: Set[bytes],
    min_diac_ratio: float = 0.60,
    min_letters: int = 10,
    chunk_words: int = 15,
    max_lines: Optional[int] = None,
    fix_hamza_kasra: bool = False,
) -> Iterable[str]:
    emitted = 0
    for raw in raw_texts:
        if not raw:
            continue
        for sub in str(raw).splitlines():
            sub = sub.strip()
            if not sub:
                continue
            sub = normalize_wasl(sub, fix_hamza_kasra=fix_hamza_kasra)
            for piece in split_long_unpunctuated(sub, max_words=chunk_words):
                if len(piece) >= 30 and piece.count("\u0651") == 0:
                    continue
                n_let, ratio = diacritization_ratio(piece)
                if n_let < min_letters or ratio < min_diac_ratio:
                    continue
                np = strip_diacritics_norm(piece)
                if line_digest(np) in blocked:
                    continue
                yield piece
                emitted += 1
                if max_lines is not None and emitted >= max_lines:
                    return


def iter_hf_parquet_column(
    repo_id: str,
    column: str,
    path_prefix: str = "",
    split_hint: str = "train",
    max_lines: Optional[int] = None,
    seed: int = 42,
) -> Iterable[str]:
    """Reads ONLY a single text column from HuggingFace Parquet files without downloading audio/other columns.
    When max_lines is set, shuffles (file, row_group) order deterministically so samples span the full corpus.
    """
    import random
    import pyarrow.parquet as pq
    from huggingface_hub import HfApi, HfFileSystem

    api = HfApi()
    fs = HfFileSystem()
    files = sorted(api.list_repo_files(repo_id, repo_type="dataset"))
    pq_files = [
        f for f in files
        if f.endswith(".parquet")
        and (not path_prefix or f.startswith(path_prefix))
        and (not split_hint or split_hint in os.path.basename(f).lower())
    ]
    if not pq_files:
        pq_files = [f for f in files if f.endswith(".parquet") and (not path_prefix or f.startswith(path_prefix))]

    if max_lines is None:
        for rel in pq_files:
            hf_path = f"datasets/{repo_id}/{rel}"
            pf = pq.ParquetFile(hf_path, filesystem=fs)
            if column not in pf.schema_arrow.names:
                raise ValueError(f"Column '{column}' not in {repo_id}/{rel}: {pf.schema_arrow.names}")
            for rg in range(pf.num_row_groups):
                tbl = pf.read_row_group(rg, columns=[column])
                for val in tbl[column].to_pylist():
                    if val:
                        yield val
        return

    rng = random.Random(seed)
    # Build (pf, rg_idx) schedule; shuffle files & row groups when subsampling
    rg_jobs = []
    pfs = []
    for rel in pq_files:
        hf_path = f"datasets/{repo_id}/{rel}"
        pf = pq.ParquetFile(hf_path, filesystem=fs)
        if column not in pf.schema_arrow.names:
            raise ValueError(f"Column '{column}' not in {repo_id}/{rel}: {pf.schema_arrow.names}")
        pfs.append(pf)
        pf_i = len(pfs) - 1
        for rg in range(pf.num_row_groups):
            rg_jobs.append((pf_i, rg))
    if max_lines is not None and len(rg_jobs) > 1:
        rng.shuffle(rg_jobs)

    for pf_i, rg in rg_jobs:
        tbl = pfs[pf_i].read_row_group(rg, columns=[column])
        vals = tbl[column].to_pylist()
        if max_lines is not None and len(vals) > 1:
            rng.shuffle(vals)
        for val in vals:
            if val:
                yield val


def iter_hf_source(
    repo_id: str,
    column: str,
    config: Optional[str] = None,
    split: str = "train",
    max_lines: Optional[int] = None,
) -> Iterable[str]:
    from huggingface_hub import HfApi

    api = HfApi()
    files = api.list_repo_files(repo_id, repo_type="dataset")
    has_parquet = any(f.endswith(".parquet") and (not config or f.startswith(config + "/") or config == "default") for f in files)
    if has_parquet:
        prefix = f"{config}/" if (config and config != "default") else ""
        try:
            yield from iter_hf_parquet_column(repo_id, column, path_prefix=prefix, split_hint=split, max_lines=max_lines)
            return
        except Exception as exc:
            print(f"[info] Fallback to datasets.load_dataset streaming for {repo_id}: {exc}", file=sys.stderr)

    from datasets import load_dataset
    ds = load_dataset(repo_id, config, split=split, streaming=True)
    for row in ds:
        val = row.get(column)
        if val:
            yield val


def iter_local_file(path: str, column: Optional[str] = None) -> Iterable[str]:
    low = path.lower()
    if low.endswith(".parquet"):
        import pyarrow.parquet as pq
        pf = pq.ParquetFile(path)
        col = column or pf.schema_arrow.names[0]
        for rg in range(pf.num_row_groups):
            tbl = pf.read_row_group(rg, columns=[col])
            for val in tbl[col].to_pylist():
                if val:
                    yield val
    elif low.endswith(".jsonl") or low.endswith(".ndjson"):
        col = column or "text"
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                obj = json.loads(line)
                val = obj.get(col)
                if val:
                    yield val
    else:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                if line.strip():
                    yield line


def parse_extra_spec(spec: str, is_hf: bool) -> Dict:
    # HF format: repo:col[:config[:split[:repeat[:max_lines]]]]
    # File format: path[:col[:repeat[:max_lines]]]
    parts = spec.split(":")
    if is_hf:
        if len(parts) < 2:
            raise ValueError(f"Expected REPO:COL[:CONFIG[:SPLIT[:REPEAT[:MAX_LINES]]]], got: {spec}")
        return {
            "kind": "hf",
            "repo": parts[0],
            "col": parts[1],
            "config": parts[2] if len(parts) > 2 and parts[2] else None,
            "split": parts[3] if len(parts) > 3 and parts[3] else "train",
            "repeat": int(parts[4]) if len(parts) > 4 and parts[4] else 1,
            "max_lines": int(parts[5]) if len(parts) > 5 and parts[5] else None,
        }
    return {
        "kind": "file",
        "path": parts[0],
        "col": parts[1] if len(parts) > 1 and parts[1] else None,
        "repeat": int(parts[2]) if len(parts) > 2 and parts[2] else 1,
        "max_lines": int(parts[3]) if len(parts) > 3 and parts[3] else None,
    }


DEFAULT_SOURCES = [
    {"kind": "hf", "repo": "TigreGotico/arabic_diacritized_text", "col": "text", "config": None, "split": "train", "repeat": 2, "max_lines": None},
    {"kind": "hf", "repo": "ReligiousLLMs/shamela_all_diacritized_fully", "col": "text", "config": None, "split": "train", "repeat": 1, "max_lines": 1000000},
    {"kind": "hf", "repo": "Abdou/arabic-tashkeel-dataset", "col": "vocalized", "config": None, "split": "train", "repeat": 1, "max_lines": 1200000},
    {"kind": "hf", "repo": "arbml/tashkeela", "col": "diacratized", "config": None, "split": "train", "repeat": 1, "max_lines": 600000},
    {"kind": "hf", "repo": "MBZUAI/ArVoice", "col": "transcription", "config": "Human_3", "split": "train", "repeat": 8, "max_lines": None},
    {"kind": "hf", "repo": "MBZUAI/ArVoice", "col": "transcription", "config": "Synthetic", "split": "train", "repeat": 8, "max_lines": None},
]


def main() -> None:
    parser = argparse.ArgumentParser(description="Build & clean Arabic diacritized training corpus safely without disk bloat")
    parser.add_argument("--out", required=True, help="Output .txt corpus path")
    parser.add_argument("--append", action="store_true", help="Append cleaned extra datasets to an existing --out file")
    parser.add_argument("--no-defaults", action="store_true", help="Do not run the 5 default HuggingFace sources (use only --add-hf / --add-file)")
    parser.add_argument("--add-hf", action="append", default=[], help="Extra HF dataset: REPO:COL[:CONFIG[:SPLIT[:REPEAT[:MAX_LINES]]]]")
    parser.add_argument("--add-file", action="append", default=[], help="Extra local file: PATH[:COL[:REPEAT[:MAX_LINES]]]")
    parser.add_argument("--bench-dir", default="/kaggle/working/workspaces/user_7058431074/ابحث_لي_عن_5759/tashkeel_bench", help="Directory of local test .txt benchmarks to exclude")
    parser.add_argument("--no-qcri", action="store_true", help="Skip fetching QCRI WikiNews benchmark files for deduplication")
    parser.add_argument("--min-diac-ratio", type=float, default=0.60, help="Minimum ratio of diacritized Arabic letters (default 0.60)")
    parser.add_argument("--chunk-words", type=int, default=15, help="Max words per unpunctuated chunk (default 15)")
    parser.add_argument("--fix-hamza-kasra", action="store_true", help="Add kasra below bare إ (MSA/WikiNews orthography)")
    args = parser.parse_args()

    t0 = time.time()
    print("[1/3] Loading benchmark & QCRI hashes for strict test-leakage removal ...")
    blocked = load_blocked_test_hashes(args.bench_dir, fetch_qcri=not args.no_qcri)
    print(f"  Blocked test signatures: {len(blocked):,}")

    sources: List[Dict] = []
    if not args.no_defaults:
        sources.extend(DEFAULT_SOURCES)
    for spec in args.add_hf:
        sources.append(parse_extra_spec(spec, is_hf=True))
    for spec in args.add_file:
        sources.append(parse_extra_spec(spec, is_hf=False))

    if not sources:
        raise SystemExit("No sources selected.")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    mode = "a" if args.append else "w"
    total_written = 0
    total_unique = 0

    print(f"[2/3] Streaming, cleaning & writing to {args.out} (mode={mode}) ...")
    with open(args.out, mode, encoding="utf-8") as fout:
        for idx, src in enumerate(sources, 1):
            label = src["repo"] + (f"/{src['config']}" if src.get("config") else "") if src["kind"] == "hf" else src["path"]
            t_src = time.time()
            raw_it = (
                iter_hf_source(src["repo"], src["col"], src.get("config"), src.get("split", "train"), max_lines=src.get("max_lines"))
                if src["kind"] == "hf"
                else iter_local_file(src["path"], src.get("col"))
            )
            cleaned_it = clean_and_yield_lines(
                raw_it,
                blocked=blocked,
                min_diac_ratio=args.min_diac_ratio,
                chunk_words=args.chunk_words,
                max_lines=src.get("max_lines"),
                fix_hamza_kasra=args.fix_hamza_kasra,
            )
            rep = max(1, src.get("repeat", 1))
            src_unique = 0
            src_written = 0
            for clean_line in cleaned_it:
                block = (clean_line + "\n") * rep
                fout.write(block)
                src_unique += 1
                src_written += rep
            total_unique += src_unique
            total_written += src_written
            print(f"  [{idx}/{len(sources)}] {label} (x{rep}): unique={src_unique:,} written={src_written:,} in {time.time()-t_src:.1f}s")

    size_mb = os.path.getsize(args.out) / 1e6
    print(f"[3/3] Finished in {time.time()-t0:.1f}s | unique={total_unique:,} | written={total_written:,} | file={size_mb:.1f} MB")


if __name__ == "__main__":
    main()
