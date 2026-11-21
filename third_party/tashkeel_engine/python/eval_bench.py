import argparse
import time

from arabic_utils import compute_metrics, strip_diacritics
from tashkeel_py import TashkeelEngine


def main():
    parser = argparse.ArgumentParser(description="Evaluate & benchmark a .tash diacritizer model")
    parser.add_argument("--model", required=True, help="Path to .tash model")
    parser.add_argument("--test", required=True, help="Path to diacritized test file")
    parser.add_argument("--lib", default=None, help="Path to libtashkeel.so")
    parser.add_argument("--max-lines", type=int, default=50000, help="Max lines to evaluate")
    args = parser.parse_args()

    t0 = time.perf_counter()
    engine = TashkeelEngine(args.model, args.lib)
    load_ms = (time.perf_counter() - t0) * 1000.0

    with open(args.test, "r", encoding="utf-8", errors="ignore") as f:
        gold_lines = [line.strip() for _, line in zip(range(args.max_lines), f) if line.strip()]

    plain_lines = [strip_diacritics(l) for l in gold_lines]

    t1 = time.perf_counter()
    pred_lines = [engine.diacritize(l) for l in plain_lines]
    elapsed = time.perf_counter() - t1

    metrics = compute_metrics(gold_lines, pred_lines)
    wps = metrics["total_words"] / max(elapsed, 1e-9)

    print(f"Model loaded in {load_ms:.2f} ms (vocab={engine.vocab_size:,})")
    print(f"Evaluated {len(gold_lines):,} lines ({metrics['total_words']:,} words, {metrics['total_chars']:,} chars)")
    print(f"Python+C++ throughput: {wps:,.0f} words/sec ({elapsed:.3f} s)")
    print(f"DER (Char Error Rate)  : {metrics['der_pct']:.2f}%  (Char Acc: {metrics['char_acc_pct']:.2f}%)")
    print(f"WER (Word Error Rate)  : {metrics['wer_pct']:.2f}%  (Word Acc: {metrics['word_acc_pct']:.2f}%)")
    print(f"Irab Error (Last Char) : {metrics['irab_err_pct']:.2f}%  (Irab Acc: {metrics['irab_acc_pct']:.2f}%)")

    print("\nSample predictions:")
    for g, p in zip(gold_lines[:3], pred_lines[:3]):
        print(f"  GOLD: {g}")
        print(f"  PRED: {p}\n")


if __name__ == "__main__":
    main()
