import argparse
from typing import List

from arabic_utils import compute_metrics, strip_diacritics
from fst_builder import NUM_WEIGHTS, update_tash_weights
from tashkeel_py import TashkeelEngine

WEIGHT_NAMES = [
    "W_LEX",
    "W_BIGRAM",
    "W_TRANS",
    "W_OOV_TREE",
    "W_IRAB_TREE",
    "W_RULE",
    "W_UNARY_RULE",
    "W_PROCLITIC_PEN",
    "W_SUFFIX_PEN",
    "W_RELAXED_PEN",
    "W_EXPAND_PEN",
    "W_CONSTRAINT",
    "W_MORPH_TREE",
]


def evaluate_weights(engine: TashkeelEngine, gold_lines: List[str], plain_lines: List[str], weights: List[int]) -> float:
    for wid, val in enumerate(weights):
        engine.set_weight(wid, val)
    preds = [engine.diacritize(l) for l in plain_lines]
    m = compute_metrics(gold_lines, preds)
    return 0.50 * m["der_pct"] + 0.35 * m["irab_err_pct"] + 0.15 * m["wer_pct"]


def main():
    parser = argparse.ArgumentParser(description="Post-training coordinate search calibration for .tash model weights")
    parser.add_argument("--model", required=True, help="Path to v2 .tash model")
    parser.add_argument("--dev", required=True, help="Path to diacritized held-out dev file")
    parser.add_argument("--max-lines", type=int, default=2000, help="Lines from dev set to tune on")
    parser.add_argument("--rounds", type=int, default=2, help="Coordinate descent passes")
    parser.add_argument("--save", action="store_true", help="Write tuned weights back into the .tash file")
    args = parser.parse_args()

    engine = TashkeelEngine(args.model)
    with open(args.dev, "r", encoding="utf-8", errors="ignore") as f:
        gold = [l.strip() for _, l in zip(range(args.max_lines), f) if l.strip()]
    plain = [strip_diacritics(l) for l in gold]

    weights = [engine.get_weight(i) for i in range(NUM_WEIGHTS)]
    MIN_BOUNDS = {7: 450, 8: 450, 11: 900}
    MAX_BOUNDS = {}
    for wid, val in enumerate(weights):
        if wid in MIN_BOUNDS and val < MIN_BOUNDS[wid]:
            weights[wid] = MIN_BOUNDS[wid]
        if wid in MAX_BOUNDS and val > MAX_BOUNDS[wid]:
            weights[wid] = MAX_BOUNDS[wid]
    best_loss = evaluate_weights(engine, gold, plain, weights)
    print(f"Initial combined loss (0.5*DER + 0.35*IrabErr + 0.15*WER): {best_loss:.4f}%")

    steps = [400, 200, 100]
    for r in range(args.rounds):
        for wid in range(NUM_WEIGHTS):
            for step in steps:
                for direction in (-1, 1):
                    cand_val = max(MIN_BOUNDS.get(wid, 0), weights[wid] + direction * step)
                    if wid in MAX_BOUNDS:
                        cand_val = min(MAX_BOUNDS[wid], cand_val)
                    if cand_val == weights[wid]:
                        continue
                    trial = list(weights)
                    trial[wid] = cand_val
                    loss = evaluate_weights(engine, gold, plain, trial)
                    if loss < best_loss - 1e-4:
                        best_loss = loss
                        weights = trial
                        print(f"  [round {r+1}] {WEIGHT_NAMES[wid]} -> {cand_val} | loss={best_loss:.4f}%")

    print("Final weights:", dict(zip(WEIGHT_NAMES, weights)))
    if args.save:
        update_tash_weights(args.model, weights)
        print(f"Saved tuned weights into {args.model}")


if __name__ == "__main__":
    main()
