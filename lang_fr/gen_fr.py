"""Generate the sources of the French data-only RHVoice language (Fasih project).

g2p: a pronunciation lexicon (ipa-dict fr_FR, MIT, about 246,000 word forms) with
letter-to-sound rules as the fallback, elided clitics (l', d', qu'...) joined to their
word, and liaison consonants written by the text front end as a prefix (‿z, ‿t, ‿n, ‿r, ‿p).
Numbers are read in full (up to the billions). Author: Riad Assoum project (written with
Claude). License: LGPL-2.1+ like RHVoice; the lexicon keeps its MIT license.
usage: gen_fr.py LEXICON OUTDIR"""
import os, re, sys

LEX, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)

def w(name, text):
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
        f.write(text)

def q(s):
    return '"' + s.replace('"', '\\"') + '"'

# ------------------------------------------------------------------ phones
VOWELS = ["i", "e", "E", "a", "O", "o", "u", "y", "eu", "oe", "ax", "en", "an", "on"]
GLIDES = ["j", "w", "hw"]
CONS = ["p", "b", "t", "d", "k", "g", "f", "v", "s", "z", "sh", "zh", "m", "n", "gn", "ng", "l", "r"]
IPA = {"i": "i", "e": "e", "ɛ": "E", "a": "a", "ɑ": "a", "ɔ": "O", "o": "o", "u": "u", "y": "y",
       "ø": "eu", "œ": "oe", "ə": "ax", "ɛ̃": "en", "œ̃": "en", "ɑ̃": "an", "ɔ̃": "on", "ã": "an", "õ": "on", "ẽ": "en",
       "j": "j", "w": "w", "ɥ": "hw", "p": "p", "b": "b", "t": "t", "d": "d", "k": "k", "g": "g", "ɡ": "g",
       "f": "f", "v": "v", "s": "s", "z": "z", "ʃ": "sh", "ʒ": "zh", "m": "m", "n": "n", "ɲ": "gn", "ŋ": "ng",
       "l": "l", "ʁ": "r", "r": "r", "x": "r", "ɪ": "i", "ʊ": "u"}
SKIP = set("ˈˌː.‿()ʼ' -")

def ipa2ph(p):
    out, i = [], 0
    while i < len(p):
        if p[i] in SKIP:
            i += 1; continue
        two = p[i:i + 2]
        if two in IPA:
            out.append(IPA[two]); i += 2; continue
        if p[i] in IPA:
            out.append(IPA[p[i]]); i += 1; continue
        return None
    return out

LOWER = "abcdefghijklmnopqrstuvwxyzàâæçèéêëîïñôöùûüÿœ"
UPPER = "ABCDEFGHIJKLMNOPQRSTUVWXYZÀÂÆÇÈÉÊËÎÏÑÔÖÙÛÜŸŒ"
LIAISON = "‿"   # ‿ : the front end writes ‿z ‿t ‿n ‿r ‿p before a word

# ------------------------------------------------------------------ lexicon (AT&T trie)
lex = {}
for line in open(LEX, encoding="utf-8"):
    word, pron = line.rstrip("\n").split("\t")
    word = word.lower().replace("’", "'")
    if not word or any(c not in LOWER + "'" for c in word) or word in lex:
        continue
    ph = ipa2ph(pron.split(",")[0].strip().strip("/"))
    if ph:
        lex[word] = ph
# a few words the lexicon lacks or gets wrong for reading aloud
lex.update({"fasih": ["f", "a", "s", "i"], "nvda": ["E", "n", "v", "e", "d", "e", "a"], "et": ["e"],
            "est": ["E"], "les": ["l", "e"], "des": ["d", "e"], "ces": ["s", "e"], "mes": ["m", "e"],
            "tes": ["t", "e"], "ses": ["s", "e"], "plus": ["p", "l", "y"], "tous": ["t", "u"], "fils": ["f", "i", "s"],
            "os": ["O", "s"], "a": ["a"], "à": ["a"], "y": ["i"], "e": ["eu"]})
states, arcs, finals = 1, [], []
trie = {}
for word in sorted(lex):
    node, s = trie, 0
    for ch in word:
        if ch not in node:
            node[ch] = [states, {}]; arcs.append((s, states, ch, "@0@")); states += 1
        s, node = node[ch][0], node[ch][1]
    # output the phones on an epsilon path ending in a final state
    prev = s
    for k, p in enumerate(lex[word]):
        arcs.append((prev, states, "@0@", p)); prev = states; states += 1
    finals.append(prev)
