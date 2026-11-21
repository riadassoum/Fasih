import re
from typing import List, Tuple

# Diacritic IDs matching cpp/include/tashkeel/format.hpp
DIAC_NONE = 0
DIAC_FATHA = 1
DIAC_DAMMA = 2
DIAC_KASRA = 3
DIAC_SUKUN = 4
DIAC_FATHATAN = 5
DIAC_DAMMATAN = 6
DIAC_KASRATAN = 7
DIAC_SHADDA = 8
DIAC_SHADDA_FATHA = 9
DIAC_SHADDA_DAMMA = 10
DIAC_SHADDA_KASRA = 11
DIAC_SHADDA_FATHATAN = 12
DIAC_SHADDA_DAMMATAN = 13
DIAC_SHADDA_KASRATAN = 14
NUM_DIACRITICS = 15

# MorphFlags matching format.hpp
MORPH_NONE = 0
MORPH_HAS_AL = 1 << 0
MORPH_PREP = 1 << 1
MORPH_NASB = 1 << 2
MORPH_JAZM = 1 << 3
MORPH_INNA = 1 << 4
MORPH_KANA = 1 << 5
MORPH_CONJ = 1 << 6
MORPH_VERB_LIKE = 1 << 7

DIAC_CHARS = {
    "\u064e": "FATHA",
    "\u064f": "DAMMA",
    "\u0650": "KASRA",
    "\u0652": "SUKUN",
    "\u064b": "FATHATAN",
    "\u064c": "DAMMATAN",
    "\u064d": "KASRATAN",
    "\u0651": "SHADDA",
}

DIAC_ID_TO_STR = {
    DIAC_NONE: "",
    DIAC_FATHA: "\u064e",
    DIAC_DAMMA: "\u064f",
    DIAC_KASRA: "\u0650",
    DIAC_SUKUN: "\u0652",
    DIAC_FATHATAN: "\u064b",
    DIAC_DAMMATAN: "\u064c",
    DIAC_KASRATAN: "\u064d",
    DIAC_SHADDA: "\u0651",
    DIAC_SHADDA_FATHA: "\u0651\u064e",
    DIAC_SHADDA_DAMMA: "\u0651\u064f",
    DIAC_SHADDA_KASRA: "\u0651\u0650",
    DIAC_SHADDA_FATHATAN: "\u0651\u064b",
    DIAC_SHADDA_DAMMATAN: "\u0651\u064c",
    DIAC_SHADDA_KASRATAN: "\u0651\u064d",
}

PREP_WORDS = {
    "في", "من", "إلى", "الى", "عن", "على", "حتى", "منذ", "مذ", "خلا", "عدا", "حاشا",
    "مع", "عند", "لدى", "بين", "تحت", "فوق", "أمام", "امام", "خلف", "وراء", "حول", "دون", "ضد", "نحو",
}
NASB_WORDS = {"أن", "ان", "لن", "كي", "لكي", "إذن", "اذن", "حتى"}
JAZM_WORDS = {"لم", "لما", "لام"}
INNA_WORDS = {"إن", "ان", "أن", "كأن", "لكن", "ليت", "لعل"}
KANA_WORDS = {
    "كان", "كانت", "كانوا", "يكون", "تكون", "أصبح", "اصبح", "أمسى", "امسى",
    "أضحى", "اضحى", "ظل", "بات", "صار", "ليس", "ليست", "مازال", "لايزال",
}
CONJ_WORDS = {"و", "أو", "او", "ثم", "بل", "لكن", "أم", "ام", "ف"}


def is_arabic_letter(ch: str) -> bool:
    cp = ord(ch)
    return (0x0621 <= cp <= 0x063A) or (0x0641 <= cp <= 0x064A) or cp == 0x0671


def is_arabic_diacritic(ch: str) -> bool:
    cp = ord(ch)
    return (0x064B <= cp <= 0x0652) or cp == 0x0670


