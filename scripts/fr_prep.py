"""French text front end for the Fasih French voice (training and the NVDA add-on).

RHVoice reads one word at a time, so the liaison (les‿amis, c'est‿un, mon‿ami) is written
here: the consonant heard at the start of the next word is put before it as ‿z, ‿t, ‿n, ‿r
or ‿p, which the French g2p reads as that sound. Common abbreviations are expanded.
Author: Riad Assoum project (written with Claude). License: GPL-3.0 like Fasih."""
import re

LIAISON = "‿"
VOWEL_START = set("aàâæeéèêëiîïoôöuùûüyœ")
# words starting with an aspirated h (no liaison, no elision) and others that refuse liaison
NO_LIAISON_NEXT = {
    "haricot", "haricots", "héros", "hache", "haches", "haine", "hasard", "haut", "haute", "hauts", "hautes",
    "honte", "hors", "huit", "huitième", "hibou", "hiboux", "hall", "halte", "hamac", "hameau", "hanche",
    "handicap", "hangar", "hareng", "harpe", "hausse", "hérisson", "hêtre", "hockey", "homard", "hongrois",
    "hollande", "hublot", "huer", "hurler", "hutte", "onze", "onzième", "oui", "ouistiti", "yaourt", "yoga",
    "yacht", "et", "ou", "où",
}
# word -> liaison consonant (the common, obligatory or near-obligatory ones)
LINK = {}
for w in ("les des ces mes tes ses nos vos leurs aux quels quelles plusieurs certains certaines "
          "nous vous ils elles deux trois six dix dans chez sans sous très plus moins gros mauvais "
          "quelques autres mêmes").split():
    LINK[w] = "z"
for w in "un aucun mon ton son on en bien rien bon".split():
    LINK[w] = "n"
for w in "est sont petit grand tout quand dont vingt cent font vont doit peut fait étaient était".split():
    LINK[w] = "t"
LINK.update({"premier": "r", "dernier": "r", "trop": "p", "beaucoup": "p"})

# plural adjectives and determiners before a noun, and a few more linking words
for w in ("petits petites grands grandes bons bonnes beaux belles nouveaux nouvelles vieux vieilles anciens "
          "anciennes premiers premières derniers dernières différents différentes divers diverses tels telles "
          "aucuns mes vos nos ses tes ces leurs aux quels quelles lesquels lesquelles desquels auxquels "
          "assez jamais toujours après depuis").split():
    LINK.setdefault(w, "z")
for w in "comment fort vont avaient seront serait sont ont".split():
    LINK.setdefault(w, "t")
LINK["ont"] = "t"

ABBR = [(r"\bM\.\s", "Monsieur "), (r"\bMM\.\s", "Messieurs "), (r"\bMme\b\.?", "Madame"),
        (r"\bMlle\b\.?", "Mademoiselle"), (r"\bDr\b\.?", "Docteur"), (r"\bSt\b\.?", "Saint"),
        (r"\betc\.", "et cetera"), (r"\bp\.\s?ex\.", "par exemple"), (r"\bn°\s?", "numéro "),
        (r"(\d)\s?%", r"\1 pour cent"), (r"(\d)\s?€", r"\1 euros"), (r"(\d)\s?\$", r"\1 dollars")]
_L = "A-Za-zÀ-ÖØ-öø-ÿŒœ"
_TOKEN = re.compile("‿?[" + _L + "]+(?:'[" + _L + "]+)*'?|[^\\s" + _L + "‿]+|\\s+")

# ------------------------------------------------------------------ numbers
_U = ["zéro", "un", "deux", "trois", "quatre", "cinq", "six", "sept", "huit", "neuf", "dix", "onze", "douze",
      "treize", "quatorze", "quinze", "seize"]
_T = {2: "vingt", 3: "trente", 4: "quarante", 5: "cinquante", 6: "soixante"}
# feminine nouns often counted: "vingt et une heures", "une fois"
FEM = set("heure heures fois minute minutes seconde secondes semaine semaines année années personne personnes "
          "page pages fille filles femme femmes voix chose choses journée journées place places".split())


def _b100(n):
    if n <= 16: return _U[n]
    if n < 20: return "dix-" + _U[n - 10]
    t, u = divmod(n, 10)
    if t in _T: return _T[t] + ("" if u == 0 else (" et un" if u == 1 else "-" + _U[u]))
    if t == 7: return "soixante" + (" et onze" if u == 1 else "-" + _b100(10 + u))
    if t == 8: return "quatre-vingt" + ("s" if u == 0 else "-" + _U[u])
    return "quatre-vingt-" + _b100(10 + u)


