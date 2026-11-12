"""Context-dependent HMM training (HTS-style, no HTK) -> RHVoice voice.data

steps (each resumable):
  stats   : per (phone instance, state) sufficient statistics from the alignment
  trees   : MDL decision-tree clustering per stream/state + duration
  write   : write voice.data (htsvoice 1.0)
"""
import os, sys, json, pickle, struct, time
import numpy as np
import scipy.sparse as sp

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from labels import LabelSet, read_lab, build_questions, PHONE_FIELDS

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
W = os.path.join(ROOT, "work")
TR = os.path.join(W, os.environ.get("RHV_TRAIN_DIR", "train"))
LANG = os.path.join(ROOT, "lang", "Arabic")
NS = 5
D_MGC, D_BAP = int(os.environ.get("RHV_MGC_DIM", "31")), 7
MGC_DIR = os.environ.get("RHV_MGC_DIR", "feat")
WINDOWS = [np.array([1.0]), np.array([-0.5, 0.0, 0.5]), np.array([1.0, -2.0, 1.0])]
VOICELESS = {"s", "S", "sh", "f", "th", "x", "H", "h", "t", "T", "k", "q", "Q", "p", "ch"}
# ه and the hamza are often voiced (breathy or creaky) between vowels; forcing them voiceless
# made them a burst of noise, heard as a dry cough. RHV_VOICED_OK="h,Q" leaves them to the pitch tracker.
VOICELESS -= set(x for x in os.environ.get("RHV_VOICED_OK", "").split(",") if x)
STREAMS = {"mgc": 3 * D_MGC, "lf0": 3, "bap": 3 * D_BAP}


def with_deltas(x):
    xp = np.pad(x, ((1, 1), (0, 0)), mode="edge")
    d1 = 0.5 * (xp[2:] - xp[:-2])
    d2 = xp[2:] - 2 * x + xp[:-2]
    return np.hstack([x, d1, d2])


def lf0_windows(lf0):
    """-> (T x 3 values, T x 3 defined mask)"""
    v = lf0 > -1e9
    T = len(lf0)
    vals = np.zeros((T, 3)); ok = np.zeros((T, 3), bool)
    vals[:, 0] = np.where(v, lf0, 0); ok[:, 0] = v
    if T >= 3:
        both = v[:-2] & v[1:-1] & v[2:]
        vals[1:-1, 1] = np.where(both, 0.5 * (lf0[2:] - lf0[:-2]), 0)
        vals[1:-1, 2] = np.where(both, lf0[2:] - 2 * lf0[1:-1] + lf0[:-2], 0)
        ok[1:-1, 1] = both; ok[1:-1, 2] = both
    return vals, ok


