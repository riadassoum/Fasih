"""Generate the foma sources for the Arabic RHVoice language.

Input to the engine is FULLY DIACRITIZED Arabic (Rawi runs upstream).
One extra convention from our preprocessing: a word-initial hamzat al-wasl that
is preceded by another word in the same phrase is written as U+0671 (alef wasla)
so it is dropped (connected speech); a plain bare alif at phrase start is read
with a glottal stop.

Phone set (ASCII, no trailing digits): see phonemes.xml.
Author: Riad Assoum project (written with Claude). License: LGPL-2.1+ like RHVoice.
"""
import os, sys, re

OUT = sys.argv[1] if len(sys.argv) > 1 else "src"
os.makedirs(OUT, exist_ok=True)

def q(ch):  # foma-quote a single symbol
    return '"' + ch + '"'

# --- letters ---------------------------------------------------------------
HAMZA, ALEF_MADDA, ALEF_HAMZA, WAW_HAMZA, ALEF_HAMZA_BELOW, YEH_HAMZA = "ءآأؤإئ"
ALEF, TEH_MARBUTA, ALEF_MAQSURA, WASLA, TATWEEL = "ا", "ة", "ى", "ٱ", "ـ"
WAW, YEH, LAM, HEH = "و", "ي", "ل", "ه"
FATHATAN, DAMMATAN, KASRATAN, FATHA, DAMMA, KASRA, SHADDA, SUKUN = (chr(c) for c in range(0x064B, 0x0653))
DAGGER = "ٰ"

CONS = {  # consonant letter -> phone
    "ب": "b", "ت": "t", "ث": "th", "ج": "j", "ح": "H", "خ": "x",
    "د": "d", "ذ": "dh", "ر": "r", "ز": "z", "س": "s", "ش": "sh",
    "ص": "S", "ض": "D", "ط": "T", "ظ": "Z", "ع": "E", "غ": "g",
    "ف": "f", "ق": "q", "ك": "k", LAM: "l", "م": "m", "ن": "n",
    HEH: "h", WAW: "w", YEH: "y",
    HAMZA: "Q", ALEF_HAMZA: "Q", WAW_HAMZA: "Q", ALEF_HAMZA_BELOW: "Q", YEH_HAMZA: "Q",
    "ڤ": "v", "پ": "p", "گ": "G", "چ": "ch",  # loan letters
}
SUN = ["ت", "ث", "د", "ذ", "ر", "ز", "س", "ش",
       "ص", "ض", "ط", "ظ", LAM, "ن"]
PREFIX = ["و", "ف", "ب", "ك"]  # wa- fa- bi- ka- before al-
SHORT = [FATHA, DAMMA, KASRA]
MARKS = [FATHATAN, DAMMATAN, KASRATAN, FATHA, DAMMA, KASRA, SHADDA, SUKUN, DAGGER]
ALL_LETTERS = list(CONS) + [ALEF, ALEF_MADDA, TEH_MARBUTA, ALEF_MAQSURA, WASLA]

def alt(xs):
    return "[" + " | ".join(q(x) for x in xs) + "]"

def w(name, text):
    text = re.sub(r"((?:define \w+)|regex)\n", r"\1 ", text)
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
        f.write(text)

# --- alphabet.foma (shared) -------------------------------------------------
w("alphabet.foma", f"""# Arabic alphabet definitions
define Mark {alt(MARKS)} ;
define ShortV {alt(SHORT)} ;
define Cons {alt(list(CONS))} ;
define SunCons {alt(SUN)} ;
define PrefixC {alt(PREFIX)} ;
define Letter {alt(ALL_LETTERS)} ;
define Grapheme Letter | Mark | {q(TATWEEL)} ;
""")

