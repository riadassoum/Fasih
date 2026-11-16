"""Monophone 5-state HMM forced alignment (Viterbi training), with optional
pauses between words. No HTK.

usage: align.py init|iter|final  [--pauses] [--workers N]
  init : flat start (uniform segmentation) -> models
  iter : one Viterbi re-estimation pass  (resumable, run repeatedly)
  final: write state alignments (work/align/*.npy) + mono labels (work/mono/*.lab)
"""
import os, sys, json, glob, pickle
import numpy as np
from multiprocessing import Pool
from numba import njit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from labels import LabelSet, read_lab

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
W = os.path.join(ROOT, "work")
NS = 5
D = 31
FRAME_100NS = 50000
MODELS = os.path.join(W, "align_models.pkl")


def utt_list():
    return [l.strip() for l in open(os.path.join(W, "ids.txt")) if l.strip()]


def load_phones(i):
    """phones + word-final flags for utterance index i (1-based lab file)."""
    L = load_phones.L
    labs = read_lab(os.path.join(W, "lab0", f"u_{i}.lab"))
    phones, wend = [], []
    ni, bi = L.names.index("name"), L.names.index("pos_in_word_bw")
    for _, _, lab in labs:
        g = L.parse(lab)
        phones.append(g[ni])
        wend.append(g[bi] == "1")
    return phones, wend


def deltas(x):
    xp = np.pad(x, ((1, 1), (0, 0)), mode="edge")
    return np.hstack([x, 0.5 * (xp[2:] - xp[:-2])]).astype(np.float32)


def load_obs(uid):
    m = np.fromfile(os.path.join(W, "feat", "mgc", uid + ".mgc"), dtype=np.float32).reshape(-1, D)
    return deltas(m)


@njit(cache=True)
def viterbi(ll, prev2):
    """ll: T x S log-likelihoods; prev2[s] = extra predecessor (or -1).
    Path must start at state 0 and end at state S-1. Returns state per frame."""
    T, S = ll.shape
    NEG = -1e30
    dp = np.full(S, NEG)
    bp = np.zeros((T, S), dtype=np.int32)
    dp[0] = ll[0, 0]
    for t in range(1, T):
        new = np.full(S, NEG)
        for s in range(S):
            best = dp[s]; arg = s
            if s > 0 and dp[s - 1] > best:
                best = dp[s - 1]; arg = s - 1
            p = prev2[s]
            if p >= 0 and dp[p] > best:
                best = dp[p]; arg = p
            if best > NEG:
                new[s] = best + ll[t, s]
                bp[t, s] = arg
        dp = new
    path = np.zeros(T, dtype=np.int32)
    s = S - 1
    if dp[s] <= NEG:
        return path, NEG
    score = dp[s]
    for t in range(T - 1, -1, -1):
        path[t] = s
        s = bp[t, s]
    return path, score


def build_graph(phones, wend, pauses):
    """-> list of (phone, state, optional_flag), prev2 array"""
    units = []  # (phone, optional)
    for k, (p, we) in enumerate(zip(phones, wend)):
        units.append((p, False))
        if pauses and we and k + 1 < len(phones) and phones[k + 1] != "pau" and p != "pau":
            units.append(("pau", True))
    states, prev2 = [], []
    for ui, (p, opt) in enumerate(units):
        for j in range(NS):
            states.append((p, j, ui))
            prev2.append(-1)
    # skip edge over optional pause units
    idx = 0
    starts = []
    for ui in range(len(units)):
        starts.append(idx); idx += NS
    for ui, (p, opt) in enumerate(units):
        if opt and ui + 1 < len(units):
            prev2[starts[ui + 1]] = starts[ui] - 1
    return units, states, np.array(prev2, dtype=np.int32)


def loglik(x, states, models):
    mu = np.stack([models[p][0][j] for p, j, _ in states])
    var = np.stack([models[p][1][j] for p, j, _ in states])
    iv = 1.0 / var
    const = -0.5 * np.log(var).sum(1)
    # (x-mu)^2/var = x^2 iv - 2 x mu iv + mu^2 iv
    q = (x ** 2) @ iv.T - 2 * x @ (mu * iv).T + (mu ** 2 * iv).sum(1)
    return (const - 0.5 * q).astype(np.float64)


def init_worker():
    load_phones.L = LabelSet(os.path.join(ROOT, "lang", os.environ.get("RHV_LANG", "Arabic")))
    load_phones.L.names = [f[0] for f in load_phones.L.fields]