# ---------------------------------------------------------------- stats
def cmd_stats():
    """One pass over the corpus: feature codes per phone + per-state stats."""
    os.makedirs(TR, exist_ok=True)
    L = LabelSet(LANG)
    names = [f[0] for f in L.fields]
    ids = [l.strip() for l in open(os.path.join(W, "ids.txt")) if l.strip()]
    rows, durs = [], []
    offsets = {}
    acc = {s: {k: [] for k in ("mgc", "bap", "lf0")} for s in range(NS)}
    t0 = time.time()
    for i, uid in enumerate(ids, 1):
        fl = os.path.join(W, "full", uid + ".lab")
        al = os.path.join(W, "align", uid + ".npy")
        if not (os.path.exists(fl) and os.path.exists(al)):
            continue
        labs = read_lab(fl)
        segs = np.load(al)  # (unit, state, start, end)
        nunits = segs[:, 0].max() + 1
        if nunits != len(labs):
            print("unit mismatch", uid, nunits, len(labs)); continue
        bap = np.fromfile(os.path.join(W, "feat", "bap", uid + ".bap"), np.float32).reshape(-1, D_BAP).astype(np.float64)
        if os.environ.get("RHV_STATS_NO_MGC"):     # pitch-only rebuild: spectrum statistics are not needed
            mgc = np.zeros((len(bap), D_MGC))
        else:
            mgc = np.fromfile(os.path.join(W, MGC_DIR, "mgc", uid + ".mgc"), np.float32).reshape(-1, D_MGC).astype(np.float64)
        lf0 = np.fromfile(os.path.join(W, "feat", "lf0", uid + ".lf0"), np.float32).astype(np.float64)
        T = min(len(mgc), len(bap), len(lf0))
        # voiced only if harvest AND D4C agree (harvest over-extends voicing into fricatives)
        lf0 = np.where(bap[:len(lf0), 0] < -10.0, lf0[:len(bap)], -1e10)
        if os.environ.get("RHV_LF0_CLEAN"):
            # Pitch-tracking slips (octave jumps, creak at nasals and at the edges of consonants)
            # pulled some models far off the contour, heard as a shake. A voiced frame more than
            # 5 semitones from the local median of the contour (125 ms) is set to that median.
            from scipy.ndimage import median_filter
            vi = np.flatnonzero(lf0 > -1e9)
            if len(vi) >= 10:
                med = median_filter(lf0[vi], size=25, mode="nearest")
                off = np.abs(lf0[vi] - med) > 5 * np.log(2) / 12
                lf0[vi[off]] = med[off]
        M = with_deltas(mgc[:T]); B = with_deltas(bap[:T]); Fv, Fok = lf0_windows(lf0[:T])
        dur = np.zeros((nunits, NS))
        st = {}
        for u, j, s, e in segs:
            e = min(e, T)
            dur[u, j] = e - s
            st[(u, j)] = (s, e)
        offsets[uid] = (len(rows), int(nunits))
        # voiceless consonants: middle states (1-3) are unvoiced, whatever the pitch tracker said
        ni = names.index("name")
        for u in range(nunits):
            if L.parse(labs[u][2])[ni] in VOICELESS:
                for j in (1, 2, 3):
                    s0, e0 = st.get((u, j), (0, 0))
                    Fok[s0:e0] = False
        for u in range(nunits):
            rows.append(L.parse(labs[u][2]))
            durs.append(dur[u])
            for j in range(NS):
                s, e = st.get((u, j), (0, 0))
                m, b = M[s:e], B[s:e]
                acc[j]["mgc"].append(np.concatenate([[e - s], m.sum(0), (m ** 2).sum(0)]).astype(np.float32))
                acc[j]["bap"].append(np.concatenate([[e - s], b.sum(0), (b ** 2).sum(0)]).astype(np.float32))
                fv, fo = Fv[s:e], Fok[s:e]
                acc[j]["lf0"].append(np.concatenate([[e - s], fo.sum(0), (fv * fo).sum(0), (fv ** 2 * fo).sum(0)]).astype(np.float32))
        if i % 500 == 0:
            print(i, len(rows), f"{time.time() - t0:.0f}s", flush=True)
    rows = np.array(rows, dtype=object)
    # integer-code every field
    codes, vocab = np.zeros(rows.shape, np.int32), {}
    for k, n in enumerate(names):
        vals = sorted(set(rows[:, k]))
        vocab[n] = vals
        idx = {v: c for c, v in enumerate(vals)}
        codes[:, k] = [idx[v] for v in rows[:, k]]
    np.save(os.path.join(TR, "codes.npy"), codes)
    json.dump(offsets, open(os.path.join(TR, "offsets.json"), "w"))
    np.save(os.path.join(TR, "durs.npy"), np.array(durs, np.float32))
    json.dump(vocab, open(os.path.join(TR, "vocab.json"), "w"), ensure_ascii=False)
    for j in range(NS):
        for k in (("lf0",) if os.environ.get("RHV_STATS_NO_MGC") else ("mgc", "bap", "lf0")):
            np.save(os.path.join(TR, f"st_{k}_{j}.npy"), np.stack(acc[j][k]))
    print("stats:", len(rows), "phones", flush=True)


# ---------------------------------------------------------------- likelihoods
VFLOOR = 0.01


def gauss_ll(n, s1, s2, gvar):
    """log-likelihood (up to const) of ML diagonal Gaussian; arrays (..., D)"""
    n = np.maximum(n, 1e-8)[..., None]
    mu = s1 / n
    var = np.maximum(s2 / n - mu ** 2, VFLOOR * gvar)
    return -0.5 * n[..., 0] * (np.log(var).sum(-1) + var.shape[-1] * (1 + np.log(2 * np.pi)))


