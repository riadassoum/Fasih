# Executed inside gen_foma.py (shares its names): syl, tok, downcase, gpos,
# numbers, spell, key, msg, lseq, phrasing, xml/config files.

VOW = ["a", "u", "i", "aa", "uu", "ii"]
PH = sorted(set(CONS.values()))
QV = " | ".join(f'"{v}"' for v in VOW)
QC = " | ".join(f'"{c}"' for c in PH)

# ---------------------------------------------------------------- syl.foma
w("syl.foma", f"""# Syllabification (single-consonant onsets) and Classical Arabic stress.
define V [{QV}] ;
define LV ["aa" | "uu" | "ii"] ;
define C [{QC}] ;
define MarkSyl [V C*] @> ... "." || _ C V ;
define Sx [C | V]+ ;
define Light (C) C V ;
define Heavy (C) C [LV | V C] ;
define Super (C) C [LV C+ | V C C+] ;
define NonSuper Light | Heavy ;
define B [.#. | "."] ;
define Stress
  [ [..] -> "_stress" || B Super _ .#. ] .o.
  [ [..] -> "_stress" || B Heavy _ "." NonSuper .#. ] .o.
  [ [..] -> "_stress" || B Sx _ "." Light "." NonSuper .#. ] .o.
  [ [..] -> "_stress" || .#. Light _ "." NonSuper .#. ] .o.
  [ [..] -> "_stress" || .#. NonSuper _ .#. ] ;
regex [C | V]+ .o. MarkSyl .o. Stress ;
""")

# ---------------------------------------------------------------- tok.foma
LATIN = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
AR_DIG = "٠١٢٣٤٥٦٧٨٩"
w("tok.foma", f"""source alphabet.foma
define NZDigit 1|2|3|4|5|6|7|8|9 ;
define Digit %0|NZDigit ;
define Number %0 | [NZDigit Digit*] ;
define Word Grapheme+ ;
define Stage1
Number @-> ... num || _ ,,
Word @-> ... word || _ .#. | \\Grapheme ,,
\\[Grapheme | Digit] @-> ... sym || _ ;
define Stage2 num -> dig || Digit^15 _ ;
regex Stage1 .o. Stage2 ;
""")

w("downcase.foma", "source alphabet.foma\nregex [Grapheme]* ;\n")

# ---------------------------------------------------------------- gpos.foma
# RHVoice applies gpos to the WORD SEQUENCE: each whole word is one input symbol,
# unknown words arrive as the identity symbol. So function words are listed as whole
# words, in the diacritized forms Rawi/ClArTTS use, plus plain and wasl spellings.
FUNC = {
 "prep": "فِي فِيهِ مِنْ مِنَ مِنِ إِلَى إلَى عَلَى عَنْ عَنِ مَعَ مَعْ بَيْنَ بَيْنِ عِنْدَ عِنْدِ حَتَّى مُنْذُ لَدَى نَحْوَ خِلَالَ بِ لِ كَ لَهُ لَهَا لَهُمْ بِهِ بِهَا عَلَيْهِ عَلَيْهَا عَلَيْهِمْ إِلَيْهِ مِنْهُ مِنْهَا عَنْهُ فِيهَا",
 "conj": "وَ فَ ثُمَّ أَوْ أَمْ بَلْ لَكِنْ لَكِنَّ إِنَّ أَنَّ أَنْ إِنْ كَيْ لِأَنَّ إِذَا إِذْ لَوْ لَوْلَا حِينَ بَيْنَمَا كَمَا لِكَيْ",
 "part": "لَا لَمْ لَنْ مَا قَدْ لَقَدْ سَوْفَ هَلْ أَ لَيْسَ لَيْسَتْ لَيْتَ لَعَلَّ إِنَّمَا أَلَا أَمَّا يَا قَدِ",
 "pron": "هُوَ هِيَ هُمْ هُنَّ هُمَا أَنَا نَحْنُ أَنْتَ أَنْتِ أَنْتُمْ أَنْتُنَّ أَنْتُمَا هَذَا هَذِهِ هَذَانِ هَاتَانِ هَؤُلَاءِ ذَلِكَ تِلْكَ أُولَئِكَ الَّذِي الَّتِي الَّذِينَ اللَّاتِي اللَّوَاتِي اللَّذَانِ اللَّتَانِ مَنْ",
}
import re as _re
def _variants(w):
    out = {w, _re.sub("[\u064B-\u0652\u0670]", "", w)}
    if w.startswith("\u0627"):
        out.add("\u0671" + w[1:])
    if _re.search("[\u064E\u064F\u0650]$", w):
        for v in "\u064E\u064F\u0650\u0652":
            out.add(w[:-1] + v)
    return out
