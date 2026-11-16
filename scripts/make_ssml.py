"""text.tsv -> one SSML document (one <s> per utterance, in id order) + ids list."""
import sys, os, re
from xml.sax.saxutils import escape
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ar_prep import mark_wasl
FR = os.environ.get("RHV_LANG") == "French"
if FR:
    from fr_prep import prepare

tsv, out_ssml, out_ids = sys.argv[1:4]
ids, sents = [], []
for line in open(tsv, encoding="utf-8"):
    uid, _dur, txt = line.rstrip("\n").split("\t")
    ids.append(uid)
    t = txt.strip()
    # one recording = one sentence: mid-line sentence ends become commas (keeps the pause)
    if FR:
        t = re.sub(r"[.!?]+(?=.*\w)", ",", t)
        sents.append(prepare(t))
    else:
        t = re.sub(r"[.!?\u061F\u06D4]+(?=.*[\u0621-\u064A])", "\u060C", t)
        sents.append(mark_wasl(t))
with open(out_ssml, "w", encoding="utf-8") as f:
    f.write('<speak xml:lang="%s">\n' % ("fr" if FR else "ar"))
    for s in sents:
        f.write("<s>" + escape(s) + "</s>\n")
    f.write("</speak>\n")
open(out_ids, "w").write("\n".join(ids) + "\n")
print(len(ids), "sentences")
