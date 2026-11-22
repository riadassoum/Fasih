import argparse
import json
import os
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ENGINE_DIR = os.path.dirname(SCRIPT_DIR)
ROOT_DIR = os.path.dirname(os.path.dirname(ENGINE_DIR))
sys.path.insert(0, SCRIPT_DIR)
sys.path.insert(0, os.path.join(ENGINE_DIR, "python"))

from arabic_utils import compute_metrics, extract_arabic_words, strip_diacritics
from eval_fast import FastEngine, load_lines


def measure_std(eng, name, path, max_lines=50000):
    gl = load_lines(path, max_lines)
    pl = [strip_diacritics(l) for l in gl]
    t0 = time.perf_counter()
    preds = eng.diacritize_lines(pl)
    dt = time.perf_counter() - t0
    m = compute_metrics(gl, preds)
    wer_nc_err, tot_w = 0, 0
    for g, p in zip(gl, preds):
        gw, pw = extract_arabic_words(g), extract_arabic_words(p)
        if len(gw) != len(pw):
            continue
        for (gu, _, gd), (pu, _, pd) in zip(gw, pw):
            if gu != pu or len(gd) != len(pd) or not gd:
                continue
            tot_w += 1
            if gd[:-1] != pd[:-1]:
                wer_nc_err += 1
    tot_c = m["total_chars"]
    err_c = round(m["der_pct"] * tot_c / 100.0)
    tot_wm = m["total_words"]
    err_w = round(m["wer_pct"] * tot_wm / 100.0)
    tot_e = m.get("irab_total", tot_wm)
    err_e = round(m["irab_err_pct"] * tot_e / 100.0)
    # Compute exact integer counts directly so sums have zero rounding drift
    ec = tc = ew = tw = ew_nc = ee = te = 0
    for g, p in zip(gl, preds):
        gw, pw = extract_arabic_words(g), extract_arabic_words(p)
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
    return {
        "name": name,
        "sentences": len(gl),
        "words": tw,
        "chars": tc,
        "irab_tot": te,
        "err_chars": ec,
        "err_words": ew,
        "err_words_nc": ew_nc,
        "err_irab": ee,
        "der_pct": 100.0 * ec / max(1, tc),
        "wer_pct": 100.0 * ew / max(1, tw),
        "wer_nc_pct": 100.0 * ew_nc / max(1, tw),
        "irab_err_pct": 100.0 * ee / max(1, te),
        "time_s": dt,
    }


def measure_mr(eng, name, path):
    gl = load_lines(path, 50000)
    plain_lines = []
    split_toks = []
    for line in gl:
        toks = line.split()
        split_toks.append(toks)
        plain_lines.append(" ".join(strip_diacritics(t.split("/")[0]) for t in toks))
    t0 = time.perf_counter()
    pred_lines = eng.diacritize_lines(plain_lines)
    dt = time.perf_counter() - t0
    tc = ec = tw = ew = ew_nc = te = ee = 0
    for toks, pred_line in zip(split_toks, pred_lines):
        p_toks = pred_line.split()
        if len(p_toks) != len(toks):
            continue
        for g_tok, p_tok in zip(toks, p_toks):
            pw = extract_arabic_words(p_tok)
            best = None
            for alt in g_tok.split("/"):
                gw = extract_arabic_words(alt)
                if len(gw) != len(pw):
                    continue
                bec = bee = bew = bew_nc = btc = btw = bte = 0
                ok = True
                for (gu, _, gd), (pu, _, pd) in zip(gw, pw):
                    if gu != pu or len(gd) != len(pd):
                        ok = False
                        break
                    btw += 1
                    if gd != pd:
                        bew += 1
                    if gd[:-1] != pd[:-1]:
                        bew_nc += 1
                    for a, b in zip(gd, pd):
                        btc += 1
                        if a != b:
                            bec += 1
                    if gd:
                        bte += 1
                        if gd[-1] != pd[-1]:
                            bee += 1
                if ok and (best is None or (bec, bee, bew) < (best[0], best[1], best[2])):
                    best = (bec, bee, bew, bew_nc, btc, btw, bte)
            if best:
                tc += best[4]
                ec += best[0]
                tw += best[5]
                ew += best[2]
                ew_nc += best[3]
                te += best[6]
                ee += best[1]
    return {
        "name": name,
        "sentences": len(gl),
        "words": tw,
        "chars": tc,
        "irab_tot": te,
        "err_chars": ec,
        "err_words": ew,
        "err_words_nc": ew_nc,
        "err_irab": ee,
        "der_pct": 100.0 * ec / max(1, tc),
        "wer_pct": 100.0 * ew / max(1, tw),
        "wer_nc_pct": 100.0 * ew_nc / max(1, tw),
        "irab_err_pct": 100.0 * ee / max(1, te),
        "time_s": dt,
    }


