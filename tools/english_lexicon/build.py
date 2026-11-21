"""Rebuild RHVoice's English lexicon (cmulex.fst) for Fasih.

Keeps every entry of RHVoice's own lexicon (Festival's CMU lexicon), adds the words of the
current CMU Pronouncing Dictionary that it lacks (about 12,000; RHVoice's letter-to-sound rules
got about half of them wrong, "changer" among them), and applies FIXES below, where both
dictionaries are wrong (CMUdict drops the l of "Nabil").

usage: build.py ORIGINAL_cmulex.fst cmudict.dict OUT_cmulex.fst
needs foma and RHVoice's src/scripts/general/att2bin (set ATT2BIN)."""
import sys, os, json, collections, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fstdump import load, paths

FIXES = {
    "nabil": "n aa0 b iy1 l",
    "fasih": "f aa0 s iy1 hh",
    "rawi": "r aa1 w iy0",
    "barq": "b aa1 r k",
}

orig, cmudict, out = sys.argv[1:4]
syms, states = load(orig)
lex = {w: ph for w, ph in paths(syms, states)}
cmu = {}
for l in open(cmudict, encoding="utf-8"):
    l = l.split("#")[0].strip()
    if l and "(" not in l.split()[0]:
        p = l.split(); cmu[p[0]] = [x.lower() for x in p[1:]]
# CMUdict's notation -> Festival's, learnt from the words both have
pairs = collections.Counter()
for w, ph in lex.items():
    c = cmu.get(w)
    if c and len(c) == len(ph):
        pairs.update(zip(c, ph))
best = {}
for (a, b), n in pairs.items():
    if a not in best or n > pairs[(a, best[a])]:
        best[a] = b
added = 0
for w, ph in cmu.items():
    if w not in lex and w.isalpha() and w.isascii():
        lex[w] = [best.get(x, x) for x in ph]; added += 1
for w, p in FIXES.items():
    lex[w] = p.split()
with open(out + ".txt", "w", encoding="utf-8") as f:
    for w in sorted(lex):
        f.write(" ".join(w) + "\n" + " ".join(lex[w]) + "\n")
subprocess.run(["foma", "-e", "read spaced-text %s.txt" % out, "-e", "write att %s.att" % out, "-s"], check=True)
subprocess.run([sys.executable, os.environ.get("ATT2BIN", "att2bin"), out + ".att", out], check=True)
os.remove(out + ".txt"); os.remove(out + ".att")
print(len(lex), "entries;", added, "added from CMUdict;", len(FIXES), "fixes")
