from typing import Dict, List, Tuple

S_ALEF = 6
S_BEH = 7
S_TEH_MARBUTA = 8
S_TEH = 9
S_SEEN = 18
S_FEH = 26
S_KAF = 28
S_LAM = 29
S_MEEM = 30
S_NOON = 31
S_HEH = 32
S_WAW = 33
S_ALEF_MAKSURA = 34
S_YEH = 35
S_ALEF_WASLA = 36
S_ALEF_HAMZA_A = 2
S_ALEF_HAMZA_B = 4

SUN_LETTERS = {9, 10, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 29, 31}

PART_NONE = 0
PART_GEN = 1
PART_AN = 2
PART_NASB = 3
PART_JAZM = 4
PART_INNA = 5
PART_IN = 6
PART_KANA = 7
PART_CONJ = 8
PART_LA = 9
PART_SAWFA = 10
PART_QAD = 11
PART_YA = 12
PART_DEM = 13
PART_REL = 14
PART_PRON = 15
PART_QUEST = 16
PART_COND = 17
PART_IDAFA_NOUN = 18
PART_PREP_PRON = 19
PART_AMMA = 20
PART_LAKIN = 21
PART_AY = 22
PART_LAMA = 23
PART_LI_AN = 24
PART_NUM_IDAFA = 25
PART_HATTA = 26
PART_ILLA = 27

PARTICLES: List[Tuple[int, List[str]]] = [
    (PART_GEN, [
        "في", "من", "إلى", "الى", "على", "عن", "منذ", "مذ", "رب",
        "لفي", "لمن", "لعلى", "لعن", "بلا",
        "عند", "لدى", "لدي", "بين", "تحت", "فوق", "أمام", "امام", "خلف", "وراء", "حول",
        "دون", "مع", "بعد", "قبل", "نحو", "ضد", "عبر", "خلال", "طوال",
        "إثر", "اثر", "عقب", "لأجل", "بسبب", "تجاه", "إزاء", "حذاء", "وسط",
    ]),
    (PART_IDAFA_NOUN, [
        "غير", "كل", "بعض", "جميع", "كافة", "سوى", "معظم", "أغلب", "اغلب",
        "مثل", "شبه", "ذو", "ذي", "ذا", "ذات", "أول", "اول", "بن", "ابن", "بنت",
        "أبو", "أبي", "أبا", "أخو", "أخي", "أخا",
        "أثناء", "اثناء", "ضمن", "قرابة",
    ]),
    (PART_NUM_IDAFA, [
        "ثلاث", "ثلاثة", "أربع", "أربعة", "خمس", "خمسة", "ست", "ستة",
        "سبع", "سبعة", "ثمان", "ثماني", "ثمانية", "تسع", "تسعة", "عشر", "عشرة",
        "مئة", "مائة", "ألف", "آلاف",
    ]),
    (PART_AY, ["أي", "اي"]),
    (PART_LAMA, ["لما"]),
    (PART_LAKIN, ["لكن"]),
    (PART_LI_AN, ["لأن", "لان", "بأن", "بان", "كأن"]),
    (PART_PREP_PRON, [
        "به", "بها", "بهم", "بهما", "بهن", "بك", "بكم", "بكما", "بكن", "بنا", "بي",
        "فيه", "فيها", "فيهم", "فيهما", "فيهن", "فيك", "فيكم", "فيكما", "فيكن", "فينا",
        "عليه", "عليها", "عليهم", "عليهما", "عليهن", "عليك", "عليكم", "عليكما", "عليكن", "علينا", "علي",
        "منه", "منها", "منهم", "منهما", "منهن", "منك", "منكم", "منكما", "منكن", "منا", "مني",
        "إليه", "إليها", "إليهم", "إليهما", "إليهن", "إليك", "إليكم", "إليكما", "إليكن", "إلينا",
        "اليه", "اليها", "اليهم", "اليهما", "اليهن", "اليك", "اليكم", "الينا",
        "عنه", "عنها", "عنهم", "عنهما", "عنهن", "عنك", "عنكم", "عنا", "عني",
        "له", "لها", "لهم", "لهما", "لهن", "لك", "لكم", "لكما", "لكن", "لنا", "لي",
        "معه", "معها", "معهم", "معهما", "معهن", "معك", "معكم", "معنا",
        "عنده", "عندها", "عندهم", "عندهما", "عندهن", "عندك", "عندكم", "عندنا",
        "لديه", "لديها", "لديهم", "لديهما", "لديهن", "لديك", "لديكم", "لدينا",
        "بينه", "بينها", "بينهم", "بينهما", "بينهن", "بينكم", "بيننا",
        "فيهن", "عليهن", "منهن", "إليهن", "اليهن", "بهن",
    ]),
    (PART_HATTA, ["حتى"]),
    (PART_ILLA, ["إلا", "الا"]),
    (PART_AMMA, ["أما", "اما"]),
    (PART_AN, ["أن", "ان"]),
    (PART_NASB, ["لن", "كي", "لكي", "لئلا", "كيلا", "إذن", "اذن"]),
    (PART_JAZM, ["لم"]),
    (PART_INNA, ["ليت", "لعل", "كأنما"]),
    (PART_IN, ["إن"]),
    (PART_KANA, [
        "كان", "كانت", "كانوا", "يكون", "تكون", "أصبح", "اصبح", "أصبحت", "اصبحت",
        "أضحى", "اضحى", "أمسى", "امسى", "ظل", "ظلت", "بات", "صار", "صارت", "ليس",
        "ليست", "مازال", "لايزال", "يزال", "تزال", "زال", "مادام",
    ]),
    (PART_CONJ, ["و", "ف", "ثم", "أو", "او", "أم", "ام", "بل"]),
    (PART_LA, ["لا"]),
    (PART_SAWFA, ["سوف"]),
    (PART_QAD, ["قد", "لقد"]),
    (PART_YA, ["يا", "أيها", "ايها", "أيتها", "ايتها"]),
    (PART_DEM, [
        "هذا", "هذه", "ذلك", "كذلك", "تلك", "هؤلاء", "أولئك", "اولئك", "هذان", "هاتان",
        "هذين", "هاتين", "ذلكم", "هنا", "هناك",
    ]),
    (PART_REL, ["الذي", "التي", "الذين", "اللذان", "اللتان", "اللواتي", "اللاتي", "اللذين", "اللتين"]),
    (PART_PRON, ["هو", "هي", "هم", "هما", "هن", "أنا", "انا", "نحن", "أنت", "انت", "أنتم", "انتم", "أنتما", "انتما", "إياه", "إياها", "إياك", "إياهم", "إيانا"]),
    (PART_QUEST, ["ما", "ماذا", "هل", "كيف", "متى", "أين", "اين", "لماذا", "كم"]),
    (PART_COND, ["إذا", "اذا", "لو", "لولا", "كلما", "إذ", "اذ", "حين", "عندما", "حيث"]),
]