def _b1000(n, plural=True):
    h, r = divmod(n, 100)
    out = []
    if h: out.append("cent" if h == 1 else _U[h] + " cent" + ("s" if r == 0 and plural else ""))
    if r or not h: out.append(_b100(r))
    t = " ".join(out)
    return t[:-1] if (not plural and t.endswith("vingts")) else t


def number_words(n):
    if n == 0: return "zéro"
    parts = []
    for div, one, many in ((10 ** 9, "un milliard", "milliards"), (10 ** 6, "un million", "millions"), (1000, "mille", "mille")):
        q, n = divmod(n, div)
        if q: parts.append(one if q == 1 else _b1000(q, False) + " " + many)
    if n: parts.append(_b1000(n))
    return " ".join(parts)


def _ordinal(n):
    if n == 1: return "premier"
    w = number_words(n)
    if w.endswith("cinq"): return w + "uième"
    if w.endswith("neuf"): return w[:-1] + "vième"
    if w.endswith("e"): w = w[:-1]
    if w.endswith("vingts") or w.endswith("cents"): w = w[:-1]
    if w.endswith(" et un") or w == "un": return w[:-2] + "unième"
    return w + "ième"


def _numbers(text):
    text = re.sub(r"\b(\d{1,2})\s?h\s?(\d{2})\b", lambda m: f"{m.group(1)} heures {m.group(2)}", text)
    text = re.sub(r"\b1(er|re|ère)\b", lambda m: "premier" if m.group(1) == "er" else "première", text)
    text = re.sub(r"\b(\d+)(e|ème|eme)\b", lambda m: _ordinal(int(m.group(1))), text)
    text = re.sub(r"\b(\d+),(\d+)\b", lambda m: f"{number_words(int(m.group(1)))} virgule {number_words(int(m.group(2)))}" if len(m.group(1)) < 13 else m.group(0), text)

    def card(m):
        n = m.group(1)
        if len(n) > 12: return n
        w = number_words(int(n))
        nxt = text[m.end():].lstrip().split(" ")[0].lower().strip(".,;:!?") if m.end() < len(text) else ""
        if nxt in FEM and (w == "un" or w.endswith(" un")):
            w += "e"
        return w
    return re.sub(r"\b(\d+)\b", card, text)


def _tail(word):
    # c'est -> est, qu'ils -> ils ; a liaison prefix already written is ignored
    if word.startswith(LIAISON): word = word[2:]
    return word.lower().split("'")[-1]


_PRON = {"il", "ils", "elle", "elles", "on", "en", "y", "je", "nous", "vous"}


def prepare(text):
    text = text.replace("’", "'").replace("ʼ", "'")
    for pat, rep in ABBR:
        text = re.sub(pat, rep, text)
    # numbers with a thousands space or dot (1 000, 1.000) are joined, then read in words
    text = re.sub(r"(?<=\d)[   .](?=\d{3}\b)", "", text)
    text = _numbers(text)
    toks = _TOKEN.findall(text)
    for i, t in enumerate(toks):
        if not (t[:1].isalpha() or t.startswith(LIAISON)) or i + 2 >= len(toks):
            continue
        sep, nxt = toks[i + 1], toks[i + 2]
        hyphen = sep == "-"
        if not ((sep.isspace() and "\n" not in sep) or hyphen) or not nxt[:1].isalpha():
            continue
        w, low = _tail(t), nxt.lower()
        c = LINK.get(w)
        if hyphen:
            # inversion: dit-il, vont-ils, a-t-elle (already written), allons-y, prends-en
            if low in _PRON and low not in ("je", "nous", "vous"):
                c = "t" if w[-1:] in "td" else ("z" if w[-1:] in "sxz" else None)
            elif w == "vingt" and low == "et":
                c = "t"
            elif low.startswith(("et", "un", "huit", "onze")) and w in ("vingt", "trente", "quarante", "cinquante", "soixante"):
                c = None
            elif w not in ("deux", "trois", "six", "dix", "vingt", "cent") or low.split("'")[0] in NO_LIAISON_NEXT:
                c = None
        if w == "vingt" and low == "et":
            c = "t"
        if w == "neuf" and low in ("heures", "ans", "hommes"):
            toks[i] = t[:-1] + "ve"          # neuf‿heures: the f itself becomes v (read as neuve)
            continue
        if not c or (low.split("'")[0] in NO_LIAISON_NEXT and not (w == "vingt" and low == "et")):
            continue
        if low[0] in VOWEL_START or (low[0] == "h" and len(low) > 1):
            toks[i + 2] = LIAISON + c + nxt
    return "".join(toks)


if __name__ == "__main__":
    import sys
    for line in sys.stdin:
        print(prepare(line.rstrip("\n")))
