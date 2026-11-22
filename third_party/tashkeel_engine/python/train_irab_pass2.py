import argparse
import os
import sys
import time
import numpy as np
import lightgbm as lgb

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arabic_utils import NUM_DIACRITICS
from fst_builder import export_lightgbm_ensemble, convert_tree_v2_to_v5

NUM_FEATS = 96


def load_bin(prefix: str):
    y = np.fromfile(prefix + "_y.bin", dtype=np.uint8)
    p1 = np.fromfile(prefix + "_p1.bin", dtype=np.uint8)
    X = np.fromfile(prefix + "_X.bin", dtype=np.int8).reshape(-1, NUM_FEATS)
    assert len(X) == len(y) == len(p1)
    return X, y, p1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--train-prefix", required=True)
    ap.add_argument("--dev-prefix", required=True)
    ap.add_argument("--out-blob", required=True)
    ap.add_argument("--rounds", type=int, default=120)
    ap.add_argument("--max-depth", type=int, default=8)
    ap.add_argument("--num-leaves", type=int, default=63)
    ap.add_argument("--lr", type=float, default=0.08)
    ap.add_argument("--min-data", type=int, default=80)
    ap.add_argument("--l2", type=float, default=2.0)
    ap.add_argument("--scale", type=float, default=450.0)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--max-rows", type=int, default=0)
    ap.add_argument("--ff", type=float, default=0.75)
    ap.add_argument("--bf", type=float, default=0.80)
    args = ap.parse_args()

    t0 = time.time()
    X_tr, y_tr, p1_tr = load_bin(args.train_prefix)
    if args.max_rows > 0 and len(X_tr) > args.max_rows:
        X_tr = X_tr[: args.max_rows]
        y_tr = y_tr[: args.max_rows]
        p1_tr = p1_tr[: args.max_rows]
    X_dv, y_dv, p1_dv = load_bin(args.dev_prefix)
    print(
        f"Loaded train={X_tr.shape} (P1 err={100.0*np.mean(y_tr != p1_tr):.3f}%) | "
        f"dev={X_dv.shape} (P1 err={100.0*np.mean(y_dv != p1_dv):.3f}%)",
        flush=True,
    )

    dtrain = lgb.Dataset(X_tr, label=y_tr, free_raw_data=True)
    dval = lgb.Dataset(X_dv, label=y_dv, reference=dtrain, free_raw_data=False)

    params = {
        "objective": "multiclass",
        "num_class": NUM_DIACRITICS,
        "metric": "multi_logloss",
        "learning_rate": args.lr,
        "num_leaves": args.num_leaves,
        "max_depth": args.max_depth,
        "min_data_in_leaf": args.min_data,
        "feature_fraction": args.ff,
        "bagging_fraction": args.bf,
        "bagging_freq": 1,
        "lambda_l2": args.l2,
        "max_bin": 128,
        "subsample_for_bin": 200000,
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
            lgb.early_stopping(stopping_rounds=15, verbose=True),
            lgb.log_evaluation(period=15),
        ],
    )
    best_iter = booster.best_iteration if booster.best_iteration and booster.best_iteration > 0 else args.rounds
    probs_dv = booster.predict(X_dv, num_iteration=best_iter)
    pred_dv = np.argmax(probs_dv, axis=1)
    p1_err = 100.0 * np.mean(y_dv != p1_dv)
    i2_err = 100.0 * np.mean(y_dv != pred_dv)
    print(f"Dev multi-end Irab err: Pass-1={p1_err:.3f}% -> Irab2 standalone={i2_err:.3f}% (delta={i2_err - p1_err:+.3f}%) at iter={best_iter}")

    # Feature importance top 15
    imp = booster.feature_importance(importance_type="gain")
    top_f = np.argsort(-imp)[:15]
    print("Top 15 Irab2 features by gain:", [(int(f), round(float(imp[f]), 1)) for f in top_f])

    blob_v2 = export_lightgbm_ensemble(booster, NUM_DIACRITICS, scale=args.scale)
    blob_v5, ok_v5 = convert_tree_v2_to_v5(blob_v2)
    print(f"Exported Irab2 blob: v2={len(blob_v2)/1e6:.2f} MB | v5_4B={len(blob_v5)/1e6:.2f} MB (ok_v5={ok_v5}) in {time.time()-t0:.1f}s")
    with open(args.out_blob, "wb") as f:
        f.write(blob_v2)
    print(f"Saved raw v2 blob to {args.out_blob} (build_v7_model.py will pack to 4B/node automatically).")


if __name__ == "__main__":
    main()