def _char_to_sym(ch: str) -> int:
    cp = ord(ch)
    if 0x0621 <= cp <= 0x063A:
        return cp - 0x0621
    if 0x0641 <= cp <= 0x064A:
        return 26 + (cp - 0x0641)
    if cp == 0x0671:
        return 36
    return 37


def _word_key(w: str) -> bytes:
    return bytes(_char_to_sym(c) for c in w)


PARTICLE_MAP: Dict[bytes, int] = {}
for _pid, _words in PARTICLES:
    for _w in _words:
        PARTICLE_MAP.setdefault(_word_key(_w), _pid)


def particle_id(syms: bytes) -> int:
    n = len(syms)
    if n == 0:
        return PART_NONE
    pid = PARTICLE_MAP.get(bytes(syms))
    if pid is not None:
        return pid
    if n >= 3 and syms[0] in (S_WAW, S_FEH):
        pid = PARTICLE_MAP.get(bytes(syms[1:]))
        if pid is not None and pid != PART_CONJ:
            return pid
    return PART_NONE


def analyze_surface(syms: bytes) -> dict:
    n = len(syms)
    info = {"al_len": 0, "has_prep": 0, "has_conj": 0, "verb_like": 0, "particle": particle_id(syms)}
    if info["particle"] == PART_NONE:
        found = False
        prefixes = [(), (S_BEH,), (S_KAF,), (S_WAW,), (S_FEH,),
                    (S_WAW, S_BEH), (S_WAW, S_KAF), (S_FEH, S_BEH), (S_FEH, S_KAF)]
        for pre in prefixes:
            pl = len(pre)
            min_stem = 2 if pl == 0 else 3
            if n < pl + 2 + min_stem:
                continue
            if tuple(syms[:pl]) != pre or syms[pl] not in (S_ALEF, S_ALEF_WASLA) or syms[pl + 1] != S_LAM:
                continue
            info["al_len"] = pl + 2
            info["has_prep"] = 1 if any(s in (S_BEH, S_KAF) for s in pre) else 0
            info["has_conj"] = 1 if any(s in (S_WAW, S_FEH) for s in pre) else 0
            found = True
            break
        if not found:
            for pre in [(), (S_WAW,), (S_FEH,)]:
                pl = len(pre)
                if n < pl + 2 + 3:
                    continue
                if tuple(syms[:pl]) != pre or syms[pl] != S_LAM or syms[pl + 1] != S_LAM:
                    continue
                info["al_len"] = pl + 2
                info["has_prep"] = 1
                info["has_conj"] = 1 if pl == 1 else 0
                break

    is_noun_ending = (syms[n - 1] == S_TEH_MARBUTA) or (n >= 3 and syms[n - 2] == S_YEH and syms[n - 1] == S_ALEF)
    if info["al_len"] == 0 and info["particle"] == PART_NONE and n >= 3 and not is_noun_ending:
        s = 0
        fut = False
        if n >= 4 and syms[0] in (S_WAW, S_FEH) and syms[1] in (S_YEH, S_TEH, S_NOON, S_SEEN):
            s = 1
        if syms[s] == S_SEEN and n - s >= 4 and syms[s + 1] in (S_YEH, S_TEH, S_NOON, S_ALEF_HAMZA_A):
            s += 1
            fut = True
        if fut:
            info["verb_like"] = 2
        elif syms[s] == S_YEH and n - s >= 3:
            info["verb_like"] = 2
        elif syms[s] in (S_TEH, S_NOON) and n - s >= 4:
            info["verb_like"] = 1
    return info


def generate_cpp_table(path: str) -> None:
    lines = ["static const ParticleEntry PARTICLE_TABLE[] = {"]
    for pid, words in PARTICLES:
        for w in words:
            lines.append(f'    {{"{w}", {pid}}},')
    lines.append("};")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
