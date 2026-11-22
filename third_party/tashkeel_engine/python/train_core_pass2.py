import argparse
import math
import os
import struct
import sys
import time
import numpy as np
import lightgbm as lgb

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fst_builder import (
    FLAT_TREE_NODE_STRUCT,
    NODE_LEAF,
    NODE_NUM,
    TREE_HDR_STRUCT,
    convert_tree_v2_to_v5,
)


def load_bin(prefix: str, num_feats: int):
    y = np.fromfile(prefix + "_y.bin", dtype=np.uint8)
    p1 = np.fromfile(prefix + "_p1.bin", dtype=np.uint8)
    gid = np.fromfile(prefix + "_gid.bin", dtype=np.uint32)
    X = np.fromfile(prefix + "_X.bin", dtype=np.int8).reshape(-1, num_feats)
    assert len(X) == len(y) == len(p1) == len(gid)
    return X, y, p1, gid


def build_group_index(gid: np.ndarray):
    change = np.flatnonzero(np.diff(gid)) + 1
    starts = np.concatenate(([0], change))
    ends = np.concatenate((change, [len(gid)]))
    lens = ends - starts
    return starts, ends, lens


def make_group_softmax_obj(starts: np.ndarray, lens: np.ndarray, y_f32: np.ndarray):
    row_to_grp = np.repeat(np.arange(len(starts), dtype=np.int32), lens)

    def obj(preds: np.ndarray, _dataset):
        pr = preds.astype(np.float32, copy=False)
        g_max = np.maximum.reduceat(pr, starts)
        shifted = pr - g_max[row_to_grp]
        np.exp(shifted, out=shifted)
        g_sum = np.add.reduceat(shifted, starts)
        p = shifted
        p /= g_sum[row_to_grp]
        grad = (p - y_f32).astype(np.float64)
        hess = np.maximum(p * (1.0 - p), np.float32(1e-4)).astype(np.float64)
        return grad, hess

    return obj


def make_group_softmax_eval(starts: np.ndarray, lens: np.ndarray, pos_mask: np.ndarray):
    row_to_grp = np.repeat(np.arange(len(starts), dtype=np.int32), lens)

    def feval(preds: np.ndarray, _dataset):
        g_max = np.maximum.reduceat(preds, starts)
        shifted = preds - g_max[row_to_grp]
        exp_s = np.exp(shifted)
        g_sum = np.add.reduceat(exp_s, starts)
        p = exp_s / g_sum[row_to_grp]
        nll = -float(np.mean(np.log(np.maximum(p[pos_mask], 1e-12))))
        return "group_nll", nll, False

    return feval


def export_binary_as_2class(booster, best_iter: int, scale: float = 500.0) -> bytes:
    """Exports a single-logit LightGBM booster into v2 FlatTreeEnsemble with num_classes=2, class_id=1.
    In C++, Core2 computes lg = sc2[1] - sc2[0] = +sum(tree_leaves * scale)."""
    dump = booster.dump_model(num_iteration=best_iter)
    tree_infos = dump["tree_info"]
    num_trees = len(tree_infos)

    tree_roots = []
    flat_nodes = []

    for t_info in tree_infos:
        tree_roots.append(len(flat_nodes))

        def emit_node(node: dict) -> int:
            my_idx = len(flat_nodes)
            flat_nodes.append((0, 0, 0, 0, 0, 0, 0))
            if "leaf_value" in node:
                val_q = int(round(float(node["leaf_value"]) * scale))
                val_q = max(-32000, min(32000, val_q))
                flat_nodes[my_idx] = (0, val_q, 0, 0, 0, NODE_LEAF, 1)
            else:
                feat = int(node["split_feature"])
                thresh = int(math.floor(float(node["threshold"])))
                left_idx = emit_node(node["left_child"])
                right_idx = emit_node(node["right_child"])
                flat_nodes[my_idx] = (0, thresh, left_idx, right_idx, feat, NODE_NUM, 1)
            return my_idx

        emit_node(t_info["tree_structure"])

    out = bytearray(
        TREE_HDR_STRUCT.size
        + num_trees * 4
        + len(flat_nodes) * FLAT_TREE_NODE_STRUCT.size
    )
    TREE_HDR_STRUCT.pack_into(out, 0, num_trees, 2, len(flat_nodes), 0)
    off = TREE_HDR_STRUCT.size
    for r in tree_roots:
        struct.pack_into("<I", out, off, r)
        off += 4
    for n in flat_nodes:
        FLAT_TREE_NODE_STRUCT.pack_into(out, off, *n)
        off += FLAT_TREE_NODE_STRUCT.size
    return bytes(out)


