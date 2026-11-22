import argparse
import os
import random
import sys
import time
from collections import Counter

import lightgbm as lgb
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arabic_utils import (
    DIAC_DAMMA,
    DIAC_FATHA,
    DIAC_KASRA,
    DIAC_NONE,
    DIAC_SHADDA,
    DIAC_SUKUN,
    NUM_DIACRITICS,
    extract_arabic_words,
)
from fst_builder import convert_tree_v2_to_v5, export_lightgbm_ensemble
from grammar import (
    S_ALEF,
    S_ALEF_HAMZA_A,
    S_ALEF_HAMZA_B,
    S_ALEF_MAKSURA,
    S_ALEF_WASLA,
    S_BEH,
    S_FEH,
    S_HEH,
    S_KAF,
    S_LAM,
    S_MEEM,
    S_NOON,
    S_SEEN,
    S_TEH,
    S_TEH_MARBUTA,
    S_WAW,
    S_YEH,
    SUN_LETTERS,
    analyze_surface,
)

S_HAMZA = 0
S_ALEF_MADDA = 1
S_WAW_HAMZA = 3
S_YEH_HAMZA = 5
S_DAL = 14
S_REH = 16
S_GHAIN = 25

OOV_V8_NUM_FEATURES = 38
LONG_VOWELS = {S_ALEF, S_WAW, S_YEH, S_ALEF_MAKSURA}


def diac_fam_sh(d: int) -> int:
    if d < 0:
        return -1
    sh = 8 if d >= 8 else 0
    if d in (2, 6, 10, 13):
        return 1 | sh
    if d in (1, 5, 9, 12):
        return 2 | sh
    if d in (3, 7, 11, 14):
        return 3 | sh
    if d == 4:
        return 4 | sh
    return sh


def char_weak_code(s: int) -> int:
    if s == S_ALEF_HAMZA_A: return 1
    if s == S_ALEF_HAMZA_B: return 2
    if s == S_ALEF_MADDA: return 3
    if s == S_WAW_HAMZA: return 4
    if s == S_YEH_HAMZA: return 5
    if s == S_HAMZA: return 6
    if s in (S_ALEF, S_ALEF_WASLA): return 7
    if s == S_WAW: return 8
    if s in (S_YEH, S_ALEF_MAKSURA): return 9
    if s == S_TEH_MARBUTA: return 10
    return 0


def stem_prefix_pattern(syms: bytes, al_len: int) -> int:
    n = len(syms)
    st = al_len
    if st >= n:
        return 0
    sn = n - st
    w0 = syms[st]
    w1 = syms[st + 1] if sn >= 2 else -1
    w2 = syms[st + 2] if sn >= 3 else -1
    if w0 == S_MEEM and sn >= 4:
        if sn >= 6 and w1 == S_SEEN and w2 == S_TEH:
            return 3  # مُسْتَـ
        if sn >= 5 and w1 == S_TEH:
            return 2  # مُتَـ
        if sn >= 5 and w1 == S_NOON:
            return 4  # مُنْـ
        return 1  # مُـ / مَـ
    if w0 in (S_ALEF, S_ALEF_WASLA) and sn >= 5:
        if sn >= 6 and w1 == S_SEEN and w2 == S_TEH:
            return 5  # اسْتَـ
        if w1 == S_NOON:
            return 7  # انْـ
        if w2 == S_TEH:
            return 8  # افْتَـ
    if w0 == S_TEH and sn >= 5:
        return 6  # تَفَعَّل / تَفَاعَل
    if w0 in (S_ALEF_HAMZA_A, S_ALEF_HAMZA_B) and sn >= 4:
        return 9  # أَفْعَل / إِفْعَال
    if w0 in (S_YEH, S_TEH, S_NOON) and sn >= 4:
        return 10
    return 0


def stem_suffix_pattern(syms: bytes) -> int:
    n = len(syms)
    if n < 3:
        return 0
    l1 = syms[n - 1]
    l2 = syms[n - 2]
    l3 = syms[n - 3] if n >= 3 else -1
    if l1 == S_TEH_MARBUTA:
        return 7 if (n >= 4 and l2 == S_YEH) else 1
    if n >= 5 and l2 == S_ALEF and l1 == S_TEH:
        return 2  # ـات
    if n >= 5 and l2 == S_WAW and l1 == S_NOON:
        return 3  # ـون
    if n >= 5 and l2 == S_YEH and l1 == S_NOON:
        return 4  # ـين
    if n >= 5 and l2 == S_ALEF and l1 == S_NOON:
        return 5  # ـان
    if n >= 4 and l1 == S_YEH:
        return 6  # ـي
    if n >= 5 and ((l2 == S_HEH and l1 in (S_ALEF, S_MEEM, S_NOON)) or
                   (l2 == S_KAF and l1 in (S_MEEM, S_NOON)) or
                   (l2 == S_NOON and l1 == S_ALEF)):
        return 8  # ـها/هم/هن/كم/نا
    if n >= 4 and l1 in (S_HEH, S_KAF):
        return 9  # ـه/ك
    if n >= 4 and ((l2 == S_WAW and l1 == S_ALEF) or (l2 == S_TEH and l1 == S_MEEM) or l1 == S_TEH):
        return 10
    if n >= 4 and l1 == S_ALEF:
        return 11
    return 0