class Stream:
    """stats matrix S (items x K) -> node log-likelihood from summed stats."""

    def __init__(self, kind, S):
        self.kind, self.S = kind, S.astype(np.float64)
        tot = self.S.sum(0)
        if kind in ("mgc", "bap", "dur"):
            d = (S.shape[1] - 1) // 2
            self.d = d
            n = tot[0]
            self.gvar = tot[1 + d:] / n - (tot[1:1 + d] / n) ** 2
            self.nparam = 2 * d
        else:  # lf0 MSD: [T, n_k(3), s1(3), s2(3)]
            self.d = 3
            n = tot[1:4]
            self.gvar = tot[7:10] / n - (tot[4:7] / n) ** 2
            self.nparam = 2 * 3 + 1

    def occ(self, s):
        return s[..., 0]

    def ll(self, s):
        if self.kind == "lf0":
            T, nk, s1, s2 = s[..., 0], s[..., 1:4], s[..., 4:7], s[..., 7:10]
            nkf = np.maximum(nk, 1e-8)
            mu = s1 / nkf
            var = np.maximum(s2 / nkf - mu ** 2, VFLOOR * self.gvar)
            g = (-0.5 * nk * (np.log(var) + 1 + np.log(2 * np.pi))).sum(-1)
            nv = nk[..., 0]; nu = np.maximum(T - nv, 0)
            w = np.clip(nv / np.maximum(T, 1e-8), 1e-6, 1 - 1e-6)
            return g + nv * np.log(w) + nu * np.log(1 - w)
        d = self.d
        return gauss_ll(s[..., 0], s[..., 1:1 + d], s[..., 1 + d:], self.gvar)

    def pdf(self, s):
        """-> float32 vector: means, variances (, msd)"""
        if self.kind == "lf0":
            T, nk, s1, s2 = s[0], s[1:4], s[4:7], s[7:10]
            nkf = np.maximum(nk, 1e-8)
            mu = s1 / nkf
            var = np.maximum(s2 / nkf - mu ** 2, VFLOOR * self.gvar)
            w = float(np.clip(nk[0] / max(T, 1e-8), 0.0, 1.0))
            return np.concatenate([mu, var, [w]]).astype(np.float32)
        d = self.d
        n = max(s[0], 1e-8)
        mu = s[1:1 + d] / n
        var = np.maximum(s[1 + d:] / n - mu ** 2, VFLOOR * self.gvar)
        return np.concatenate([mu, var]).astype(np.float32)


# ---------------------------------------------------------------- tree growing
def question_matrices(codes, vocab, names, qs):
    """per field: list of question ids and sparse (Q_f x V_f) membership"""
    per = {}
    for qi, q in enumerate(qs):
        per.setdefault(q["field"], []).append(qi)
    mats = {}
    for f, qids in per.items():
        V = len(vocab[f])
        idx = {v: c for c, v in enumerate(vocab[f])}
        M = np.zeros((len(qids), V), np.float64)
        for r, qi in enumerate(qids):
            for v in qs[qi]["values"]:
                M[r, idx[v]] = 1
        mats[f] = (np.array(qids), M, names.index(f))
    return mats


def grow_tree(stream, codes, mats, qs, mdl=1.0, min_occ=10.0, log=None):
    S = stream.S
    n_root = stream.occ(S.sum(0)) if stream.kind != "lf0" else S[:, 0].sum()
    thresh = mdl * 0.5 * stream.nparam * np.log(max(n_root, 2))
    # node: (item index array) ; tree as list of nodes: [q, yes_child, no_child] or leaf id
    nodes = [{"items": np.arange(len(S))}]
    queue = [0]
    while queue:
        ni = queue.pop()
        items = nodes[ni]["items"]
        Si = S[items]
        tot = Si.sum(0)
        base = stream.ll(tot)
        best = (thresh, None, None)
        for f, (qids, M, col) in mats.items():
            c = codes[items, col]
            V = M.shape[1]
            G = sp.csr_matrix((np.ones(len(items)), (c, np.arange(len(items)))), shape=(V, len(items))) @ Si
            Y = M @ G                       # Q_f x K
            N = tot[None, :] - Y
            oy = Y[:, 0] if stream.kind != "lf0" else Y[:, 0]
            on = N[:, 0]
            gain = stream.ll(Y) + stream.ll(N) - base
            gain[(oy < min_occ) | (on < min_occ)] = -np.inf
            k = int(np.argmax(gain))
            if gain[k] > best[0]:
                best = (gain[k], qids[k], col)
        if best[1] is None:
            nodes[ni]["leaf"] = True
            nodes[ni]["members"] = items
            nodes[ni]["stats"] = tot
            del nodes[ni]["items"]
            continue
        q = qs[best[1]]
        vals = set(q["values"])
        vocab_f = mats[q["field"]]
        # recompute membership for items
        member = np.isin(codes[items, best[2]], [VOC[q["field"]][v] for v in q["values"]])
        yes, no = items[member], items[~member]
        nodes[ni].update({"q": best[1], "yes": len(nodes), "no": len(nodes) + 1})
        del nodes[ni]["items"]
        nodes.append({"items": yes}); nodes.append({"items": no})
        queue += [len(nodes) - 2, len(nodes) - 1]
        if log and len(nodes) % 200 == 1:
            log(f"  nodes={len(nodes)} queue={len(queue)}")
    return nodes


VOC = {}


