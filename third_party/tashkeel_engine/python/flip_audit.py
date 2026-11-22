import argparse
import collections
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ENGINE_DIR = os.path.dirname(SCRIPT_DIR)
ROOT_DIR = os.path.dirname(os.path.dirname(ENGINE_DIR))
sys.path.insert(0, SCRIPT_DIR)
sys.path.insert(0, os.path.join(ENGINE_DIR, "scripts"))

from arabic_utils import DIAC_ID_TO_STR, extract_arabic_words, strip_diacritics
from eval_fast import FastEngine, load_lines


def render_w(letters, diacs):
    return "".join(ch + DIAC_ID_TO_STR[d] for ch, d in zip(letters, diacs))


def audit_dataset(eng_ref: FastEngine, eng_new: FastEngine, name: str, path: str, max_lines: int = 50000):
    gl = load_lines(path, max_lines)
    pl = [strip_diacritics(l) for l in gl]
    p_ref = eng_ref.diacritize_lines(pl)
    p_new = eng_new.diacritize_lines(pl)

    nc_w = nc_l = ir_w = ir_l = fw_w = fw_l = 0
    win_ex = collections.Counter()
    loss_ex = collections.Counter()

    for g, r, n in zip(gl, p_ref, p_new):
        gw, rw, nw = extract_arabic_words(g), extract_arabic_words(r), extract_arabic_words(n)
        if not (len(gw) == len(rw) == len(nw)):
            continue
        for (gu, gls, gd), (ru, _, rd), (nu, _, nd) in zip(gw, rw, nw):
            if not (gu == ru == nu and len(gd) == len(rd) == len(nd) and gd):
                continue
            r_nc_ok = (rd[:-1] == gd[:-1])
            n_nc_ok = (nd[:-1] == gd[:-1])
            if n_nc_ok and not r_nc_ok:
                nc_w += 1
                win_ex[(render_w(gu, rd), render_w(gu, nd), render_w(gu, gd))] += 1
            elif r_nc_ok and not n_nc_ok:
                nc_l += 1
                loss_ex[(render_w(gu, rd), render_w(gu, nd), render_w(gu, gd))] += 1
            r_ir_ok = (rd[-1] == gd[-1])
            n_ir_ok = (nd[-1] == gd[-1])
            if n_ir_ok and not r_ir_ok:
                ir_w += 1
            elif r_ir_ok and not n_ir_ok:
                ir_l += 1
            r_fw_ok = (rd == gd)
            n_fw_ok = (nd == gd)
            if n_fw_ok and not r_fw_ok:
                fw_w += 1
            elif r_fw_ok and not n_fw_ok:
                fw_l += 1

    print(
        f"{name:16s} | NonIrab: +{nc_w:4d} / -{nc_l:4d} (net={nc_w-nc_l:+5d}, W/L={nc_w/max(1,nc_l):.2f}x) | "
        f"Irab: +{ir_w:4d} / -{ir_l:4d} (net={ir_w-ir_l:+4d}) | "
        f"FullW: +{fw_w:4d} / -{fw_l:4d} (net={fw_w-fw_l:+5d})"
    )
    return nc_w, nc_l, ir_w, ir_l, fw_w, fw_l, win_ex, loss_ex


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default="../data/barq.tash")
    ap.add_argument("--new", required=True)
    ap.add_argument("--show-top", type=int, default=12)
    args = ap.parse_args()

    eng_ref = FastEngine(args.ref, workers=4)
    eng_new = FastEngine(args.new, workers=4)

    bench_dir = os.path.join(ROOT_DIR, "tashkeel_bench")
    sets = [
        ("WikiNews-2014", os.path.join(bench_dir, "wikinews_2014.txt"), 50000),
        ("TigreVal-5k", os.path.join(bench_dir, "tigre_val_sample.txt"), 5000),
        ("TigreTest-5k", os.path.join(bench_dir, "tigre_test_sample.txt"), 5000),
    ]
    tot_wins = collections.Counter()
    tot_losses = collections.Counter()
    s_nc_w = s_nc_l = s_ir_w = s_ir_l = s_fw_w = s_fw_l = 0
    for name, path, max_l in sets:
        if not os.path.exists(path):
            continue
        nw, nl, iw, il, fw, fl, wex, lex = audit_dataset(eng_ref, eng_new, name, path, max_l)
        s_nc_w += nw; s_nc_l += nl; s_ir_w += iw; s_ir_l += il; s_fw_w += fw; s_fw_l += fl
        tot_wins.update(wex); tot_losses.update(lex)
    print("-" * 105)
    print(
        f"{'TOTAL_STD_3':16s} | NonIrab: +{s_nc_w:4d} / -{s_nc_l:4d} (net={s_nc_w-s_nc_l:+5d}, W/L={s_nc_w/max(1,s_nc_l):.2f}x) | "
        f"Irab: +{s_ir_w:4d} / -{s_ir_l:4d} (net={s_ir_w-s_ir_l:+4d}) | "
        f"FullW: +{s_fw_w:4d} / -{s_fw_l:4d} (net={s_fw_w-s_fw_l:+5d})"
    )
    print(f"\nTop {args.show_top} Non-Irab WINS (ref -> new | gold):")
    for (r, n, g), cnt in tot_wins.most_common(args.show_top):
        print(f"  {cnt:4d}x : {r} -> {n}  (gold: {g})")
    print(f"\nTop {args.show_top} Non-Irab LOSSES (ref -> new | gold):")
    for (r, n, g), cnt in tot_losses.most_common(args.show_top):
        print(f"  {cnt:4d}x : {r} -> {n}  (gold: {g})")

    eng_ref.close()
    eng_new.close()


if __name__ == "__main__":
    main()
