"""DER/WER of ONNX diacritizers (gated_cls interface) on a vowelled test file.
usage: der_onnx.py TEST.txt MODEL.onnx..."""
import sys, json, unicodedata, time
import numpy as np, onnxruntime as ort
voc = json.load(open("/home/claude/rawift/rawi_v3.vocab.json")); C2I, D2I = voc["char_to_idx"], voc["diac_to_idx"]; I2D = {v: k for k, v in D2I.items()}
H = "ًٌٍَُِّْ"
def diac(sess, bare):
    t = unicodedata.normalize("NFD", bare); t = "".join(c for c in t if unicodedata.category(c) != "Mn")
    ids = np.array([[C2I.get(c, C2I["<UNK>"]) for c in t]], dtype=np.int64)
    out = sess.run(None, {"input": ids})[0]
    if out.ndim == 3: out = out.argmax(-1)
    s = "".join(c + (I2D.get(int(k), "") if unicodedata.category(c).startswith("L") else "") for c, k in zip(t, out[0]))
    return unicodedata.normalize("NFC", s)
def split(s):
    base, marks = [], []
    for c in unicodedata.normalize("NFC", s):
        if c in H:
            if marks: marks[-1] += c
        elif c in "ٰـ" or unicodedata.category(c) == "Mn": continue
        else: base.append(c); marks.append("")
    return base, ["".join(sorted(m)) for m in marks]
import os, re
LEX = json.load(open(os.environ["LEX"], encoding="utf-8")) if os.environ.get("LEX") else None
_W = re.compile("[\u0621-\u064A\u064B-\u0652]+")
def apply_lex(text):
    def fix(m):
        w = m.group(0); bare = "".join(c for c in w if c not in H)
        st = LEX.get(bare)
        if not st: return w
        i = len(w)
        while i and w[i - 1] in H: i -= 1
        return st + w[i - 1:]
    return _W.sub(fix, text)
test = [l.strip() for l in open(sys.argv[1], encoding="utf-8") if l.strip()]
for p in sys.argv[2:]:
    s = ort.InferenceSession(p); e = n = we = wn = 0; t0 = time.perf_counter(); skipped = 0
    for ref in test:
        bare = "".join(c for c in unicodedata.normalize("NFC", ref) if c not in H)
        hyp = diac(s, bare)
        if LEX: hyp = apply_lex(unicodedata.normalize('NFC', hyp))
        rb, rm = split(ref); hb, hm = split(hyp)
        if [unicodedata.normalize("NFD", x)[0] for x in rb] != [unicodedata.normalize("NFD", x)[0] for x in hb]: skipped += 1; continue
        cur = []; words = []
        for b, r, h in zip(rb, rm, hm):
            if "ء" <= b <= "ي":
                bad = r.replace("ْ", "") != h.replace("ْ", ""); n += 1; e += bad; cur.append(bad)
            elif cur: words.append(any(cur)); cur = []
        if cur: words.append(any(cur))
        we += sum(words); wn += len(words)
    print("%-28s DER %.2f%%  WER %.1f%%  %.1f ms/sent  (skipped %d)" % (p.split("/")[-1], 100 * e / n, 100 * we / wn, 1000 * (time.perf_counter() - t0) / len(test), skipped), flush=True)