def cmd_trees(mdl=1.0):
    codes = np.load(os.path.join(TR, "codes.npy"))
    vocab = json.load(open(os.path.join(TR, "vocab.json")))
    for f, vals in vocab.items():
        VOC[f] = {v: c for c, v in enumerate(vals)}
    L = LabelSet(LANG)
    names = [f[0] for f in L.fields]
    qs = build_questions(L.fields, vocab, L.phfeats)
    json.dump(qs, open(os.path.join(TR, "questions.json"), "w"), ensure_ascii=False)
    mats = question_matrices(codes, vocab, names, qs)
    print(len(qs), "questions", flush=True)
    jobs = [("dur", None)] + [(k, j) for k in ("mgc", "lf0", "bap") for j in range(NS)]
    # held-out utterances (for objective tests): their statistics are zeroed, so they train nothing
    held = np.zeros(len(codes), bool)
    hf = os.environ.get("RHV_HOLDOUT")
    if hf:
        offsets = json.load(open(os.path.join(TR, "offsets.json")))
        for u in open(hf).read().split():
            if u in offsets:
                o, n = offsets[u]; held[o:o + n] = True
        print("held out", int(held.sum()), "phones", flush=True)
    only = os.environ.get("RHV_ONLY_STREAMS")
    if only:
        jobs = [j for j in jobs if j[0] in only.split(",")]
    for kind, j in jobs:
        out = os.path.join(TR, f"tree_{kind}_{j}.pkl")
        if os.path.exists(out):
            continue
        t0 = time.time()
        if kind == "dur":
            d = np.load(os.path.join(TR, "durs.npy")).astype(np.float64)
            cap = float(os.environ.get("RHV_DUR_FINAL_CAP", "0"))
            if cap > 0:
                # Some corpora (synthetic ones especially) stretch the last syllables of each
                # utterance far beyond normal, and the model then drags out sentence ends.
                # The last two spoken phones of each utterance are limited to `cap` times that
                # phone's usual (median) length elsewhere.
                vocab = json.load(open(os.path.join(TR, "vocab.json"), encoding="utf-8"))
                ni = [f[0] for f in LabelSet(LANG).fields].index("name")
                names_ = np.array(vocab["name"], dtype=object)[codes[:, ni]]
                offsets = json.load(open(os.path.join(TR, "offsets.json")))
                final = np.zeros(len(d), bool)
                for o, n in offsets.values():
                    sp = [k for k in range(o, o + n) if names_[k] not in ("pau", "sil", "x")]
                    final[sp[-int(os.environ.get("RHV_DUR_FINAL_N", "2")):]] = True
                tot = d.sum(1)
                med = {}
                for ph in set(names_):
                    m = (names_ == ph) & ~final
                    med[ph] = np.median(tot[m]) if m.sum() >= 5 else np.median(tot[~final])
                lim = np.array([cap * med[ph] for ph in names_])
                over = final & (tot > lim)
                d[over] *= (lim[over] / tot[over])[:, None]
                print("final-lengthening cap: %d phones shortened" % over.sum(), flush=True)
            S = np.hstack([np.ones((len(d), 1)), d, d ** 2])
            S[held] = 0
            st = Stream("dur", S)
            pq = [i for i, q in enumerate(qs) if q["field"] == "name" and q["values"] == ["pau"]]
            if os.environ.get("RHV_DUR_SPLIT_PAU") and pq:
                # Pauses and speech sounds get separate duration trees, so a rare context at the
                # end of a sentence can never share a pause's length (a 0.5 s "consonant").
                col = mats[qs[pq[0]]["field"]][2]
                is_p = np.isin(codes[:, col], [VOC["name"][v] for v in qs[pq[0]]["values"]])
                nodes = [{"q": pq[0]}]
                for side, mask in (("yes", is_p), ("no", ~is_p)):
                    S2 = S.copy(); S2[~mask] = 0
                    sub = grow_tree(Stream("dur", S2), codes, mats, qs, mdl, min_occ=5.0)
                    base = len(nodes)
                    for n in sub:
                        if "yes" in n:
                            n["yes"] += base; n["no"] += base
                        if n.get("leaf"):
                            n["members"] = n["members"][mask[n["members"]]]
                    nodes[0][side] = base
                    nodes += sub
            else:
                nodes = grow_tree(st, codes, mats, qs, mdl, min_occ=5.0)
        else:
            S = np.load(os.path.join(TR, f"st_{kind}_{j}.npy"))
            S[held] = 0
            st = Stream(kind, S)
            mo = float(os.environ.get("RHV_MIN_OCC_MGC", "10")) if kind == "mgc" else \
                float(os.environ.get("RHV_MIN_OCC_LF0", "10")) if kind == "lf0" else 10.0
            nodes = grow_tree(st, codes, mats, qs, mdl, min_occ=mo)
        leaves = [n for n in nodes if n.get("leaf")]
        leaf_of = np.zeros(len(S), np.int32)
        for li, n in enumerate(leaves):
            n["pdf"] = st.pdf(n.pop("stats"))
            leaf_of[n.pop("members")] = li
        np.save(os.path.join(TR, f"leafof_{kind}_{j}.npy"), leaf_of)
        pickle.dump(nodes, open(out, "wb"))
        print(f"tree {kind} {j}: {len(leaves)} leaves, {time.time() - t0:.0f}s", flush=True)


