"""Build a Fasih unit-selection voice from an aligned corpus.

usage: build_units.py ROOT OUTDIR
  ROOT   : a training root as made by tools/pipeline.sh (corpus/wav at 24 kHz,
           work/full/*.lab full-context labels with aligned times, work/feat/{mgc,lf0})
  OUTDIR : voice folder to write (units.bin, audio.pcm, layout.txt, phones.txt)

Each phone is cut at its middle into a left and a right half-phone unit.  Every
unit keeps the context features of its phone (the same ones the engine reads
from RHVoice's labels at synthesis time) and spectral, pitch and energy features
at both of its edges, used by the join cost.
"""
import os, sys, struct, json
import numpy as np
import soundfile as sf

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
from labels import field_layout, make_parser, read_lab

SR = 24000
FP = 120            # 5 ms frames at 24 kHz
MGC_D = 31
JD = 13             # join spectrum: mgc 0..12 (0 = energy)

# context features stored per unit: (label field, clip) -- clip=None: categorical
CTX = [
    ("prev_prev_name", None), ("prev_name", None), ("next_name", None), ("next_next_name", None),
    ("syl_stress", None), ("pos_in_syl_fw", 3), ("pos_in_syl_bw", 3),
    ("syl_pos_in_word_fw", 3), ("syl_pos_in_word_bw", 3),
    ("syl_pos_in_phrase_fw", 4), ("syl_pos_in_phrase_bw", 4),
    ("word_pos_in_phrase_bw", 3), ("phrase_pos_in_utt_fw", 3), ("phrase_pos_in_utt_bw", 3),
    ("pos_in_word_fw", 3), ("pos_in_word_bw", 3), ("next_syl_stress", None), ("prev_syl_stress", None),
]
MAGIC = b"FSUV0002"


def utt_type(text):
    t = text.strip().rstrip("‏ \"'»)")
    if t.endswith(("?", "؟")):
        return "q"
    if t.endswith("!"):
        return "e"
    return "s"