with open(os.path.join(OUT, "lex.att"), "w", encoding="utf-8") as f:
    for a in arcs:
        f.write("%d\t%d\t%s\t%s\n" % a)
    for s in finals:
        f.write("%d\n" % s)
print("lexicon", len(lex), "words", states, "states", file=sys.stderr)

# ------------------------------------------------------------------ alphabet
VL = "aàâæeéèêëiîïoôöuùûüyÿœ"
w("alphabet.foma", "define Letter [" + " | ".join(q(c) for c in LOWER) + "] ;\n"
  "define Upper [" + " | ".join(q(c) for c in UPPER) + "] ;\n"
  "define Grapheme Letter | Upper ;\n"
  "define V [" + " | ".join(q(c) for c in VL) + "] ;\n"
  "define C [Letter - V] ;\n"
  "define Apos %' | %’ ;\n")

# ------------------------------------------------------------------ letter-to-sound fallback
# (graphemes, phones, left context, right context); applied in one leftmost-longest pass,
# every rule seeing the original spelling. "#" = word boundary. Unmatched letters are silent.
FRONT = '[e|i|y|é|è|ê|ë|î|ï]'
NV = '[C - [n|m]]'                  # not followed by a vowel or n/m: nasal context (or the end)
R = []
def r(g, p, L="", Rc=""):
    R.append((g, p, L, Rc))
for g in ("aim", "ain", "eim", "ein"): r(g, "en", "", NV)
r("oin", "w en", "", NV); r("ien", "j en", "", NV)
for g in ("an", "am", "en", "em"): r(g, "an", "", NV)
for g in ("on", "om"): r(g, "on", "", NV)
for g in ("in", "im", "yn", "ym", "un", "um"): r(g, "en", "", NV)
r("eau", "o"); r("au", "o"); r("ou", "u"); r("où", "u"); r("oû", "u"); r("ou", "w", "", "V")
r("oi", "w a"); r("oî", "w a"); r("oy", "w a j", "", "V")
for g in ("ai", "aî", "ei", "ê"): r(g, "E")
r("ay", "E j", "", "V"); r("eu", "eu"); r("œu", "eu"); r("eû", "eu"); r("œ", "eu")
r("er", "e", "V C*", ".#."); r("ez", "e", "", ".#."); r("et", "E", "", ".#.")
r("es", "", "V C*", ".#."); r("ent", "", "V C*", ".#.")
r("ail", "a j", "", ".#."); r("eil", "E j", "", ".#."); r("ouil", "u j", "", ".#.")
r("aill", "a j"); r("eill", "E j"); r("ouill", "u j"); r("ill", "i j", "C", "")
r("a", "a"); r("à", "a"); r("â", "a"); r("é", "e"); r("è", "E"); r("ë", "E")
r("i", "i"); r("î", "i"); r("ï", "i"); r("i", "j", "", "V")
r("o", "O"); r("o", "o", "", ".#."); r("o", "o", "", "[s|t|x] .#."); r("ô", "o")
r("u", "y"); r("û", "y"); r("ù", "y"); r("ü", "y"); r("u", "hw", "", "[i|é|è|e|a]")
r("y", "i"); r("y", "j", "V", "V")
r("e", "", "V C*", ".#."); r("e", "E", "", "[C - [r|l]] C"); r("e", "E", "", "[x]"); r("e", "ax")
r("ch", "sh"); r("ph", "f"); r("th", "t"); r("gn", "gn"); r("qu", "k"); r("gu", "g", "", FRONT)
r("ge", "zh", "", "[a|o|u]"); r("c", "s", "", FRONT); r("ç", "s"); r("c", "k"); r("cc", "k s", "", FRONT)
r("cc", "k"); r("ck", "k"); r("sc", "s", "", FRONT)
r("g", "zh", "", FRONT); r("g", "g"); r("ss", "s"); r("s", "z", "V", "V"); r("s", "s")
r("tion", "s j on"); r("x", "k s"); r("x", "g z", ".#. e", "V"); r("z", "z"); r("h", ""); r("j", "zh")
r("w", "w"); r("k", "k"); r("q", "k")
for c in "bdflmnprtv":
    r(c, c); r(c + c, c)
for c in "tdsxzpg":
    r(c, "", "", ".#.")
r("ll", "l")

def ctx(s):
    s = s.replace("[C - [n|m]]", "\x01").replace("[C - [r|l]]", "\x02")
    s = s.replace("V", " VX ").replace("C", " CX ")
    return s.replace("\x01", " NMC ").replace("\x02", " NRC ")

def grs(g):
    return "{" + g + "}" if len(g) > 1 else q(g)

def phs(p):
    return "[" + " ".join(q("#" + x) for x in p.split()) + "]" if p else "0"