# ---------------------------------------------------------------- htsvoice writer
def tree_text(nodes, qs, leafname, state):
    leaf_ids, k = {}, 0
    for i, n in enumerate(nodes):
        if n.get("leaf"):
            k += 1; leaf_ids[i] = k
    inner = [i for i, n in enumerate(nodes) if not n.get("leaf")]
    num = {i: -r for r, i in enumerate(inner)}  # 0, -1, -2 ...

    def ref(i):
        return f'"{leafname}_s{state}_{leaf_ids[i]}"' if i in leaf_ids else str(num[i])

    lines = [f"{{*}}[{state}]"]
    if not inner:
        lines.append(f'"{leafname}_s{state}_1"')
    else:
        lines.append("{")
        for i in inner:
            n = nodes[i]
            # HTS: "no" branch first, then "yes"
            lines.append(f"  {num[i]} Q{n['q']} {ref(n['no'])} {ref(n['yes'])}")
        lines.append("}")
    pdfs = [nodes[i]["pdf"] for i in sorted(leaf_ids, key=leaf_ids.get)]
    return "\n".join(lines) + "\n", pdfs


def qs_text(qs, used):
    out = []
    for qi in sorted(used):
        q = qs[qi]
        pats = ",".join('"' + p + '"' for p in q["patterns"])
        out.append(f"QS Q{qi} {{ {pats} }}")
    return "\n".join(out) + "\n"