def main():
    root, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    W = os.path.join(root, "work")
    lang_xml = os.environ.get("RHV_LABELLING") or os.path.join(root, "lang", os.environ.get("RHV_LANG", "Arabic"), "labelling.xml")
    fields = field_layout(lang_xml)
    names = [f[0] for f in fields]
    parse = make_parser(fields)
    with open(os.path.join(out, "layout.txt"), "w", encoding="utf-8") as f:
        for n, l, r in fields:
            f.write("%s\t%s\t%s\n" % (n, l, r))
    ni = names.index("name")
    # a language whose labels lack some of these fields simply does without them
    global CTX
    CTX = [c for c in CTX if c[0] in names]
    ci = [names.index(n) for n, _ in CTX]

    texts = {}
    for line in open(os.path.join(root, "corpus", "text.tsv"), encoding="utf-8"):
        u, _, t = line.rstrip("\n").split("\t")
        texts[u] = t
    # utterance order: work/mono.ssml order == sorted mono names (full_labels.py)
    mono = sorted(n[:-4] for n in os.listdir(os.path.join(W, "mono")) if n.endswith(".lab"))
    if os.environ.get("KEEP_UTTS"):     # pruned voice: only these recordings (unit/prune.py)
        keep = set(l.strip() for l in open(os.environ["KEEP_UTTS"]) if l.strip())
        mono = [m for m in mono if m in keep]
    full_dir = os.path.join(W, "full")

    phones = {"pau": 0}
    vocab = [dict() for _ in CTX]   # categorical values -> ids (per feature)

    def cat(k, v):
        d = vocab[k]
        if v not in d:
            d[v] = len(d)
        return d[v]

    units = []      # tuples
    unit_labels = []  # full-context label of every unit (for build_models.py)
    audio = open(os.path.join(out, "audio.pcm"), "wb")
    utt_tab = []
    pos = 0
    skipped = 0
    for ui, uid in enumerate(mono):
        # RHVoice-make-hts-labels names its outputs after the utterance index
        cand = [os.path.join(full_dir, uid + ".lab"), os.path.join(full_dir, "u_%d.lab" % (ui + 1))]
        fl = next((c for c in cand if os.path.exists(c)), None)
        mg = os.path.join(W, "feat", "mgc", uid + ".mgc")
        if fl is None or not os.path.exists(mg):
            skipped += 1
            continue
        labs = read_lab(fl)
        x, sr = sf.read(os.path.join(root, "corpus", "wav", uid + ".wav"), dtype="int16")
        assert sr == SR
        mgc = np.fromfile(mg, dtype=np.float32).reshape(-1, MGC_D)
        lf0 = np.fromfile(os.path.join(W, "feat", "lf0", uid + ".lf0"), dtype=np.float32)
        T = min(len(mgc), len(lf0))
        ut = utt_type(texts.get(uid, ""))

        def jfeat(sample):
            fr = int(np.clip(round(sample / FP), 0, T - 1))
            a, b = max(fr - 1, 0), min(fr + 2, T)
            m = mgc[a:b, :JD].mean(0)
            lv = lf0[a:b]
            v = lv[lv > -1e9]
            lp = float(np.median(v)) if len(v) >= 2 else -1.0
            return m, lp

        base = pos
        audio.write(x.tobytes())
        nph = sum(1 for (_s, _e, l) in labs if parse.match(l) and parse.match(l).groups()[ni] != "pau")
        sp = sum((e - s) * 1e-7 for (s, e, l) in labs if parse.match(l) and parse.match(l).groups()[ni] != "pau")
        lv = lf0[lf0 > -1e9]
        utt_tab.append((base, len(x), nph / max(sp, 0.1), float(np.median(lv)) if len(lv) else 0.0))
        pos += len(x)
        for (s, e, lab) in labs:
            g = parse.match(lab)
            if g is None:
                continue
            g = g.groups()
            ph = g[ni]
            if ph == "pau":
                continue
            s_smp = int(round(s * 1e-7 * SR)); e_smp = int(round(e * 1e-7 * SR))
            if e_smp - s_smp < 2 * FP // 2:
                continue
            if ph not in phones:
                phones[ph] = len(phones)
            ctx = []
            for k, (n, clip) in enumerate(CTX):
                v = g[ci[k]]
                if clip is None:
                    ctx.append(cat(k, v))
                else:
                    try:
                        ctx.append(min(int(v), clip))
                    except ValueError:
                        ctx.append(0)
            ctx.append({"s": 0, "q": 1, "e": 2}[ut])
            mid = (s_smp + e_smp) // 2
            unit_labels.append((lab, ut)); unit_labels.append((lab, ut))
            for half, (a, b) in enumerate(((s_smp, mid), (mid, e_smp))):
                ma, la = jfeat(a); mb, lb = jfeat(b)
                units.append((phones[ph], half, len(utt_tab) - 1, base + a, base + b, ctx, ma, la, mb, lb))
        if ui % 1000 == 0:
            print(ui, len(units), flush=True)
    audio.close()

    # normalisation of the join spectrum (per-dim std) so all dims weigh alike
    M = np.array([u[6] for u in units], dtype=np.float32)
    sd = M.std(0) + 1e-6
    NC = len(CTX) + 1
    with open(os.path.join(out, "units.bin"), "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IIIII", len(units), len(utt_tab), JD, NC, SR))
        f.write(sd.astype(np.float32).tobytes())
        for b, n, rate, f0m in utt_tab:
            f.write(struct.pack("<QIff", b, n, rate, f0m))
        # units: positions relative to their recording, join features in half precision
        for (p, h, u, a, b, ctx, ma, la, mb, lb) in units:
            ub = utt_tab[u][0]
            f.write(struct.pack("<HBBIII", p, h, 0, u, a - ub, b - ub))
            f.write(np.array(ctx, dtype=np.uint8).tobytes())
            f.write((ma / sd).astype(np.float16).tobytes()); f.write(np.float16(la).tobytes())
            f.write((mb / sd).astype(np.float16).tobytes()); f.write(np.float16(lb).tobytes())
    with open(os.path.join(out, "unit_labels.txt"), "w", encoding="utf-8") as f:   # training only, not shipped
        for lab, ut in unit_labels:
            f.write("%s %s\n" % (ut, lab))
    with open(os.path.join(out, "phones.txt"), "w", encoding="utf-8") as f:
        for p, i in sorted(phones.items(), key=lambda t: t[1]):
            f.write(p + "\n")
    with open(os.path.join(out, "context.txt"), "w", encoding="utf-8") as f:
        for k, (n, clip) in enumerate(CTX):
            vals = "" if clip is not None else " ".join(v for v, _ in sorted(vocab[k].items(), key=lambda t: t[1]))
            f.write("%s\t%s\t%s\n" % (n, "" if clip is None else clip, vals))
        f.write("utt_type\t\ts q e\n")
    print("units", len(units), "utts", len(utt_tab), "skipped", skipped, "hours", pos / SR / 3600)