def job_init(args):
    i, uid = args
    phones, wend = load_phones(i)
    x = load_obs(uid)
    T = len(x)
    S = len(phones) * NS
    if T < S:
        return {}
    b = np.linspace(0, T, S + 1).astype(int)
    acc = {}
    for s in range(S):
        p, j = phones[s // NS], s % NS
        seg = x[b[s]:b[s + 1]]
        a = acc.setdefault(p, [np.zeros(NS), np.zeros((NS, 2 * D)), np.zeros((NS, 2 * D))])
        a[0][j] += len(seg); a[1][j] += seg.sum(0); a[2][j] += (seg.astype(np.float64) ** 2).sum(0)
    return acc


def job_iter(args):
    i, uid, models, pauses, keep = args
    phones, wend = load_phones(i)
    x = load_obs(uid)
    units, states, prev2 = build_graph(phones, wend, pauses)
    if not states or len(x) < len(units) * 1:
        return {}, 0.0, None
    ll = loglik(x, states, models)
    path, score = viterbi(ll, prev2)
    if score < -1e29:
        return {}, 0.0, None
    acc = {}
    for t, s in enumerate(path):
        p, j, _ = states[s]
        a = acc.setdefault(p, [np.zeros(NS), np.zeros((NS, 2 * D)), np.zeros((NS, 2 * D))])
        a[0][j] += 1; a[1][j] += x[t]; a[2][j] += x[t].astype(np.float64) ** 2
    out = None
    if keep:
        # segments: list of (phone, unit_index, state, start, end) frames
        segs = []
        st = 0
        for t in range(1, len(path) + 1):
            if t == len(path) or path[t] != path[st]:
                p, j, ui = states[path[st]]
                segs.append((p, ui, j, st, t))
                st = t
        # renumber units consecutively (skipped optional pauses leave gaps)
        order = {}
        for p, ui, j, s0, e0 in segs:
            order.setdefault(ui, len(order))
        out = [(p, order[ui], j, s0, e0) for p, ui, j, s0, e0 in segs]
    return acc, score / len(x), out


def merge(accs):
    tot = {}
    for acc in accs:
        for p, a in acc.items():
            if p not in tot:
                tot[p] = [v.copy() for v in a]
            else:
                for k in range(3):
                    tot[p][k] += a[k]
    return tot


def to_models(tot, floor_frac=0.01):
    allc = sum(a[0].sum() for a in tot.values())
    gvar = sum(a[2].sum(0) for a in tot.values()) / allc - (sum(a[1].sum(0) for a in tot.values()) / allc) ** 2
    models = {}
    for p, (c, s1, s2) in tot.items():
        c = np.maximum(c, 1)[:, None]
        mu = s1 / c
        var = np.maximum(s2 / c - mu ** 2, floor_frac * gvar)
        models[p] = (mu.astype(np.float32), var.astype(np.float32))
    return models


def main():
    cmd = sys.argv[1]
    nw = int(sys.argv[sys.argv.index("--workers") + 1]) if "--workers" in sys.argv else 2
    pauses = "--pauses" in sys.argv
    ids = utt_list()
    ready = [(i + 1, u) for i, u in enumerate(ids) if os.path.exists(os.path.join(W, "feat", "bap", u + ".bap"))]
    print(f"{len(ready)} utterances with features", flush=True)
    with Pool(nw, initializer=init_worker) as pool:
        if cmd == "init":
            tot = merge(pool.imap_unordered(job_init, ready, chunksize=20))
            pickle.dump({"models": to_models(tot), "iter": 0}, open(MODELS, "wb"))
            print("init done:", sorted(tot), flush=True)
            return
        st = pickle.load(open(MODELS, "rb"))
        models = st["models"]
        keep = cmd == "final"
        args = [(i, u, models, pauses, keep) for i, u in ready]
        accs, scores, n_ok = [], [], 0
        if keep:
            os.makedirs(os.path.join(W, "align"), exist_ok=True)
            os.makedirs(os.path.join(W, "mono"), exist_ok=True)
        for (i, u), (acc, sc, segs) in zip(ready, pool.imap(job_iter, args, chunksize=10)):
            if not acc:
                continue
            accs.append(acc); scores.append(sc); n_ok += 1
            if keep and segs:
                np.save(os.path.join(W, "align", u + ".npy"),
                        np.array([(ui, j, s, e) for p, ui, j, s, e in segs], dtype=np.int32))
                with open(os.path.join(W, "align", u + ".phones"), "w") as f:
                    units = {}
                    for p, ui, j, s, e in segs:
                        units.setdefault(ui, [p, s, e])[2] = e
                    f.write(" ".join(units[k][0] for k in sorted(units)) + "\n")
                with open(os.path.join(W, "mono", u + ".lab"), "w") as f:
                    for k in sorted(units):
                        p, s, e = units[k]
                        f.write(f"{s * FRAME_100NS} {e * FRAME_100NS} {p}\n")
        if not keep:
            tot = merge(accs)
            st = {"models": to_models(tot), "iter": st["iter"] + 1}
            pickle.dump(st, open(MODELS, "wb"))
        print(f"{cmd} iter={st['iter']} ok={n_ok} avg_ll/frame={np.mean(scores):.3f}", flush=True)


if __name__ == "__main__":
    main()