def is_likely_loanword(syms: bytes, al_len: int) -> int:
    n = len(syms)
    sn = n - al_len
    if sn < 4:
        return 0
    lv = sum(1 for k in range(al_len, n) if syms[k] in (S_ALEF, S_WAW, S_YEH))
    has_gh = any(syms[k] == S_GHAIN for k in range(al_len, n))
    if sn >= 5 and lv >= 2 and syms[n - 1] in (S_WAW, S_ALEF, S_YEH):
        return 2
    if sn >= 6 and (lv >= 3 or (has_gh and lv >= 2)):
        return 1
    return 0


def proclitic_code(syms: bytes, al_len: int) -> int:
    if al_len > 0 or len(syms) < 4:
        return 0
    s0 = syms[0]
    if s0 == S_WAW: return 1
    if s0 == S_FEH: return 2
    if s0 == S_BEH: return 3
    if s0 == S_LAM: return 4
    if s0 == S_KAF: return 5
    if s0 == S_SEEN: return 6
    return 0


def extract_oov_v8_features(syms: bytes, diacs, si: dict):
    n = len(syms)
    al_len = si["al_len"]
    stem_len = n - al_len
    last_sym = syms[n - 1]
    cpos = n - 2 if (n >= 3 and last_sym == S_ALEF) else n - 1
    is_sun_al = 1 if (al_len >= 2 and al_len < n and syms[al_len] in SUN_LETTERS) else 0
    starts_meem = 1 if (al_len < n and syms[al_len] == S_MEEM) else 0
    starts_imp = si["verb_like"]
    ends_tm = 1 if last_sym == S_TEH_MARBUTA else 0
    ends_at = 1 if (n >= 4 and syms[n - 2] == S_ALEF and last_sym == S_TEH) else 0
    ends_wa = 1 if (n >= 4 and syms[n - 2] == S_WAW and last_sym == S_ALEF) else 0

    st0 = syms[al_len] if al_len < n else -1
    st1 = syms[al_len + 1] if al_len + 1 < n else -1
    st2 = syms[al_len + 2] if al_len + 2 < n else -1
    penult = syms[n - 2] if n >= 2 else -1
    antepen = syms[n - 3] if n >= 3 else -1
    sp_pat = stem_prefix_pattern(syms, al_len)
    ss_pat = stem_suffix_pattern(syms)
    loan = is_likely_loanword(syms, al_len)
    pcode = proclitic_code(syms, al_len)

    rows = []
    labels = []
    for i in range(n):
        d = diacs[i]
        # Skip noisy unvocalized middle consonants (garbage data)
        lv_mask = (
            (1 if (i >= 1 and syms[i - 1] in LONG_VOWELS) else 0)
            | (2 if (i + 1 < n and syms[i + 1] in LONG_VOWELS) else 0)
            | (4 if (syms[i] in LONG_VOWELS) else 0)
        )
        d1 = diacs[i - 1] if i >= 1 else -1
        d2 = diacs[i - 2] if i >= 2 else -1
        d3 = diacs[i - 3] if i >= 3 else -1
        feat = [
            syms[i - 3] if i >= 3 else -1,
            syms[i - 2] if i >= 2 else -1,
            syms[i - 1] if i >= 1 else -1,
            syms[i],
            syms[i + 1] if i + 1 < n else -1,
            syms[i + 2] if i + 2 < n else -1,
            syms[i + 3] if i + 3 < n else -1,
            min(47, i),
            min(47, n - 1 - i),
            min(47, n),
            al_len,
            max(-8, min(47, i - al_len)),
            d1,
            d2,
            min(47, stem_len),
            last_sym,
            is_sun_al,
            starts_meem,
            starts_imp,
            ends_tm,
            ends_at,
            ends_wa,
            syms[i - 4] if i >= 4 else -1,
            syms[i + 4] if i + 4 < n else -1,
            d3,
            st0,
            st1,
            st2,
            penult,
            antepen,
            sp_pat,
            ss_pat,
            lv_mask,
            max(-8, min(47, cpos - i)),
            loan,
            pcode,
            diac_fam_sh(d1),
            char_weak_code(syms[i]),
        ]
        rows.append(feat)
        labels.append(d)
    return rows, labels