if __name__ == "__main__" and len(sys.argv) == 3:
    main()


def pitch_marks(x, lf0, sr=SR, fp=FP):
    """Epochs: voiced -> one mark per period at the main negative/positive peak, unvoiced -> every 5 ms.
    Returns int64 array of pos*2+voiced."""
    from scipy.signal import butter, sosfiltfilt
    y = sosfiltfilt(butter(4, 900, fs=sr, output="sos"), x.astype(np.float64))
    # polarity: use the sign with larger peaks
    if np.percentile(-y, 99.5) > np.percentile(y, 99.5):
        y = -y
    n = len(x); T = len(lf0)
    f0 = np.where(lf0 > -1e9, np.exp(np.clip(lf0, 3, 7)), 0.0)
    marks = []
    t = 0
    while t < n:
        fr = min(int(t / fp), T - 1)
        if f0[fr] > 0:
            P = sr / f0[fr]
            if marks and marks[-1][1]:
                c = marks[-1][0] + P
                lo, hi = int(c - 0.25 * P), int(c + 0.25 * P) + 1
            else:
                lo, hi = t, int(t + P) + 1
            lo = max(lo, 0); hi = min(hi, n)
            if hi <= lo:
                break
            m = lo + int(np.argmax(y[lo:hi]))
            if marks and m <= marks[-1][0]:
                m = marks[-1][0] + 1
            marks.append((m, 1)); t = m + 1
        else:
            m = (marks[-1][0] + fp // 1) if marks else 0
            if m >= n:
                break
            marks.append((m, 0)); t = m + 1
            # snap: next mark exactly 5 ms on
    return np.array([m * 2 + v for m, v in marks], dtype=np.int64)


def build_marks(root, out):
    """marks.bin: per utterance (same order as units.bin) uint32 count + uint32 (pos*2+voiced)."""
    W = os.path.join(root, "work")
    mono = sorted(n[:-4] for n in os.listdir(os.path.join(W, "mono")) if n.endswith(".lab"))
    if os.environ.get("KEEP_UTTS"):
        keep = set(l.strip() for l in open(os.environ["KEEP_UTTS"]) if l.strip())
        mono = [m for m in mono if m in keep]
    from multiprocessing import Pool
    with Pool(int(os.environ.get("WORKERS", "2"))) as p:
        res = p.map(_marks_one, [(root, u) for u in mono], 32)
    with open(os.path.join(out, "marks.bin"), "wb") as f:
        f.write(b"FSPM0002"); f.write(struct.pack("<I", len(res)))
        for m in res:
            # value = pos*2+voiced; stored as the first value (u32) then u16 differences
            m = m.astype(np.int64)
            d = np.diff(m)
            assert len(m) == 0 or (d.min() > 0 and d.max() < 65536), "mark gap too large"
            f.write(struct.pack("<I", len(m)))
            if len(m):
                f.write(struct.pack("<I", int(m[0]))); f.write(d.astype(np.uint16).tobytes())
    print("marks", sum(len(m) for m in res))


def _marks_one(a):
    root, uid = a
    W = os.path.join(root, "work")
    x, sr = sf.read(os.path.join(root, "corpus", "wav", uid + ".wav"), dtype="int16")
    lf0 = np.fromfile(os.path.join(W, "feat", "lf0", uid + ".lf0"), dtype=np.float32)
    return pitch_marks(x, lf0)


if __name__ == "__main__" and len(sys.argv) > 3 and sys.argv[3] == "marks":
    build_marks(sys.argv[1], sys.argv[2])