def char_to_sym(ch: str) -> int:
    cp = ord(ch)
    if 0x0621 <= cp <= 0x063A:
        return cp - 0x0621
    if 0x0641 <= cp <= 0x064A:
        return 26 + (cp - 0x0641)
    if cp == 0x0671:
        return 36
    return 37


def combine_diacritics(marks: List[str]) -> int:
    has_shadda = "\u0651" in marks
    vowel = None
    for m in marks:
        if m != "\u0651" and m in DIAC_CHARS:
            vowel = m
    if has_shadda:
        if vowel == "\u064e":
            return DIAC_SHADDA_FATHA
        if vowel == "\u064f":
            return DIAC_SHADDA_DAMMA
        if vowel == "\u0650":
            return DIAC_SHADDA_KASRA
        if vowel == "\u064b":
            return DIAC_SHADDA_FATHATAN
        if vowel == "\u064c":
            return DIAC_SHADDA_DAMMATAN
        if vowel == "\u064d":
            return DIAC_SHADDA_KASRATAN
        return DIAC_SHADDA
    if vowel == "\u064e":
        return DIAC_FATHA
    if vowel == "\u064f":
        return DIAC_DAMMA
    if vowel == "\u0650":
        return DIAC_KASRA
    if vowel == "\u0652":
        return DIAC_SUKUN
    if vowel == "\u064b":
        return DIAC_FATHATAN
    if vowel == "\u064c":
        return DIAC_DAMMATAN
    if vowel == "\u064d":
        return DIAC_KASRATAN
    return DIAC_NONE


def parse_diacritized_word(word: str) -> Tuple[str, bytes, List[int]]:
    """Returns (undiac_word, sym_bytes, diacritic_ids_per_letter)."""
    letters = []
    syms = []
    diacs = []
    i = 0
    n = len(word)
    while i < n:
        ch = word[i]
        if is_arabic_letter(ch):
            letters.append(ch)
            syms.append(char_to_sym(ch))
            marks = []
            i += 1
            while i < n and (is_arabic_diacritic(word[i]) or word[i] == "\u0640"):
                if is_arabic_diacritic(word[i]):
                    marks.append(word[i])
                i += 1
            diacs.append(combine_diacritics(marks))
        else:
            i += 1
    return "".join(letters), bytes(syms), diacs


_AR_WORD_RE = re.compile(r"[\u0621-\u063A\u0641-\u064A\u0671\u064B-\u0652\u0670\u0640]+")
_SYM_MAP = {chr(cp): cp - 0x0621 for cp in range(0x0621, 0x063B)}
_SYM_MAP.update({chr(cp): 26 + (cp - 0x0641) for cp in range(0x0641, 0x064B)})
_SYM_MAP["\u0671"] = 36
_DIAC_SET = {"\u064b", "\u064c", "\u064d", "\u064e", "\u064f", "\u0650", "\u0651", "\u0652", "\u0670"}
_DIAC_LUT = {
    "": 0, "\u064e": 1, "\u064f": 2, "\u0650": 3, "\u0652": 4,
    "\u064b": 5, "\u064c": 6, "\u064d": 7, "\u0651": 8,
    "\u0651\u064e": 9, "\u064e\u0651": 9,
    "\u0651\u064f": 10, "\u064f\u0651": 10,
    "\u0651\u0650": 11, "\u0650\u0651": 11,
    "\u0651\u064b": 12, "\u064b\u0651": 12,
    "\u0651\u064c": 13, "\u064c\u0651": 13,
    "\u0651\u064d": 14, "\u064d\u0651": 14,
}


def extract_arabic_words(line: str) -> List[Tuple[str, bytes, List[int]]]:
    """Splits a diacritized Arabic line into parsed Arabic word tuples."""
    words = []
    for m in _AR_WORD_RE.finditer(line):
        w = m.group(0)
        letters = []
        syms = []
        diacs = []
        i = 0
        n = len(w)
        while i < n:
            ch = w[i]
            s = _SYM_MAP.get(ch)
            if s is not None:
                letters.append(ch)
                syms.append(s)
                i += 1
                if i < n and w[i] not in _SYM_MAP:
                    j = i + 1
                    while j < n and w[j] not in _SYM_MAP:
                        j += 1
                    seg = w[i:j]
                    d = _DIAC_LUT.get(seg)
                    if d is None:
                        d = combine_diacritics([c for c in seg if c in _DIAC_SET])
                    diacs.append(d)
                    i = j
                else:
                    diacs.append(0)
            else:
                i += 1
        if letters:
            words.append(("".join(letters), bytes(syms), diacs))
    return words


