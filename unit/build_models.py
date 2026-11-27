"""Statistical guidance for a unit voice (HMM-guided unit selection, non-neural).

usage: build_models.py VOICE_DIR [PHONEMES_XML]
Reads VOICE_DIR/units.bin and VOICE_DIR/unit_labels.txt (written by build_units.py) and trains three
context-clustering regression trees, as HTS does, on the units themselves:
  duration  -> log length of a half-phone
  pitch     -> log F0 of a voiced half-phone
  spectrum  -> mean join spectrum of a half-phone (13 dims, normalised)
The trees use HTS-style questions: phone identity and phonetic classes of the five phones in context,
numeric positions (syllable, word, phrase, utterance), the half, and the sentence type.
Writes VOICE_DIR/models.bin (read by the engine to predict targets for every half-phone) and stores in
units.bin each unit's quality cost: how far the unit is from what its own context predicts (theatrical
lengthening, unusual pitch, alignment errors), so the engine can avoid outliers.
"""
import os, sys, struct, re
import numpy as np
import xml.etree.ElementTree as ET
from sklearn.tree import DecisionTreeRegressor

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
from labels import make_parser

PHONE_FIELDS = ["prev_prev_name", "prev_name", "name", "next_name", "next_next_name"]
MAGIC = b"FSUM0001"
UVULAR_EMPH = {"q"}


def read_units(path):
    f = open(path, "rb")
    assert f.read(8) == b"FSUV0002"
    nu, nutt, jd, nc, sr = struct.unpack("<IIIII", f.read(20))
    f.read(4 * jd)
    f.read(20 * nutt)
    hdr_end = f.tell()
    rec = 16 + nc + 4 * (jd + 1)
    raw = np.frombuffer(f.read(nu * rec), dtype=np.uint8).reshape(nu, rec)
    a = raw[:, 8:12].copy().view("<u4").ravel(); b = raw[:, 12:16].copy().view("<u4").ravel()
    half = raw[:, 2].astype(np.int8)
    o = 16 + nc
    jl = raw[:, o:o + 2 * (jd + 1)].copy().view("<f2").astype(np.float32)
    jr = raw[:, o + 2 * (jd + 1):o + 4 * (jd + 1)].copy().view("<f2").astype(np.float32)
    return dict(nu=nu, jd=jd, nc=nc, sr=sr, hdr_end=hdr_end, rec=rec, a=a, b=b, half=half,
                spec=(jl[:, :jd] + jr[:, :jd]) / 2, lf0l=jl[:, jd], lf0r=jr[:, jd])