# one rule after another: longer spellings first, and within a length the rules with a context
# before the general one (the outputs are #-symbols, so a later rule never rewrites them)
order = sorted(range(len(R)), key=lambda i: (-len(R[i][0]), 0 if (R[i][2] or R[i][3]) else 1, i))
parts = []
for i in order:
    g, p, L, Rc = R[i]
    rule = f"[{grs(g)} @-> {phs(p)}"
    if L or Rc:
        rule += f" || {ctx(L) if L else ''} _ {ctx(Rc) if Rc else ''}"
    parts.append(rule + "]")
rules = " .o.\n  ".join(parts)
PH = VOWELS + GLIDES + CONS
w("g2p.foma", f"""source alphabet.foma
read att lex.att
define Lex ;
define VPh [{" | ".join(q("#" + x) for x in VOWELS)}] ;
define CPh [{" | ".join(q("#" + x) for x in CONS + GLIDES)}] ;
define VX V | VPh ;
define CX C | CPh ;
define NMC [CX - ["n" | "m" | "#n" | "#m"]] | .#. ;
define NRC [CX - ["r" | "l" | "#r" | "#l"]] ;
define LTS {rules} ;
define Ren [{" | ".join(f'{q("#" + x)}:{q(x)}' for x in PH)}] ;
define Clean [[Letter | %'] -> 0] .o. Ren* ;
define Rules LTS .o. Clean ;
define Word Lex .P. Rules ;
define Clitic [{{l'}}:"l" | {{d'}}:"d" | {{j'}}:"zh" | {{c'}}:"s" | {{s'}}:"s" | {{n'}}:"n" | {{m'}}:"m" | {{t'}}:"t" |
  {{qu'}}:"k" | {{jusqu'}}:["zh" "y" "s" "k"] | {{lorsqu'}}:["l" "O" "r" "s" "k"] | {{puisqu'}}:["p" "hw" "i" "s" "k"] |
  {{quoiqu'}}:["k" "w" "a" "k"] | {{presqu'}}:["p" "r" "E" "s" "k"]] ;
define Liaison [{q(LIAISON)}:0 ["z" | "t" | "n" | "r" | "p" | "v"]] ;
define G2P Lex .P. [ (Liaison) (Clitic) Word ] ;
regex G2P ;
""")

# ------------------------------------------------------------------ syllables and stress
QV = " | ".join(q(v) for v in VOWELS)
QC = " | ".join(q(c) for c in CONS + GLIDES)
w("syl.foma", f"""define V [{QV}] ;
define C [{QC}] ;
define Obs ["p" | "b" | "t" | "d" | "k" | "g" | "f" | "v"] ;
define Onset C | [Obs ["l" | "r"]] | [C ["j" | "w" | "hw"]] | [Obs ["l" | "r"] ["j" | "w" | "hw"]] ;
define MarkSyl [V C*] @> ... "." || _ Onset V ;
define Syl [C | V]+ ;
define Stress [..] -> "_stress" || [.#. | "."] [C* [V - "ax"] C*] _ .#. ;
define Stress2 [..] -> "_stress" || [.#. | "."] Syl _ "." [C* "ax" C*] .#. ;
regex [C | V]+ .o. MarkSyl .o. Stress .o. Stress2 ;
""")

# ------------------------------------------------------------------ tokenizer and case
w("tok.foma", f"""source alphabet.foma
define NZDigit 1|2|3|4|5|6|7|8|9 ;
define Digit %0|NZDigit ;
define Number %0 | [NZDigit Digit*] ;
define L Grapheme | {q(LIAISON)} ;
define Word L+ [Apos L+]* ;
define Stage1 Number @-> ... num || _ ,,
Word @-> ... word || _ .#. | \\L ,,
\\[L | Digit] @-> ... sym || _ ;
define Stage2 num -> dig || Digit^13 _ ;
regex Stage1 .o. Stage2 ;
""")
w("downcase.foma", "source alphabet.foma\ndefine Map [" + " | ".join(f"{q(u)}:{q(l)}" for u, l in zip(UPPER, LOWER)) +
  " | %’:%'] ;\nregex [Map | Letter | %' | " + q(LIAISON) + "]* ;\n")