# --- g2p.foma ----------------------------------------------------------------
pairs_sh = " | ".join(f'[{q(c)} {q(SHADDA)}]:["{p}" "{p}"]' for c, p in CONS.items())
single = " | ".join(f'{q(c)}:"{p}"' for c, p in CONS.items())
F, D, K, SH, S0 = q(FATHA), q(DAMMA), q(KASRA), q(SHADDA), q(SUKUN)
g2p = f"""# Arabic grapheme-to-phoneme rules for fully diacritized text.
source alphabet.foma

# 0. tidy: drop tatweel, put shadda before its vowel mark
define Tidy [{q(TATWEEL)} -> 0] .o.
  [[{alt([FATHA, DAMMA, KASRA, FATHATAN, DAMMATAN, KASRATAN, DAGGER])} {SH}] @-> {SH} ... || _ ] ;
# the second rule above inserts a shadda before the vowel; remove the original trailing one
define DedupShadda [{SH} -> 0 || {SH} Mark _ ] ;

# 0b. hidden long vowels written without dagger alif (lexical), silent waw
define P [PrefixC ShortV] ;
define Lex
  [ [..] -> {K} || {q(ALEF_HAMZA_BELOW)} _ [\\Mark | .#.] ] .o.
  [ [..] -> {F} || {q(ALEF_HAMZA)} _ [\\Mark | .#.] ] .o.
  [ [..] -> {q(DAGGER)} || .#. (P) {q(HEH)} {F} _ ["\u0630" \\{SH} | "\u0624"] ] .o.
  [ [..] -> {q(DAGGER)} || .#. (P) {q(HEH)} {F} _ "\u0643" {F} "\u0630" ] .o.
  [ [..] -> {q(DAGGER)} || .#. (P) "\u0630" {F} _ {q(LAM)} {K} "\u0643" ] .o.
  [ [..] -> {q(DAGGER)} || .#. (P) "\u0643" {F} "\u0630" {F} _ {q(LAM)} {K} "\u0643" ] .o.
  [ [..] -> {q(DAGGER)} || .#. (P) {q(LAM)} {F} _ "\u0643" {K} "\u0646" ] .o.
  [ [..] -> {q(DAGGER)} || "\u0631" {SH} {F} "\u062D" {S0} "\u0645" {F} _ "\u0646" ] .o.
  [ [..] -> {q(DAGGER)} || .#. (P) {q(ALEF_HAMZA_BELOW)} ({K}) {q(LAM)} {F} _ {q(HEH)} ] .o.
  [ [..] -> {q(DAGGER)} || "\u0633" ({SH}) {F} "\u0645" {F} _ {q(WAW)} {F} {q(ALEF)} "\u062A" ] .o.
  [ [..] -> {q(DAGGER)} || .#. (P) {q(ALEF_HAMZA)} {D} {q(WAW)} {q(LAM)} {F} _ {q(YEH_HAMZA)} ] .o.
  [ [..] -> {q(WAW)} || "\u062F" {F} {q(ALEF)} {q(WAW)} {D} _ "\u062F" .#. ] .o.
  [ {q(WAW)} -> 0 || .#. (P) {q(ALEF_HAMZA)} {D} _ {q(LAM)} ShortV ({q(DAGGER)}) [{q(WAW)} | {q(YEH)} | {q(YEH_HAMZA)}] ] .o.
  [ {F} -> "aa" || {q(LAM)} {SH} _ {q(HEH)} {D} "\u0645" {SH} {F} .#. ] ;

# 1. hamzat al-wasl / definite article
define Wasl
  [[{q(ALEF)} ShortV] -> 0 || .#. PrefixC ShortV _ {q(LAM)} ] .o.
  [{q(WASLA)} -> 0 || .#. _ ] .o.
  [{q(WASLA)} -> 0 || .#. PrefixC ShortV _ ] .o.
  [{q(ALEF)} -> 0 || .#. PrefixC ShortV _ {q(LAM)} ] .o.
  [{q(ALEF)} -> "Q" || .#. _ Mark ] .o.
  [{q(ALEF)} -> ["Q" "a"] || .#. _ {q(LAM)} ] .o.
  [{q(ALEF)} -> ["Q" "i"] || .#. _ ] ;

# 2. sun letters: unmarked lam of the article before a doubled consonant is silent
define SunLam
  [{q(LAM)} -> 0 || [.#. "Q" "a" | .#. "Q" ShortV | .#. | .#. PrefixC ShortV | .#. {q(LAM)} {K}] _ Cons {SH} ] ;

# 3. the name of God: (al)lah / lillah -> l l aa h
define Allah [{F} -> "aa" || {q(LAM)} {SH} _ {q(HEH)} Mark* .#. ] ;

# 4. long vowels and silent letters
define LongV
  [{q(ALEF)} -> 0 || {q(FATHATAN)} _ ] .o.
  [{q(ALEF)} -> 0 || {q(FATHATAN)} {q(ALEF_MAQSURA)} _ ] .o.
  [{q(ALEF_MAQSURA)} -> 0 || {q(FATHATAN)} _ ] .o.
  [{q(ALEF)} -> 0 || [{D} {q(WAW)} | {q(WAW)} {S0}] _ .#. ] .o.
  [[{F} {q(ALEF)}] -> "aa" ] .o.
  [[{F} {q(ALEF_MAQSURA)}] -> "aa" ] .o.
  [[({F}) {q(DAGGER)}] @-> "aa" ] .o.
  [[{D} {q(WAW)}] -> "uu" || _ [\\[Mark] | .#.] ] .o.
  [[{K} {q(YEH)}] -> "ii" || _ [\\[Mark] | .#.] ] .o.
  [{q(ALEF_MADDA)} -> ["Q" "aa"] ] .o.
  [{q(ALEF)} -> "aa" ] .o.
  [{q(ALEF_MAQSURA)} -> "aa" ] ;

# 5. teh marbuta: t before a vowel, h at a pause
define TaaMarbuta [{q(TEH_MARBUTA)} -> "t" || _ Mark ] .o. [{q(TEH_MARBUTA)} -> "h"] ;

# 6. consonants (shadda doubles), then vowel marks
define Consonants [ {pairs_sh} ] @-> || _ ;
define Singles [ {single} ] @-> || _ ;
define Vowels
  [{F} -> "a"] .o. [{D} -> "u"] .o. [{K} -> "i"] .o.
  [{q(FATHATAN)} -> ["a" "n"]] .o. [{q(DAMMATAN)} -> ["u" "n"]] .o. [{q(KASRATAN)} -> ["i" "n"]] .o.
  [[{S0} | {SH}] -> 0] ;

define G2P Tidy .o. DedupShadda .o. Lex .o. Wasl .o. SunLam .o. Allah .o. LongV .o. TaaMarbuta
  .o. Consonants .o. Singles .o. Vowels ;
regex G2P ;
"""
w("g2p.foma", g2p)

exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'gen_rest.py'),encoding='utf-8').read())
print("written to", OUT)