def main():
    vd = sys.argv[1]
    pxml = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lang", "Arabic", "phonemes.xml")
    U = read_units(os.path.join(vd, "units.bin"))
    layout = [l.rstrip("\n").split("\t") for l in open(os.path.join(vd, "layout.txt"), encoding="utf-8") if l.strip()]
    layout = [(t + ["", "", ""])[:3] for t in layout]
    names = [t[0] for t in layout]
    parse = make_parser([tuple(t) for t in layout])
    rows = [l.rstrip("\n").split(" ", 1) for l in open(os.path.join(vd, "unit_labels.txt"), encoding="utf-8")]
    assert len(rows) == U["nu"], "unit_labels.txt does not match units.bin"
    vals = []
    for ut, lab in rows:
        g = parse.match(lab)
        vals.append((ut, g.groups() if g else tuple("x" for _ in names)))

    # questions -------------------------------------------------------------
    phones = {}
    for p in ET.parse(pxml).getroot():
        phones[p.get("name")] = {k: v for k, v in p.attrib.items() if k != "name"}
    classes = {}   # (attr, value) -> set of phones
    for ph, at in phones.items():
        for k, v in at.items():
            classes.setdefault((k, v), set()).add(ph)
    numeric = []
    for i, n in enumerate(names):
        if n in PHONE_FIELDS:
            continue
        col = [v[1][i] for v in vals[::97]]
        if all(re.fullmatch(r"-?\d+|x", c) for c in col) and any(c != "x" for c in col):
            numeric.append(i)
    qs = []   # ("S", field index, set of values) | ("N", field index) | ("H",) | ("U",)
    for f in PHONE_FIELDS:
        if f not in names:
            continue
        fi = names.index(f)
        for ph in sorted(phones):
            qs.append(("S", fi, {ph}))
        qs.append(("S", fi, {"pau"}))
        for (k, v), st in sorted(classes.items()):
            if 1 < len(st) < len(phones):
                qs.append(("S", fi, st))
    for fi in numeric:
        qs.append(("N", fi))
    qs.append(("H",)); qs.append(("U",))
    print(len(qs), "questions,", len(numeric), "numeric fields")

    # feature matrix (training sample for speed; all units for the quality cost)
    def feats(idx):
        X = np.zeros((len(idx), len(qs)), dtype=np.float32)
        for r, k in enumerate(idx):
            ut, g = vals[k]
            for j, q in enumerate(qs):
                if q[0] == "S":
                    X[r, j] = g[q[1]] in q[2]
                elif q[0] == "N":
                    X[r, j] = -1 if g[q[1]] == "x" else int(g[q[1]])
                elif q[0] == "H":
                    X[r, j] = U["half"][k]
                else:
                    X[r, j] = {"s": 0, "q": 1, "e": 2}.get(ut, 0)
        return X

    rng = np.random.default_rng(1)
    nu = U["nu"]
    dur = np.log(np.maximum(U["b"] - U["a"], 1).astype(np.float64))
    voiced = (U["lf0l"] > 0) & (U["lf0r"] > 0)
    lf0 = np.where(voiced, (U["lf0l"] + U["lf0r"]) / 2, 0)
    spec = U["spec"].astype(np.float64)
    tr = rng.choice(nu, size=min(nu, 400000), replace=False)
    Xtr = feats(tr)
    models = []
    for name, y, mask, leaf in (("duration", dur[:, None], np.ones(nu, bool), 60),
                                ("pitch", lf0[:, None], voiced, 60),
                                ("spectrum", spec, np.ones(nu, bool), 80)):
        m = mask[tr]
        t = DecisionTreeRegressor(min_samples_leaf=leaf, max_leaf_nodes=6000, random_state=0)
        t.fit(Xtr[m], y[tr][m])
        pred = t.predict(Xtr[m]).reshape(m.sum(), -1)
        res = y[tr][m] - pred
        sd = res.std(0) + 1e-6
        print("%-9s leaves %5d  residual sd %s  (target sd %s)" % (name, t.get_n_leaves(), np.round(sd[:3], 3), np.round(y[tr][m].std(0)[:3], 3)))
        models.append((name, t, sd))

    # quality cost of every unit, in batches
    score = np.zeros(nu, dtype=np.float32)
    B = 50000
    for s0 in range(0, nu, B):
        idx = np.arange(s0, min(nu, s0 + B)); X = feats(idx)
        zd = np.abs(dur[idx] - models[0][1].predict(X)) / models[0][2][0]
        zp = np.where(voiced[idx], np.abs(lf0[idx] - models[1][1].predict(X)) / models[1][2][0], 0)
        ps = models[2][1].predict(X).reshape(len(idx), -1)
        zs = np.sqrt((((spec[idx] - ps) / models[2][2]) ** 2).mean(1))
        # only what is beyond normal variation counts: z above 1.5
        score[idx] = np.maximum(zd - 1.5, 0) + np.maximum(zp - 1.5, 0) + np.maximum(zs - 1.5, 0)
    # acoustic vowel check: is a vowel closer to another vowel than to its own label?
    rows_ph = np.array([g[names.index("name")] for _, g in vals])
    vowels = [p for p, at in phones.items() if at.get("vc") == "+"]
    stats = {}
    for p in vowels:
        m = rows_ph == p
        if m.sum() < 50:
            continue
        X = spec[m]; stats[p] = (X.mean(0), X.var(0) + 1e-3)
    def ll(X, st):
        mu, var = st
        return -0.5 * (((X - mu) ** 2) / var + np.log(var)).sum(1)
    flagged = 0
    for p, st in stats.items():
        m = np.where(rows_ph == p)[0]
        same_len = [o for o in stats if o != p and phones[o].get("vlng") == phones[p].get("vlng")]
        if not same_len:
            continue
        own = ll(spec[m], st)
        best_other = np.max(np.stack([ll(spec[m], stats[o]) for o in same_len]), 0)
        margin = own - best_other           # negative: sounds more like another vowel
        pen = np.clip(-margin / 4.0, 0, 3)  # up to 3, growing with how clearly it sounds like another vowel
        score[m] += pen.astype(np.float32); flagged += int((margin < 0).sum())
    print("vowel check: %d vowel units sound closer to another vowel" % flagged)
    print("quality cost: median %.2f, 95%% %.2f, 99%% %.2f" % tuple(np.percentile(score, [50, 95, 99])))
    q = np.clip(np.round(score * 20), 0, 255).astype(np.uint8)     # stored as cost*20 in the unit's spare byte
    with open(os.path.join(vd, "units.bin"), "r+b") as f:
        for k in range(nu):
            f.seek(U["hdr_end"] + k * U["rec"] + 3); f.write(bytes([q[k]]))

    # models.bin -------------------------------------------------------------
    with open(os.path.join(vd, "models.bin"), "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", len(qs)))
        for q_ in qs:
            if q_[0] == "S":
                s = " ".join(sorted(q_[2])).encode("utf-8")
                f.write(struct.pack("<BH", 0, q_[1])); f.write(struct.pack("<H", len(s))); f.write(s)
            elif q_[0] == "N":
                f.write(struct.pack("<BH", 1, q_[1]))
            else:
                f.write(struct.pack("<BH", 2 if q_[0] == "H" else 3, 0))
        f.write(struct.pack("<I", len(models)))
        for name, t, sd in models:
            T = t.tree_; dim = T.value.shape[1]
            f.write(struct.pack("<II", T.node_count, dim))
            f.write(sd.astype(np.float32).tobytes())
            f.write(T.feature.astype(np.int32).tobytes())
            f.write(T.threshold.astype(np.float32).tobytes())
            f.write(T.children_left.astype(np.int32).tobytes())
            f.write(T.children_right.astype(np.int32).tobytes())
            f.write(T.value.reshape(T.node_count, dim).astype(np.float32).tobytes())
    # phonetic substitution costs between context phones (used when a unit's recorded neighbour differs)
    feats_ = ["vc", "cplace", "ctype", "cvox", "emph", "vheight", "vfront", "vlng"]
    allp = sorted(phones) + ["pau"]
    with open(os.path.join(vd, "phonesim.txt"), "w", encoding="utf-8") as f:
        # vowels are coloured by both neighbours along their whole length
        f.write("#vowels %s\n" % " ".join(p for p in sorted(phones) if phones[p].get("vc") == "+"))
        for a in allp:
            for b in allp:
                if a == b:
                    continue
                if "pau" in (a, b):
                    c = 1.5
                else:
                    A, B = phones[a], phones[b]
                    keys = [k for k in feats_ if k in A or k in B]
                    diff = sum(A.get(k) != B.get(k) for k in keys) / max(len(keys), 1)
                    c = 0.35 + 0.65 * diff
                    # pharyngealised consonants colour neighbouring vowels strongly (emphasis spreading);
                    # q is marked emphatic in the phoneme set but, being uvular, colours far less
                    pa = A.get("emph") == "+" and a not in UVULAR_EMPH
                    pb = B.get("emph") == "+" and b not in UVULAR_EMPH
                    if pa != pb:
                        c += 1.0
                    elif A.get("emph", "-") != B.get("emph", "-"):
                        c += 0.3
                f.write("%s %s %.3f\n" % (a, b, c))
    print("models.bin and phonesim.txt written")


if __name__ == "__main__":
    main()