def verify_v5_parity(blob_v5: bytes, X_sample: np.ndarray, ref_logits: np.ndarray, scale: float):
    nt, nc, nn, bsq = TREE_HDR_STRUCT.unpack_from(blob_v5, 0)
    roots = struct.unpack_from(f"<{nt}I", blob_v5, 16)
    nodes = struct.unpack_from(f"<{nn}I", blob_v5, 16 + nt * 4)
    diffs = []
    for i in range(min(200, len(X_sample))):
        row = X_sample[i]
        sc1 = bsq
        for r in roots:
            idx = r
            while idx < nn:
                nd = nodes[idx]
                if nd >> 31:
                    val = nd & 0xFFFF
                    if val >= 0x8000:
                        val -= 0x10000
                    sc1 += val
                    break
                feat = (nd >> 17) & 0x7F
                th = ((nd >> 24) & 0x7F) - 8
                idx += 1 if int(row[feat]) <= th else (nd & 0x1FFFF)
        diffs.append(sc1 - ref_logits[i] * scale)
    # Note: binary objective has a constant init_score offset across all rows, so check std of diffs
    std_err = float(np.std(diffs))
    print(f"V5_4B parity check over {len(diffs)} rows: std(q - raw*scale) = {std_err:.2f}", flush=True)
    assert std_err < 40.0, f"Unexpected V5 quantization drift: std={std_err}"