def build_type_balanced_dataset(corpus_path: str, max_lines: int, max_chars: int, max_per_word: int = 3):
    X_list = []
    y_list = []
    seen = Counter()
    with open(corpus_path, "r", encoding="utf-8", errors="ignore") as f:
        for li, line in enumerate(f):
            if li >= max_lines:
                break
            words = extract_arabic_words(line)
            for undiac, syms, diacs in words:
                n = len(syms)
                if n < 3 or n > 32:
                    continue
                if seen[undiac] >= max_per_word:
                    continue
                # Quality filter: skip partially unvocalized or malformed words
                non_weak = sum(1 for k in range(n - 1) if syms[k] not in (S_ALEF, S_ALEF_WASLA, S_ALEF_MADDA, S_WAW, S_YEH, S_ALEF_MAKSURA))
                diac_nw = sum(1 for k in range(n - 1) if syms[k] not in (S_ALEF, S_ALEF_WASLA, S_ALEF_MADDA, S_WAW, S_YEH, S_ALEF_MAKSURA) and diacs[k] != DIAC_NONE)
                if non_weak >= 2 and diac_nw * 100 < non_weak * 70:
                    continue
                seen[undiac] += 1
                si = analyze_surface(syms)
                rows, labels = extract_oov_v8_features(syms, diacs, si)
                X_list.extend(rows)
                y_list.extend(labels)
                if len(X_list) >= max_chars:
                    return np.asarray(X_list, dtype=np.int8), np.asarray(y_list, dtype=np.uint8), len(seen)
    return np.asarray(X_list, dtype=np.int8), np.asarray(y_list, dtype=np.uint8), len(seen)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--train-corpus", required=True)
    ap.add_argument("--dev-corpus", required=True)
    ap.add_argument("--out-blob", required=True)
    ap.add_argument("--max-lines", type=int, default=600000)
    ap.add_argument("--max-chars", type=int, default=1600000)
    ap.add_argument("--rounds", type=int, default=90)
    ap.add_argument("--num-leaves", type=int, default=63)
    ap.add_argument("--max-depth", type=int, default=8)
    ap.add_argument("--lr", type=float, default=0.10)
    ap.add_argument("--threads", type=int, default=4)
    args = ap.parse_args()

    t0 = time.time()
    X_tr, y_tr, n_types = build_type_balanced_dataset(args.train_corpus, args.max_lines, args.max_chars, max_per_word=3)
    X_dv, y_dv, dv_types = build_type_balanced_dataset(args.dev_corpus, 10000, 120000, max_per_word=2)
    print(f"OOV-V8 dataset built in {time.time()-t0:.1f}s: train={X_tr.shape} ({n_types:,} types) | dev={X_dv.shape} ({dv_types:,} types)", flush=True)

    dtrain = lgb.Dataset(X_tr, label=y_tr, free_raw_data=True)
    dval = lgb.Dataset(X_dv, label=y_dv, reference=dtrain, free_raw_data=False)
    params = {
        "objective": "multiclass",
        "num_class": NUM_DIACRITICS,
        "metric": "multi_logloss",
        "learning_rate": args.lr,
        "num_leaves": args.num_leaves,
        "max_depth": args.max_depth,
        "min_data_in_leaf": 60,
        "feature_fraction": 0.8,
        "bagging_fraction": 0.8,
        "bagging_freq": 1,
        "lambda_l2": 2.0,
        "max_bin": 128,
        "force_col_wise": True,
        "verbose": -1,
        "num_threads": args.threads,
        "seed": 20261122,
    }
    booster = lgb.train(
        params,
        dtrain,
        num_boost_round=args.rounds,
        valid_sets=[dval],
        callbacks=[
            lgb.early_stopping(stopping_rounds=12, verbose=True),
            lgb.log_evaluation(period=15),
        ],
    )
    best_iter = booster.best_iteration if booster.best_iteration and booster.best_iteration > 0 else args.rounds
    preds = np.argmax(booster.predict(X_dv, num_iteration=best_iter), axis=1)
    acc = 100.0 * np.mean(preds == y_dv)
    print(f"OOV-V8 dev char accuracy={acc:.3f}% (err={100.0-acc:.3f}%) at iter={best_iter}", flush=True)

    blob_v2 = export_lightgbm_ensemble(booster, NUM_DIACRITICS, scale=500.0)
    blob_v5, ok_v5 = convert_tree_v2_to_v5(blob_v2)
    print(f"Exported OOV-V8 blob: v2={len(blob_v2)/1e6:.2f} MB | v5_4B={len(blob_v5)/1e6:.2f} MB (ok_v5={ok_v5}) in {time.time()-t0:.1f}s")
    assert ok_v5
    with open(args.out_blob, "wb") as f:
        f.write(blob_v5)
    print(f"Saved packed v5_4B OOV-V8 blob to {args.out_blob}")


if __name__ == "__main__":
    main()