# ------------------------------------------------------------------ function words (gpos)
FUNC = {
 "prep": "à au aux de du des en dans par pour sur sous avec sans chez vers entre contre depuis pendant avant après",
 "conj": "et ou mais donc or ni car que qu' quand comme si lorsque puisque",
 "det": "le la les l' un une ce cet cette ces mon ma mes ton ta tes son sa ses notre nos votre vos leur leurs",
 "pron": "je j' tu il elle on nous vous ils elles me m' te t' se s' lui y moi toi soi qui quoi dont",
 "part": "ne n' pas plus jamais rien très",
}
lines, seen = [], set()
for tag, words in FUNC.items():
    fs = [x for x in words.split() if x not in seen]; seen.update(fs)
    lines.append(f"define {tag.capitalize()} [" + " | ".join("{" + x + "}" if len(x) > 1 else q(x) for x in fs) + "] ;")
w("gpos.foma", "\n".join(lines) + """
define FunctionWord Prep | Conj | Det | Pron | Part ;
regex [ Prep:prep | Conj:conj | Det:det | Pron:pron | Part:part | [? - FunctionWord]:content ]* ;
""")

# ------------------------------------------------------------------ numbers in words
U = ["zéro", "un", "deux", "trois", "quatre", "cinq", "six", "sept", "huit", "neuf", "dix", "onze", "douze",
     "treize", "quatorze", "quinze", "seize"]
T = {2: "vingt", 3: "trente", 4: "quarante", 5: "cinquante", 6: "soixante"}
def below100(n):
    if n <= 16: return U[n]
    if n < 20: return "dix " + U[n - 10]
    t, u = divmod(n, 10)
    if t in T:
        return T[t] + ("" if u == 0 else (" et un" if u == 1 else " " + U[u]))
    if t == 7: return "soixante " + ("et onze" if u == 1 else below100(10 + u))
    if t == 8: return "quatre vingt" + ("s" if u == 0 else " " + U[u])
    return "quatre vingt " + below100(10 + u)
def below1000(n, plural=True):
    h, r_ = divmod(n, 100)
    if h == 0:
        t = below100(r_)
        return t[:-1] if (not plural and t.endswith("vingts")) else t
    head = "cent" if h == 1 else U[h] + " cent" + ("s" if (r_ == 0 and plural) else "")
    tail = below100(r_) if r_ else ""
    if tail and not plural and tail.endswith("vingts"):
        tail = tail[:-1]
    return head + (" " + tail if tail else "")
def fq(s):
    return "[" + " ".join(q(x) for x in s.split()) + "]" if s else "0"
def key(n, width=0):
    t = f"{n:0{width}d}" if width else str(n)
    return "{" + t + "}" if len(t) > 1 else t
def alts(rng, width=0, plural=True):
    return " |\n".join(f"{key(n, width)}:{fq(below1000(n, plural))}" for n in rng)
w("numbers.foma", f"""define N3 {alts(range(1000))} ;
define Z3 {{000}}:0 | {alts(range(1, 1000), 3)} ;
define M2 {alts(range(2, 1000), 0, False)} ;
define M2Z {alts(range(2, 1000), 3, False)} ;
define T3 {{000}}:0 | {{001}}:"mille" | [M2Z 0:"mille"] ;
define MM3 {{000}}:0 | {{001}}:["un" "million"] | [M2Z 0:"millions"] ;
define Thousands [{{1}}:"mille" | [M2 0:"mille"]] Z3 ;
define Millions [{{1}}:["un" "million"] | [M2 0:"millions"]] T3 Z3 ;
define Milliards [{{1}}:["un" "milliard"] | [M2 0:"milliards"]] MM3 T3 Z3 ;
regex N3 | Thousands | Millions | Milliards ;
""")

# ------------------------------------------------------------------ spelling, keys, messages
LETTER = {"a": "a", "b": "bé", "c": "cé", "d": "dé", "e": "e", "f": "effe", "g": "gé", "h": "hache", "i": "i",
          "j": "ji", "k": "ka", "l": "elle", "m": "emme", "n": "enne", "o": "o", "p": "pé", "q": "cu", "r": "erre",
          "s": "esse", "t": "té", "u": "u", "v": "vé", "w": "double vé", "x": "ixe", "y": "i grec", "z": "zède",
          "é": "e accent aigu", "è": "e accent grave", "ê": "e accent circonflexe", "ë": "e tréma",
          "à": "a accent grave", "â": "a accent circonflexe", "î": "i accent circonflexe", "ï": "i tréma",
          "ô": "o accent circonflexe", "ù": "u accent grave", "û": "u accent circonflexe", "ü": "u tréma",
          "ç": "cé cédille", "œ": "e dans l'o", "æ": "a e", "ÿ": "i grec tréma", "ñ": "enne tilde", "ö": "o tréma"}