def eval_group_ranking(logits: np.ndarray, y: np.ndarray, is_p1: np.ndarray, gid: np.ndarray, scale: float):
    starts, ends, _ = build_group_index(gid)
    n_groups = len(starts)

    p1_ok = np.zeros(n_groups, dtype=np.uint8)
    best_ok = np.zeros(n_groups, dtype=np.uint8)
    best_diff = np.zeros(n_groups, dtype=np.float32)
    same_as_p1 = np.zeros(n_groups, dtype=np.uint8)

    for gi in range(n_groups):
        s, e = starts[gi], ends[gi]
        lg = logits[s:e]
        yg = y[s:e]
        pg = is_p1[s:e]
        p1_idx = int(np.argmax(pg))
        best_idx = int(np.argmax(lg))
        p1_ok[gi] = yg[p1_idx]
        best_ok[gi] = yg[best_idx]
        best_diff[gi] = (lg[best_idx] - lg[p1_idx]) * scale
        same_as_p1[gi] = 1 if best_idx == p1_idx else 0

    p1_err = 100.0 * (1.0 - float(np.mean(p1_ok)))
    print(f"Group eval over {n_groups:,} multi-core tokens: Pass-1 core err = {p1_err:.3f}%", flush=True)
    for margin in (0, 50, 100, 200, 300, 450, 600, 800, 1000):
        flip = (same_as_p1 == 0) & (best_diff > margin)
        final_ok = np.where(flip, best_ok, p1_ok)
        wins = int(np.sum(flip & (p1_ok == 0) & (best_ok == 1)))
        losses = int(np.sum(flip & (p1_ok == 1) & (best_ok == 0)))
        err = 100.0 * (1.0 - float(np.mean(final_ok)))
        ratio = wins / max(1, losses)
        print(
            f"  margin={margin:4d}: Core2 err={err:.3f}% (delta={err - p1_err:+.3f}%) | "
            f"flips={int(np.sum(flip)):5d} | wins={wins:4d} losses={losses:4d} net={wins - losses:+5d} (W/L={ratio:.2f}x)",
            flush=True,
        )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--train-prefix", required=True)
    ap.add_argument("--dev-prefix", required=True)
    ap.add_argument("--out-blob", required=True)
    ap.add_argument("--num-feats", type=int, default=72)
    ap.add_argument("--loss", choices=["softmax", "binary"], default="softmax")
    ap.add_argument("--rounds", type=int, default=550)
    ap.add_argument("--max-depth", type=int, default=9)
    ap.add_argument("--num-leaves", type=int, default=127)
    ap.add_argument("--lr", type=float, default=0.065)
    ap.add_argument("--min-data", type=int, default=160)
    ap.add_argument("--l2", type=float, default=8.0)
    ap.add_argument("--scale", type=float, default=500.0)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--max-rows", type=int, default=0)
    ap.add_argument("--ff", type=float, default=0.80)
    ap.add_argument("--bf", type=float, default=0.80)
    args = ap.parse_args()

    t0 = time.time()
    X_tr, y_tr, p1_tr, gid_tr = load_bin(args.train_prefix, args.num_feats)
    if args.max_rows > 0 and len(X_tr) > args.max_rows:
        cut = args.max_rows
        while cut < len(gid_tr) and gid_tr[cut] == gid_tr[cut - 1]:
            cut += 1
        X_tr, y_tr, p1_tr, gid_tr = X_tr[:cut], y_tr[:cut], p1_tr[:cut], gid_tr[:cut]
    X_dv, y_dv, p1_dv, gid_dv = load_bin(args.dev_prefix, args.num_feats)
    tr_starts, _, tr_lens = build_group_index(gid_tr)
    dv_starts, _, dv_lens = build_group_index(gid_dv)
    print(
        f"Loaded Core2 ({args.loss}, F={args.num_feats}) train={X_tr.shape} ({len(tr_starts):,} groups) | "
        f"dev={X_dv.shape} ({len(dv_starts):,} groups)",
        flush=True,
    )

    y_tr_f32 = y_tr.astype(np.float32)
    dv_pos_mask = y_dv == 1

    dtrain = lgb.Dataset(X_tr, label=y_tr, free_raw_data=True)
    dval = lgb.Dataset(X_dv, label=y_dv, reference=dtrain, free_raw_data=False)

    params = {
        "learning_rate": args.lr,
        "num_leaves": args.num_leaves,
        "max_depth": args.max_depth,
        "min_data_in_leaf": args.min_data,
        "feature_fraction": args.ff,
        "bagging_fraction": args.bf,
        "bagging_freq": 1,
        "lambda_l2": args.l2,
        "max_bin": 128,
        "subsample_for_bin": 300000,
        "force_col_wise": True,
        "verbose": -1,
        "num_threads": args.threads,
        "seed": 20261123,
    }
    feval_fn = make_group_softmax_eval(dv_starts, dv_lens, dv_pos_mask)
    if args.loss == "softmax":
        params["objective"] = make_group_softmax_obj(tr_starts, tr_lens, y_tr_f32)
        params["metric"] = "None"
    else:
        params["objective"] = "binary"
        params["metric"] = "binary_logloss"

    booster = lgb.train(
        params,
        dtrain,
        num_boost_round=args.rounds,
        valid_sets=[dval],
        feval=feval_fn,
        callbacks=[
            lgb.early_stopping(stopping_rounds=30, verbose=True),
            lgb.log_evaluation(period=25),
        ],
    )
    best_iter = booster.best_iteration if booster.best_iteration and booster.best_iteration > 0 else args.rounds
    logits_dv = booster.predict(X_dv, num_iteration=best_iter, raw_score=True)
    is_p1_dv = (X_dv[:, 0] == 1).astype(np.uint8)
    eval_group_ranking(logits_dv, y_dv, is_p1_dv, gid_dv, args.scale)

    imp = booster.feature_importance(importance_type="gain")
    top_f = np.argsort(-imp)[:18]
    print("Top 18 Core2 features by gain:", [(int(f), round(float(imp[f]), 1)) for f in top_f], flush=True)

    blob_v2 = export_binary_as_2class(booster, best_iter, scale=args.scale)
    blob_v5, ok_v5 = convert_tree_v2_to_v5(blob_v2)
    assert ok_v5, "Core2 blob failed to pack into v5_4B!"
    verify_v5_parity(blob_v5, X_dv, logits_dv, args.scale)
    print(
        f"Exported Core2 {args.loss}->2class blob (best_iter={best_iter}): "
        f"v2={len(blob_v2)/1e6:.2f} MB | v5_4B={len(blob_v5)/1e6:.2f} MB in {time.time()-t0:.1f}s",
        flush=True,
    )
    os.makedirs(os.path.dirname(os.path.abspath(args.out_blob)), exist_ok=True)
    with open(args.out_blob, "wb") as f:
        f.write(blob_v5)
    print(f"Saved Core2 blob to {args.out_blob}", flush=True)


if __name__ == "__main__":
    main()