def cmd_write(out_path):
    qs = json.load(open(os.path.join(TR, "questions.json")))
    trees = {}
    for kind in ("mgc", "lf0", "bap"):
        trees[kind] = [pickle.load(open(os.path.join(TR, f"tree_{kind}_{j}.pkl"), "rb")) for j in range(NS)]
    dur = pickle.load(open(os.path.join(TR, "tree_dur_None.pkl"), "rb"))
    pct = float(os.environ.get("RHV_DUR_PCT", "0"))
    if pct > 0:
        # A duration model whose speech sound lasts longer than that sound ever does in `pct`%
        # of the recordings (a rare context, e.g. ه before و in يَهْوَاهُ) is shortened to that length.
        d = np.load(os.path.join(TR, "durs.npy")).astype(np.float64).sum(1)
        codes = np.load(os.path.join(TR, "codes.npy"))
        vocab = json.load(open(os.path.join(TR, "vocab.json"), encoding="utf-8"))
        flds = [f[0] for f in LabelSet(LANG).fields]
        ni, nn = flds.index("name"), flds.index("next_name")
        names_ = np.array(vocab["name"], dtype=object)[codes[:, ni]]
        nexts_ = np.array(vocab["next_name"], dtype=object)[codes[:, nn]]
        keys = np.array([a + ("|end" if b in ("pau", "sil", "x") else "") for a, b in zip(names_, nexts_)], dtype=object)
        leafof = np.load(os.path.join(TR, "leafof_dur_None.npy"))
        lim = {k: min(np.percentile(d[keys == k], pct), 2.0 * np.median(d[keys == k]))
               for k in set(keys) if (keys == k).sum() >= 20}
        leaves = [n for n in dur if n.get("leaf")]
        nfix = 0
        for li, n in enumerate(leaves):
            mem = keys[leafof == li]
            if len(mem) == 0:
                continue
            vals, cnt = np.unique(mem, return_counts=True)
            k = vals[cnt.argmax()]
            if k.split("|")[0] in ("pau", "sil", "x") or k not in lim:
                continue
            p = n["pdf"].astype(np.float64).copy()
            tot = p[:NS].sum()
            if tot > lim[k]:
                p[:NS] *= lim[k] / tot
                n["pdf"] = p.astype(np.float32); nfix += 1
        print("duration cap: %d models shortened" % nfix, flush=True)
        rel = [x for x in os.environ.get("RHV_STOP_RELEASE", "").split(",") if x]
        if rel:
            # Stops whose release the model smeared into a slow noisy rise (heard as a cough):
            # the release states (2-4) are shortened so the burst is crisp again.
            nrel = 0
            for li, n in enumerate(leaves):
                mem = names_[leafof == li]
                if len(mem) == 0:
                    continue
                vals, cnt = np.unique(mem, return_counts=True)
                if vals[cnt.argmax()] in rel:
                    p = n["pdf"].astype(np.float64).copy()
                    p[2:NS] = np.maximum(p[2:NS] * 0.4, 1.0)
                    n["pdf"] = p.astype(np.float32); nrel += 1
            print("stop release: %d models shortened" % nrel, flush=True)
    used = set()
    for t in [dur] + [x for v in trees.values() for x in v]:
        used |= {n["q"] for n in t if not n.get("leaf")}
    qtxt = qs_text(qs, used)

    blocks = []  # (key, bytes)
    # duration
    ttxt, pdfs = tree_text(dur, qs, "dur", 2)
    blocks.append(("DURATION_PDF", struct.pack("<I", len(pdfs)) + b"".join(p.tobytes() for p in pdfs)))
    blocks.append(("DURATION_TREE", (qtxt + ttxt).encode()))
    win_bytes = [b"1 1.0\n", b"3 -0.5 0.0 0.5\n", b"3 1.0 -2.0 1.0\n"]
    for k in ("MCP", "LF0", "BAP"):
        for wb in win_bytes:
            blocks.append((f"STREAM_WIN[{k}]", wb))
    # optional lift of voiceless fricatives (HTS averaging dulls them): RHV_FRIC_BOOST_DB
    boost = float(os.environ.get("RHV_FRIC_BOOST_DB", "0"))
    if boost:
        L = LabelSet(LANG); ni = [f[0] for f in L.fields].index("name")
        codes = np.load(os.path.join(TR, "codes.npy")); vocab = json.load(open(os.path.join(TR, "vocab.json")))
        ph = np.array(vocab["name"])[codes[:, ni]]
        fric = np.isin(ph, ["s", "S", "sh", "f", "th", "x", "H", "h"])
        for j in (1, 2, 3):
            lo = np.load(os.path.join(TR, f"leafof_mgc_{j}.npy"))
            leaves = [n for n in trees["mgc"][j] if n.get("leaf")]
            share = np.bincount(lo, weights=fric, minlength=len(leaves)) / np.maximum(np.bincount(lo, minlength=len(leaves)), 1)
            for li, n in enumerate(leaves):
                if share[li] > 0.8:
                    n["pdf"] = n["pdf"].copy(); n["pdf"][0] += boost / 8.686
    # Variance scaling: HMM averaging flattens the spectrum (muffled sound). Every mean is moved
    # away from the voice's average spectrum by a per-coefficient factor, which restores the
    # natural variance. It is a purely linear change of the output, so unlike GV it cannot click.
    # RHV_VSCALE = .npy of variance ratios synth/natural (from eval_voice.py); RHV_VSCALE_W = strength 0..1
    vs = os.environ.get("RHV_VSCALE")
    if vs:
        ratio = np.load(vs).astype(np.float64)
        wgt = float(os.environ.get("RHV_VSCALE_W", "1"))
        alpha = np.ones(D_MGC)
        k = min(len(ratio), D_MGC)
        alpha[:k] = np.clip(ratio[:k], 0.2, 1.0) ** (-0.5 * wgt)
        alpha[k:] = alpha[k - 1]
        alpha[0] = 1.0                                   # loudness contour is left alone
        tot = np.zeros(1 + 2 * 3 * D_MGC)
        for j in range(NS):
            tot += np.load(os.path.join(TR, f"st_mgc_{j}.npy"), mmap_mode="r").sum(0, dtype=np.float64)
        gmean = tot[1:1 + D_MGC] / tot[0]
        a3 = np.tile(alpha, 3)
        for j in range(NS):
            for n in trees["mgc"][j]:
                if n.get("leaf"):
                    p = n["pdf"].astype(np.float64).copy(); dd = 3 * D_MGC
                    p[:D_MGC] = gmean + alpha * (p[:D_MGC] - gmean)
                    p[D_MGC:dd] *= a3[D_MGC:]
                    p[dd:] *= a3 ** 2
                    n["pdf"] = p.astype(np.float32)
    # overall level of the voice (RHVoice clamps gain to 0.5..2): RHV_LEVEL_DB
    lvl = float(os.environ.get("RHV_LEVEL_DB", "0"))
    if lvl:
        for j in range(NS):
            for n in trees["mgc"][j]:
                if n.get("leaf"):
                    p = n["pdf"].copy(); p[0] += lvl / 8.686; n["pdf"] = p
    # less noise in voiced sounds (vocoder noise is not pitch-synchronous): RHV_BAP_OFFSET_DB
    bofs = float(os.environ.get("RHV_BAP_OFFSET_DB", "0"))
    if bofs:
        for j in range(NS):
            for n in trees["bap"][j]:
                if n.get("leaf"):
                    p = n["pdf"].copy(); p[1:D_BAP] = np.minimum(p[1:D_BAP] + bofs, 0.0); n["pdf"] = p
    lvs = float(os.environ.get("RHV_LF0_VAR_SCALE", "1"))
    if lvs != 1.0:
        # Wider pitch variances on the static term let the smoothness constraints (deltas) win:
        # the contour no longer jumps between neighbouring states (heard as a shake).
        for j in range(NS):
            for n in trees["lf0"][j]:
                if n.get("leaf"):
                    p = n["pdf"].astype(np.float64).copy(); p[3] *= lvs; n["pdf"] = p.astype(np.float32)
    for k, kind in (("MCP", "mgc"), ("LF0", "lf0"), ("BAP", "bap")):
        allp, ttx = [], qtxt
        counts = []
        for j in range(NS):
            t, p = tree_text(trees[kind][j], qs, kind, j + 2)
            ttx += t; counts.append(len(p)); allp += p
        blocks.append((f"STREAM_PDF[{k}]", struct.pack(f"<{NS}I", *counts) + b"".join(p.tobytes() for p in allp)))
        blocks.append((f"STREAM_TREE[{k}]", ttx.encode()))
    gvf = os.path.join(TR, "gv.npz")
    use_gv = os.path.exists(gvf)
    if use_gv:
        gv = np.load(gvf)
        for k, kind in (("MCP", "mgc"),):  # pitch GV made the voice shaky
            g = gv[kind].copy(); d = len(g) // 2
            w = float(os.environ.get("RHV_GV_WEIGHT", "1"))   # partial GV strength (variance / w)
            g[d:] = g[d:] / max(w, 1e-3)
            if os.environ.get("RHV_GV_NO_ENERGY"):
                g[d] = 1e10                                    # c0 (loudness) left to the model
            blocks.append((f"GV_PDF[{k}]", struct.pack("<I", 1) + g.astype(np.float32).tobytes()))
            blocks.append((f"GV_TREE[{k}]", b'{*}[2]\n"gv_s2_1"\n'))
    # positions
    pos, offs = 0, {}
    for key, b in blocks:
        offs.setdefault(key, []).append((pos, pos + len(b) - 1)); pos += len(b)
    pl = []
    for key in ["DURATION_PDF", "DURATION_TREE", "STREAM_WIN[MCP]", "STREAM_WIN[LF0]", "STREAM_WIN[BAP]",
                "STREAM_PDF[MCP]", "STREAM_PDF[LF0]", "STREAM_PDF[BAP]", "STREAM_TREE[MCP]", "STREAM_TREE[LF0]", "STREAM_TREE[BAP]"]:
        pl.append(f"{key}:" + ",".join(f"{a}-{b}" for a, b in offs[key]))
    if use_gv:
        for k in ("MCP",):
            for key in (f"GV_PDF[{k}]", f"GV_TREE[{k}]"):
                pl.append(f"{key}:" + ",".join(f"{a}-{b}" for a, b in offs[key]))
    g = "1" if use_gv else "0"
    header = f"""[GLOBAL]
HTS_VOICE_VERSION:1.0
SAMPLING_FREQUENCY:24000
FRAME_PERIOD:120
NUM_STATES:{NS}
NUM_STREAMS:3
STREAM_TYPE:MCP,LF0,BAP
FULLCONTEXT_FORMAT:HTS_TTS_ARA{"_WORLD" if os.environ.get("RHV_VOCODER") == "world" else ""}
FULLCONTEXT_VERSION:1.0
GV_OFF_CONTEXT:"*-pau+*"
COMMENT:Arabic voice trained on ClArTTS for RHVoice
[STREAM]
VECTOR_LENGTH[MCP]:{D_MGC}
VECTOR_LENGTH[LF0]:1
VECTOR_LENGTH[BAP]:{D_BAP}
IS_MSD[MCP]:0
IS_MSD[LF0]:1
IS_MSD[BAP]:0
NUM_WINDOWS[MCP]:3
NUM_WINDOWS[LF0]:3
NUM_WINDOWS[BAP]:3
USE_GV[MCP]:{g}
USE_GV[LF0]:0
USE_GV[BAP]:0
OPTION[MCP]:ALPHA=0.466
OPTION[LF0]:
OPTION[BAP]:
[POSITION]
""" + "\n".join(pl) + "\n[DATA]\n"
    with open(out_path, "wb") as f:
        f.write(header.encode())
        for _, b in blocks:
            f.write(b)
    print("wrote", out_path, os.path.getsize(out_path), "bytes")