def aggregate(items, name="UNIFIED_ALL"):
    tot = {
        "name": name,
        "sentences": sum(x["sentences"] for x in items),
        "words": sum(x["words"] for x in items),
        "chars": sum(x["chars"] for x in items),
        "irab_tot": sum(x["irab_tot"] for x in items),
        "err_chars": sum(x["err_chars"] for x in items),
        "err_words": sum(x["err_words"] for x in items),
        "err_words_nc": sum(x["err_words_nc"] for x in items),
        "err_irab": sum(x["err_irab"] for x in items),
        "time_s": sum(x["time_s"] for x in items),
    }
    tot["der_pct"] = 100.0 * tot["err_chars"] / max(1, tot["chars"])
    tot["wer_pct"] = 100.0 * tot["err_words"] / max(1, tot["words"])
    tot["wer_nc_pct"] = 100.0 * tot["err_words_nc"] / max(1, tot["words"])
    tot["irab_err_pct"] = 100.0 * tot["err_irab"] / max(1, tot["irab_tot"])
    return tot


def fmt_row(r, prev=None):
    base = (
        f"{r['name']:18s} | N={r['sentences']:5d} W={r['words']:6d} | "
        f"DER={r['der_pct']:.3f}% WER={r['wer_pct']:.3f}% "
        f"WER_nc={r['wer_nc_pct']:.3f}% Irab={r['irab_err_pct']:.3f}% "
        f"(err_irab={r['err_irab']}/{r['irab_tot']}) | {r['time_s']:.2f}s"
    )
    if prev:
        di = r["err_irab"] - prev["err_irab"]
        dw = r["err_words"] - prev["err_words"]
        dc = r["err_chars"] - prev["err_chars"]
        rel_i = 100.0 * di / max(1, prev["err_irab"])
        base += f" | dChar={dc:+d} dWord={dw:+d} dIrab={di:+d} ({rel_i:+.2f}%)"
    return base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default=os.path.join(ENGINE_DIR, "models", "barq.tash"))
    ap.add_argument("--save", default="")
    ap.add_argument("--compare", default="")
    args = ap.parse_args()

    workers = min(8, max(2, os.cpu_count() or 4))
    eng = FastEngine(args.model, workers=workers)
    bench_dir = os.path.join(ROOT_DIR, "tashkeel_bench")
    results = [
        measure_std(eng, "WikiNews-2014", os.path.join(bench_dir, "wikinews_2014.txt")),
        measure_mr(eng, "WikiNews-2014-MR", os.path.join(bench_dir, "wikinews_2014_multiref.txt")),
        measure_mr(eng, "WikiNews-2024-MR", os.path.join(bench_dir, "wikinews_2024.txt")),
        measure_std(eng, "TigreVal-5k", os.path.join(bench_dir, "tigre_val_sample.txt"), 5000),
        measure_std(eng, "TigreTest-5k", os.path.join(bench_dir, "tigre_test_sample.txt"), 5000),
    ]
    eng.close()

    unified = aggregate(results, "UNIFIED_ALL_5")
    msa_uni = aggregate(results[:3], "UNIFIED_MSA_3")
    cls_uni = aggregate(results[3:], "UNIFIED_CLS_2")

    prev_map = {}
    if args.compare and os.path.exists(args.compare):
        with open(args.compare, encoding="utf-8") as f:
            data = json.load(f)
        for x in data.get("benchmarks", []) + [data.get("unified"), data.get("msa_unified"), data.get("cls_unified")]:
            if x and "name" in x:
                prev_map[x["name"]] = x

    for r in results:
        print(fmt_row(r, prev_map.get(r["name"])))
    print("-" * 110)
    print(fmt_row(msa_uni, prev_map.get("UNIFIED_MSA_3")))
    print(fmt_row(cls_uni, prev_map.get("UNIFIED_CLS_2")))
    print(fmt_row(unified, prev_map.get("UNIFIED_ALL_5")))

    if args.save:
        os.makedirs(os.path.dirname(os.path.abspath(args.save)), exist_ok=True)
        with open(args.save, "w", encoding="utf-8") as f:
            json.dump(
                {
                    "benchmarks": results,
                    "msa_unified": msa_uni,
                    "cls_unified": cls_uni,
                    "unified": unified,
                },
                f,
                ensure_ascii=False,
                indent=2,
            )


if __name__ == "__main__":
    main()