def infer_word_flags(undiac: str, syms: bytes = None) -> int:
    from grammar import (
        PART_AN, PART_CONJ, PART_IN, PART_INNA, PART_JAZM, PART_KANA,
        PART_LAKIN, PART_LAMA, PART_LI_AN, PART_NASB, PART_GEN,
        analyze_surface,
    )
    if syms is None:
        syms = bytes(char_to_sym(c) for c in undiac)
    si = analyze_surface(syms)
    pid = si["particle"]
    flags = MORPH_NONE
    if si["al_len"] > 0:
        flags |= MORPH_HAS_AL
    if pid == PART_GEN:
        flags |= MORPH_PREP
    elif pid in (PART_AN, PART_NASB):
        flags |= MORPH_NASB
    elif pid in (PART_JAZM, PART_LAMA):
        flags |= MORPH_JAZM
    elif pid in (PART_INNA, PART_LAKIN, PART_LI_AN):
        flags |= MORPH_INNA
    elif pid == PART_KANA:
        flags |= MORPH_KANA
    elif pid == PART_CONJ:
        flags |= MORPH_CONJ
    if si["verb_like"] == 2 and not (flags & MORPH_HAS_AL):
        flags |= MORPH_VERB_LIKE
    return flags


def morph_to_role(flags: int) -> int:
    if flags & MORPH_PREP:
        return 1
    if flags & MORPH_NASB:
        return 2
    if flags & MORPH_JAZM:
        return 3
    if flags & MORPH_INNA:
        return 4
    if flags & MORPH_KANA:
        return 5
    if flags & MORPH_CONJ:
        return 6
    if flags & MORPH_HAS_AL:
        return 7
    if flags & MORPH_VERB_LIKE:
        return 8
    return 0


def strip_diacritics(text: str) -> str:
    return "".join(ch for ch in text if not is_arabic_diacritic(ch) and ch != "\u0640")


def compute_metrics(gold_lines: List[str], pred_lines: List[str]) -> dict:
    """Computes Character-level DER, Word-level WER, and Case-Ending (I'rab) Error Rate."""
    total_chars = 0
    err_chars = 0
    total_words = 0
    err_words = 0
    total_endings = 0
    err_endings = 0

    for g_line, p_line in zip(gold_lines, pred_lines):
        g_words = extract_arabic_words(g_line)
        p_words = extract_arabic_words(p_line)
        for (gu, _, gd), (pu, _, pd) in zip(g_words, p_words):
            if gu != pu or len(gd) != len(pd):
                continue
            total_words += 1
            if gd != pd:
                err_words += 1
            for g_d, p_d in zip(gd, pd):
                total_chars += 1
                if g_d != p_d:
                    err_chars += 1
            if len(gd) >= 1:
                total_endings += 1
                if gd[-1] != pd[-1]:
                    err_endings += 1

    return {
        "total_words": total_words,
        "total_chars": total_chars,
        "der_pct": (100.0 * err_chars / total_chars) if total_chars else 0.0,
        "wer_pct": (100.0 * err_words / total_words) if total_words else 0.0,
        "irab_err_pct": (100.0 * err_endings / total_endings) if total_endings else 0.0,
        "char_acc_pct": (100.0 * (total_chars - err_chars) / total_chars) if total_chars else 0.0,
        "word_acc_pct": (100.0 * (total_words - err_words) / total_words) if total_words else 0.0,
        "irab_acc_pct": (100.0 * (total_endings - err_endings) / total_endings) if total_endings else 0.0,
    }
