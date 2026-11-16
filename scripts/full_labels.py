"""Full-context labels with aligned times (and aligned pauses) via RHVoice's own labeller.
Uses every utterance that has a mono label in work/mono/."""
import os, sys, subprocess
from xml.sax.saxutils import escape
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ar_prep import mark_wasl
import re
FR = os.environ.get("RHV_LANG") == "French"
if FR:
    from fr_prep import prepare
    mark_wasl = lambda t: prepare(re.sub(r"[.!?]+(?=.*\w)", ",", t))
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
W = os.path.join(ROOT, "work")
text = {}
for line in open(os.path.join(ROOT, "corpus", "text.tsv"), encoding="utf-8"):
    uid, _d, t = line.rstrip("\n").split("\t"); text[uid] = t
mono = sorted(n[:-4] for n in os.listdir(os.path.join(W, "mono")) if n.endswith(".lab"))
with open(os.path.join(W, "mono.ssml"), "w", encoding="utf-8") as f:
    f.write('<speak xml:lang="%s">\n' % ("fr" if FR else "ar") + "".join("<s>" + escape(mark_wasl(text[u])) + "</s>\n" for u in mono) + "</speak>\n")
os.makedirs(os.path.join(W, "full"), exist_ok=True)
env = dict(os.environ, RHVOICE_DATA_PATH=os.path.join(ROOT, "data"), RHVOICE_CONFIG_PATH="/tmp/labt")
r = subprocess.run(["/tmp/rhb2/src/utils/RHVoice-make-hts-labels", "-l", os.path.join(W, "mono"),
                    os.path.join(W, "mono.ssml"), os.path.join(W, "full")], env=env, capture_output=True, text=True)
print(len(mono), "utterances; rc", r.returncode, r.stderr[-500:])