def cmd_gv():
    """Global variance: per-utterance variance of static features over speech
    frames (pauses excluded), modelled by one Gaussian per stream."""
    ids = [l.strip() for l in open(os.path.join(W, "ids.txt")) if l.strip()]
    acc = {"mgc": [], "lf0": [], "bap": []}
    for uid in ids:
        al = os.path.join(W, "align", uid + ".npy"); ph = os.path.join(W, "align", uid + ".phones")
        if not (os.path.exists(al) and os.path.exists(ph)):
            continue
        phones = open(ph).read().split()
        segs = np.load(al)
        bap = np.fromfile(os.path.join(W, "feat", "bap", uid + ".bap"), np.float32).reshape(-1, D_BAP).astype(np.float64)
        if os.environ.get("RHV_STATS_NO_MGC"):     # pitch-only rebuild: spectrum statistics are not needed
            mgc = np.zeros((len(bap), D_MGC))
        else:
            mgc = np.fromfile(os.path.join(W, MGC_DIR, "mgc", uid + ".mgc"), np.float32).reshape(-1, D_MGC).astype(np.float64)
        lf0 = np.fromfile(os.path.join(W, "feat", "lf0", uid + ".lf0"), np.float32).astype(np.float64)
        T = min(len(mgc), len(bap), len(lf0))
        speech = np.zeros(T, bool)
        for u, j, st, e in segs:
            if u < len(phones) and phones[u] != "pau":
                speech[st:min(e, T)] = True
        lf0 = np.where(bap[:len(lf0), 0] < -10.0, lf0[:len(bap)], -1e10)[:T]
        if speech.sum() < 20:
            continue
        acc["mgc"].append(mgc[:T][speech].var(0)); acc["bap"].append(bap[:T][speech].var(0))
        v = speech & (lf0 > -1e9)
        if v.sum() > 10:
            acc["lf0"].append([lf0[v].var()])
    out = {}
    for k, a in acc.items():
        a = np.array(a)
        out[k] = np.concatenate([a.mean(0), np.maximum(a.var(0), 1e-12)]).astype(np.float32)
        print(k, len(a), "utts; gv mean[:3]", a.mean(0)[:3])
    np.savez(os.path.join(TR, "gv.npz"), **out)