_seen = {}
lines = []
for tag, words in FUNC.items():
    forms = set()
    for x in words.split():
        for v in _variants(x):
            if v not in _seen:
                _seen[v] = tag; forms.add(v)
    lines.append(f"define {tag.capitalize()} [" + " | ".join('"' + f + '"' for f in sorted(forms)) + "] ;")
w("gpos.foma", "\n".join(lines) + """
define FunctionWord Prep | Conj | Part | Pron ;
regex [ Prep:prep | Conj:conj | Part:part | Pron:pron | [? - FunctionWord]:content ]* ;
""")

# ---------------------------------------------------------------- names (diacritized)
LETTER_NAMES = {
 "ا": "أَلِف", "ب": "بَاء", "ت": "تَاء", "ث": "ثَاء", "ج": "جِيم", "ح": "حَاء",
 "خ": "خَاء", "د": "دَال", "ذ": "ذَال", "ر": "رَاء", "ز": "زَاي", "س": "سِين",
 "ش": "شِين", "ص": "صَاد", "ض": "ضَاد", "ط": "طَاء", "ظ": "ظَاء", "ع": "عَيْن",
 "غ": "غَيْن", "ف": "فَاء", "ق": "قَاف", "ك": "كَاف", "ل": "لَام", "م": "مِيم",
 "ن": "نُون", "ه": "هَاء", "و": "وَاو", "ي": "يَاء", "ء": "هَمْزَة",
 "أ": "أَلِف هَمْزَة", "إ": "أَلِف هَمْزَة", "آ": "أَلِف مَدّ", "ؤ": "وَاو هَمْزَة",
 "ئ": "يَاء هَمْزَة", "ة": "تَاء مَرْبُوطَة", "ى": "أَلِف مَقْصُورَة", "ٱ": "أَلِف وَصْل",
 FATHA: "فَتْحَة", DAMMA: "ضَمَّة", KASRA: "كَسْرَة", SUKUN: "سُكُون", SHADDA: "شَدَّة",
 FATHATAN: "تَنْوِين فَتْح", DAMMATAN: "تَنْوِين ضَمّ", KASRATAN: "تَنْوِين كَسْر", DAGGER: "أَلِف خَنْجَرِيَّة",
 TATWEEL: "تَطْوِيل",
}
DIGIT_NAMES = ["صِفْر", "وَاحِد", "اِثْنَان", "ثَلَاثَة", "أَرْبَعَة", "خَمْسَة", "سِتَّة", "سَبْعَة", "ثَمَانِيَة", "تِسْعَة"]
PUNCT = {",": "فَاصِلَة", "،": "فَاصِلَة", ".": "نُقْطَة", "?": "عَلَامَة اِسْتِفْهَام", "؟": "عَلَامَة اِسْتِفْهَام",
 "!": "عَلَامَة تَعَجُّب", ":": "نُقْطَتَان", ";": "فَاصِلَة مَنْقُوطَة", "؛": "فَاصِلَة مَنْقُوطَة",
 "-": "شَرْطَة", "(": "قَوْس", ")": "قَوْس", "\"": "عَلَامَة تَنْصِيص", "%": "بِالْمِئَة", "$": "دُولَار", "@": "آت",
 "/": "شَرْطَة مَائِلَة", "+": "زَائِد", "=": "يُسَاوِي", "*": "نَجْمَة", "#": "شَبَكَة", "&": "وَ"}

def fq(s):  # each output symbol is one whole word for RHVoice
    return "[" + " ".join('"' + x + '"' for x in s.split()) + "]"

def fsym(c):
    return "%" + c if not c.isalnum() else ('"' + c + '"')

spell_letters = " |\n".join(f'{q(k)}:{fq(v)}' for k, v in LETTER_NAMES.items())
spell_dig = " | ".join(f'{d}:{fq(n)}' for d, n in zip("0123456789", DIGIT_NAMES)) + " | " + \
            " | ".join(f'{q(d)}:{fq(n)}' for d, n in zip(AR_DIG, DIGIT_NAMES))
spell_p = " |\n".join(f'{fsym(k) if k not in chr(0x60C)+chr(0x61F)+chr(0x61B) else q(k)}:{fq(v)}' for k, v in PUNCT.items())
w("spell.foma", f"""define Letter {spell_letters} ;
define Digit {spell_dig} ;
define Whitespace tab:"تَبْوِيب" | sp:"مَسَافَة" ;
define Punctuation {spell_p} ;
regex Letter | Digit | Whitespace | Punctuation ;
""")

