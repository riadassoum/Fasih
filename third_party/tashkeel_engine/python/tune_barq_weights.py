import argparse
import ctypes
import os
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ENGINE_DIR = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, SCRIPT_DIR)
sys.path.insert(0, os.path.join(ENGINE_DIR, "scripts"))

from arabic_utils import extract_arabic_words, strip_diacritics
from build_model import update_v8_weights_inplace
from eval_fast import FastEngine, load_lines

V8_NAMES = ["V8W_CORE2", "V8W_CORE2_MARGIN", "V8W_EXTRA_CORE_PEN"]
V7_NAMES = [
    "V7W_NGRAM_LEFT", "V7W_RBG", "V7W_CTR3", "V7W_SKIP1",
    "V7W_CORE_BG_L", "V7W_CORE_BG_R", "V7W_CORE_BG_C",
    "V7W_TRANS3", "V7W_VALENCY", "V7W_IRAB2",
    "V7W_P2_IRAB1_SCALE", "V7W_P2_SYN_SCALE",
]


def eval_dev(eng: FastEngine, gold_words, plain_lines):
    preds = eng.diacritize_lines(plain_lines)
    ec = tc = ew = tw = ew_nc = ee = te = 0
    for gw, p in zip(gold_words, preds):
        pw = extract_arabic_words(p)
        if len(gw) != len(pw):
            continue
        for (gu, _, gd), (pu, _, pd) in zip(gw, pw):
            if gu != pu or len(gd) != len(pd) or not gd:
                continue
            tw += 1
            if gd != pd:
                ew += 1
            if gd[:-1] != pd[:-1]:
                ew_nc += 1
            for a, b in zip(gd, pd):
                tc += 1
                if a != b:
                    ec += 1
            te += 1
            if gd[-1] != pd[-1]:
                ee += 1
    der = 100.0 * ec / max(1, tc)
    wer = 100.0 * ew / max(1, tw)
    wer_nc = 100.0 * ew_nc / max(1, tw)
    irab = 100.0 * ee / max(1, te)
    # Strongly reward non-Irab & DER while guarding Irab strictly
    loss = 0.35 * der + 0.30 * wer_nc + 0.25 * irab + 0.10 * wer
    return loss, der, wer, wer_nc, irab


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--dev", default="../data/dev_balanced.txt")
    ap.add_argument("--max-lines", type=int, default=6000)
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--tune-corebg", action="store_true")
    ap.add_argument("--save", action="store_true")
    args = ap.parse_args()

    gl = load_lines(args.dev, args.max_lines)
    gold_words = [extract_arabic_words(l) for l in gl]
    plain_lines = [strip_diacritics(l) for l in gl]

    eng = FastEngine(args.model, workers=args.workers)
    eng._lib.tashkeel_get_weight.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    eng._lib.tashkeel_get_weight.restype = ctypes.c_int32
    eng._lib.tashkeel_set_weight.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int32]
    eng._lib.tashkeel_set_weight.restype = ctypes.c_int

    def get_w8():
        return [eng._lib.tashkeel_get_weight(eng._h, 200 + i) for i in range(3)]

    def set_w8(ws):
        for i, v in enumerate(ws):
            eng._lib.tashkeel_set_weight(eng._h, 200 + i, int(v))

    def get_w7():
        return [eng._lib.tashkeel_get_weight(eng._h, 100 + i) for i in range(12)]

    def set_w7(ws):
        for i, v in enumerate(ws):
            eng._lib.tashkeel_set_weight(eng._h, 100 + i, int(v))

    w8 = get_w8()
    w7 = get_w7()
    t0 = time.perf_counter()
    best_loss, der, wer, wer_nc, irab = eval_dev(eng, gold_words, plain_lines)
    init_irab = irab
    print(
        f"Init ({time.perf_counter()-t0:.2f}s): loss={best_loss:.4f} | "
        f"DER={der:.3f}% WER={wer:.3f}% WER_nc={wer_nc:.3f}% Irab={irab:.3f}% | w8={w8}",
        flush=True,
    )

    # Grid search V8W_CORE2_MARGIN (id 201) & V8W_EXTRA_CORE_PEN (id 202)
    print("--- Sweep V8W_CORE2_MARGIN ---", flush=True)
    for margin in (0, 100, 200, 300, 400, 500, 650, 800, 1000, 1300):
        trial = [w8[0], margin, w8[2]]
        set_w8(trial)
        loss, d, w, wnc, ir = eval_dev(eng, gold_words, plain_lines)
        mark = ""
        if loss < best_loss - 1e-4 and ir <= init_irab + 0.02:
            best_loss = loss
            w8 = trial
            mark = " <-- BEST"
        print(
            f"  margin={margin:4d} | loss={loss:.4f} DER={d:.3f}% WER={w:.3f}% WER_nc={wnc:.3f}% Irab={ir:.3f}%{mark}",
            flush=True,
        )
    set_w8(w8)

    print("--- Sweep V8W_EXTRA_CORE_PEN ---", flush=True)
    for pen in (0, 150, 300, 500, 750, 1000):
        trial = [w8[0], w8[1], pen]
        set_w8(trial)
        loss, d, w, wnc, ir = eval_dev(eng, gold_words, plain_lines)
        mark = ""
        if loss < best_loss - 1e-4 and ir <= init_irab + 0.02:
            best_loss = loss
            w8 = trial
            mark = " <-- BEST"
        print(
            f"  ec_pen={pen:4d} | loss={loss:.4f} DER={d:.3f}% WER={w:.3f}% WER_nc={wnc:.3f}% Irab={ir:.3f}%{mark}",
            flush=True,
        )
    set_w8(w8)

    # Fine-tune margin around winner
    for step in (50, 25):
        for d_dir in (-1, 1):
            cand = max(0, w8[1] + d_dir * step)
            trial = [w8[0], cand, w8[2]]
            set_w8(trial)
            loss, d, w, wnc, ir = eval_dev(eng, gold_words, plain_lines)
            if loss < best_loss - 1e-4 and ir <= init_irab + 0.02:
                best_loss = loss
                w8 = trial
                print(
                    f"  [fine] margin={cand:4d} | loss={loss:.4f} DER={d:.3f}% WER={w:.3f}% WER_nc={wnc:.3f}% Irab={ir:.3f}%",
                    flush=True,
                )
            else:
                set_w8(w8)

    if args.tune_corebg:
        print("--- Fine-tune V7W_CORE_BG_L/R/C ---", flush=True)
        for wid in (4, 5, 6):
            for step in (300, 150):
                for d_dir in (-1, 1):
                    cand = max(0, min(2400, w7[wid] + d_dir * step))
                    if cand == w7[wid]:
                        continue
                    trial = list(w7)
                    trial[wid] = cand
                    set_w7(trial)
                    loss, d, w, wnc, ir = eval_dev(eng, gold_words, plain_lines)
                    if loss < best_loss - 1e-4 and ir <= init_irab + 0.02:
                        best_loss = loss
                        w7 = trial
                        print(
                            f"  {V7_NAMES[wid]:16s} -> {cand:4d} | loss={loss:.4f} DER={d:.3f}% WER={w:.3f}% WER_nc={wnc:.3f}% Irab={ir:.3f}%",
                            flush=True,
                        )
                    else:
                        set_w7(w7)

    print("Final V8 weights:", dict(zip(V8_NAMES, w8)), flush=True)
    if args.tune_corebg:
        print("Final V7 CoreBg weights:", {V7_NAMES[i]: w7[i] for i in (4, 5, 6)}, flush=True)
    eng.close()
    if args.save:
        update_v8_weights_inplace(args.model, v7_weights=w7 if args.tune_corebg else None, v8_weights=w8)
        print(f"Saved tuned V8 weights into {args.model}", flush=True)


if __name__ == "__main__":
    main()