def cmd_realign():
    """Viterbi re-segmentation of every phone into 5 states with the tied
    context-dependent mgc models (static+delta+delta2), keeping the phone
    sequence (incl. pauses). Rewrites work/align/*.npy."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from align import viterbi
    offsets = json.load(open(os.path.join(TR, "offsets.json")))
    pdfs, leafof = [], []
    for j in range(NS):
        nodes = pickle.load(open(os.path.join(TR, f"tree_mgc_{j}.pkl"), "rb"))
        P = np.stack([n["pdf"] for n in nodes if n.get("leaf")]).astype(np.float64)
        pdfs.append(P); leafof.append(np.load(os.path.join(TR, f"leafof_mgc_{j}.npy")))
    dd = 3 * D_MGC
    done = 0; tot_ll = 0.0; frames = 0
    for uid, (o, n) in offsets.items():
        mgc = np.fromfile(os.path.join(W, MGC_DIR, "mgc", uid + ".mgc"), np.float32).reshape(-1, D_MGC).astype(np.float64)
        x = with_deltas(mgc)
        S = n * NS
        mu = np.zeros((S, dd)); var = np.ones((S, dd))
        for u in range(n):
            for j in range(NS):
                p = pdfs[j][leafof[j][o + u]]
                mu[u * NS + j], var[u * NS + j] = p[:dd], p[dd:2 * dd]
        if len(x) < S:
            continue
        iv = 1.0 / var
        ll = -0.5 * ((x ** 2) @ iv.T - 2 * x @ (mu * iv).T + (mu ** 2 * iv).sum(1)) - 0.5 * np.log(var).sum(1)
        path, score = viterbi(ll, np.full(S, -1, np.int32))
        if score < -1e29:
            continue
        segs, st = [], 0
        for t in range(1, len(path) + 1):
            if t == len(path) or path[t] != path[st]:
                segs.append((path[st] // NS, path[st] % NS, st, t)); st = t
        np.save(os.path.join(W, "align", uid + ".npy"), np.array(segs, np.int32))
        done += 1; tot_ll += score; frames += len(x)
        if done % 1000 == 0:
            print(done, f"avg ll/frame {tot_ll / frames:.2f}", flush=True)
    print("realigned", done, f"avg ll/frame {tot_ll / max(frames, 1):.2f}", flush=True)


if __name__ == "__main__":
    c = sys.argv[1]
    if c == "stats":
        cmd_stats()
    elif c == "trees":
        cmd_trees(float(sys.argv[2]) if len(sys.argv) > 2 else 1.0)
    elif c == "gv":
        cmd_gv()
    elif c == "realign":
        cmd_realign()
    elif c == "write":
        cmd_write(sys.argv[2])