# numbers: read digit by digit (the NVDA add-on expands real numbers to words before RHVoice)
w("numbers.foma", f"""define D {" | ".join(f'{d}:{fq(n)}' for d, n in zip("0123456789", DIGIT_NAMES))} ;
regex D+ ;
""")
w("key.foma", f"""define D {" | ".join(f'{d}:{fq(n)}' for d, n in zip("0123456789", DIGIT_NAMES))} ;
define K {{enter}}:"إِدْخَال" | {{escape}}:"هُرُوب" | {{space}}:"مَسَافَة" | {{tab}}:"تَبْوِيب" | {{backspace}}:["حَذْف" "خَلْفِي"] | {{delete}}:"حَذْف" | {{shift}}:"شِفْت" | {{control}}:"كُنْتْرُول" | {{alt}}:"أَلْت" ;
regex D | K ;
""")
w("msg.foma", 'regex character:"حَرْف" %%d | capital:"كَبِير" %%c ;\n')

# lseq: spell each letter by name, straight to phones
lseq_parts = " | ".join(f'[{q(k)}:{{{v}}} ]' for k, v in LETTER_NAMES.items() if " " not in v)
w("lseq.foma", f"""source g2p.foma
define Names [{lseq_parts}] ;
regex [Names .o. G2P]+ ;
""")

w("phrasing.tree", """((R:Word.n.name is 0)
 ((B))
 ((R:Token.n.name is 0)
  ((R:TokStructure.parent.parent.daughtern.name in ("," "،" ":" ";" "؛" ")" "." "?" "؟" "!" "-" "–" "—"))
   ((B))
   ((R:TokStructure.parent.parent.n.daughter1.name in ("(" "-" "–" "—"))
    ((B))
    ((NB))))
  ((NB))))
""")

# ---------------------------------------------------------------- phonemes.xml
# features: vc, vlng (s/l), cplace, ctype, cvox, emph (pharyngealized), guttural
VF = {"a": ("s", "l", "c"), "i": ("s", "h", "f"), "u": ("s", "h", "b"),
      "aa": ("l", "l", "c"), "ii": ("l", "h", "f"), "uu": ("l", "h", "b")}
CF = {  # place, type, voiced, emphatic
 "b": ("lab", "s", "+", "-"), "t": ("alv", "s", "-", "-"), "th": ("den", "f", "-", "-"), "j": ("pal", "a", "+", "-"),
 "H": ("phar", "f", "-", "-"), "x": ("uvu", "f", "-", "-"), "d": ("alv", "s", "+", "-"), "dh": ("den", "f", "+", "-"),
 "r": ("alv", "r", "+", "-"), "z": ("alv", "f", "+", "-"), "s": ("alv", "f", "-", "-"), "sh": ("pal", "f", "-", "-"),
 "S": ("alv", "f", "-", "+"), "D": ("alv", "s", "+", "+"), "T": ("alv", "s", "-", "+"), "Z": ("den", "f", "+", "+"),
 "E": ("phar", "f", "+", "-"), "g": ("uvu", "f", "+", "-"), "f": ("lab", "f", "-", "-"), "q": ("uvu", "s", "-", "+"),
 "k": ("vel", "s", "-", "-"), "l": ("alv", "l", "+", "-"), "m": ("lab", "n", "+", "-"), "n": ("alv", "n", "+", "-"),
 "h": ("glo", "f", "-", "-"), "w": ("lab", "g", "+", "-"), "y": ("pal", "g", "+", "-"), "Q": ("glo", "s", "-", "-"),
 "v": ("lab", "f", "+", "-"), "p": ("lab", "s", "-", "-"), "G": ("vel", "s", "+", "-"), "ch": ("pal", "a", "-", "-"),
}
x = ['<?xml version="1.0" encoding="utf-8"?>', "<phonemes>"]
for v, (ln, ht, fr) in VF.items():
    x.append(f'<phoneme name="{v}" vc="+" vlng="{ln}" vheight="{ht}" vfront="{fr}" cons="-"/>')
for c, (pl, ty, vo, em) in CF.items():
    x.append(f'<phoneme name="{c}" vc="-" cons="+" cplace="{pl}" ctype="{ty}" cvox="{vo}" emph="{em}"/>')
x.append('<phoneme name="pau" vc="-" cons="-"/>')
x.append("</phonemes>")
w("phonemes.xml", "\n".join(x) + "\n")