DIG = U[:10]
PUN = {",": "virgule", ".": "point", "?": "point d'interrogation", "!": "point d'exclamation", ":": "deux points",
       ";": "point virgule", "-": "tiret", "(": "parenthèse", ")": "parenthèse", "\"": "guillemet", "'": "apostrophe",
       "%": "pour cent", "$": "dollar", "€": "euro", "@": "arobase", "/": "barre oblique", "+": "plus", "=": "égal",
       "*": "étoile", "#": "dièse", "&": "et", "_": "souligné", "«": "guillemet", "»": "guillemet"}
def fsym(c):
    return q(c) if c.isalnum() or ord(c) > 127 else "%" + c
w("spell.foma", "define Letter " + " |\n".join(f"{q(k)}:{fq(v)}" for k, v in LETTER.items()) + " ;\n"
  "define Digit " + " | ".join(f"{d}:{fq(n)}" for d, n in zip("0123456789", DIG)) + " ;\n"
  'define Whitespace tab:"tabulation" | sp:"espace" ;\n'
  "define Punctuation " + " |\n".join(f"{fsym(k)}:{fq(v)}" for k, v in PUN.items()) + " ;\n"
  "regex Letter | Digit | Whitespace | Punctuation ;\n")
w("key.foma", "define D " + " | ".join(f"{d}:{fq(n)}" for d, n in zip("0123456789", DIG)) + " ;\n"
  'define K {enter}:"entrée" | {escape}:"échap" | {space}:"espace" | {tab}:"tabulation" | {backspace}:["retour" "arrière"] | '
  '{delete}:"supprimer" | {shift}:"majuscule" | {control}:"contrôle" | {alt}:"alt" ;\nregex D | K ;\n')
w("msg.foma", 'regex character:"caractère" %%d | capital:"majuscule" %%c ;\n')
w("lseq.foma", "source g2p.foma\ndefine Names [" + " | ".join(f"[{q(k)}:{{{v}}}]" for k, v in LETTER.items() if " " not in v and "'" not in v) +
  "] ;\nregex [Names .o. G2P]+ ;\n")
w("phrasing.tree", """((R:Word.n.name is 0)
 ((B))
 ((R:Token.n.name is 0)
  ((R:TokStructure.parent.parent.daughtern.name in ("," ":" ";" ")" "." "?" "!" "-" "–" "—" "»"))
   ((B))
   ((R:TokStructure.parent.parent.n.daughter1.name in ("(" "-" "–" "—" "«"))
    ((B))
    ((NB))))
  ((NB))))
""")

# ------------------------------------------------------------------ phonemes.xml
VF = {"i": ("h", "f", "-", "-"), "e": ("m", "f", "-", "-"), "E": ("m", "f", "-", "-"), "a": ("l", "c", "-", "-"),
      "O": ("m", "b", "+", "-"), "o": ("m", "b", "+", "-"), "u": ("h", "b", "+", "-"), "y": ("h", "f", "+", "-"),
      "eu": ("m", "f", "+", "-"), "oe": ("m", "f", "+", "-"), "ax": ("m", "c", "+", "-"),
      "en": ("m", "f", "-", "+"), "an": ("l", "b", "-", "+"), "on": ("m", "b", "+", "+")}
CF = {"p": ("lab", "s", "-"), "b": ("lab", "s", "+"), "t": ("alv", "s", "-"), "d": ("alv", "s", "+"),
      "k": ("vel", "s", "-"), "g": ("vel", "s", "+"), "f": ("lab", "f", "-"), "v": ("lab", "f", "+"),
      "s": ("alv", "f", "-"), "z": ("alv", "f", "+"), "sh": ("pal", "f", "-"), "zh": ("pal", "f", "+"),
      "m": ("lab", "n", "+"), "n": ("alv", "n", "+"), "gn": ("pal", "n", "+"), "ng": ("vel", "n", "+"),
      "l": ("alv", "l", "+"), "r": ("uvu", "r", "+"), "j": ("pal", "g", "+"), "w": ("lab", "g", "+"), "hw": ("pal", "g", "+")}
x = ['<?xml version="1.0" encoding="utf-8"?>', "<phonemes>"]
for v, (ht, fr, rd, na) in VF.items():
    x.append(f'<phoneme name="{v}" vc="+" vlng="s" vheight="{ht}" vfront="{fr}" vrnd="{rd}" vnas="{na}" cons="-"/>')
for c, (pl, ty, vo) in CF.items():
    x.append(f'<phoneme name="{c}" vc="-" cons="+" cplace="{pl}" ctype="{ty}" cvox="{vo}" emph="-"/>')
x.append('<phoneme name="pau" vc="-" cons="-"/>')
x.append("</phonemes>")
w("phonemes.xml", "\n".join(x) + "\n")
w("phones.txt", " ".join(PH) + "\n")
