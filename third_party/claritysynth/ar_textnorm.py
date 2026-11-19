# -*- coding: utf-8 -*-
"""Modern-Standard-Arabic text normalisation for ClaritySynth.

Turns raw written text into fully *spoken* Arabic so the diacritizer and
neural voice receive words, not digits and glyphs. This is what lets the
synth read a real newspaper or book the way a person would:

    "في 25/7/2026 ارتفعت الأسعار 15% إلى 1500$"
    -> "في الخامس والعشرين من يوليو ألفين وستة وعشرين ارتفعت الأسعار
        خمسة عشر بالمئة إلى ألف وخمسمائة دولار"

Everything here is pure-Python (num2words + a little logic) and runs before
diacritization. It is deliberately conservative: anything it is unsure about
is left untouched for the existing pipeline to handle, and the whole module
fails soft — if a helper library is missing, the original text is returned
unchanged and the synth behaves exactly as it did before.

The single public entry point is ``normalize(text, opts)`` where ``opts`` is a
mapping of feature switches (see ``DEFAULTS``). ClaritySynth calls it from
``ar_g2p.clean_arabic_text`` so both the neural and formant voices benefit.
"""

import re

# --- optional heavy-lifting libraries (vendored, pure-python) --------------
# Import lazily and defensively: a failure here must never break speech.
try:
    try:
        from .lib_pure.num2words import num2words as _n2w
    except Exception:
        import os as _os, sys as _sys
        _lp = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)),
                            "lib_pure")
        if _lp not in _sys.path:
            _sys.path.insert(0, _lp)
        from num2words import num2words as _n2w

    def _num2words(n, **kw):
        try:
            return _n2w(n, lang="ar", **kw)
        except Exception:
            return None
except Exception:
    def _num2words(n, **kw):
        return None

try:
    try:
        from .lib_pure.hijridate import Gregorian as _Gregorian
    except Exception:
        import os as _os, sys as _sys
        _lp = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)),
                            "lib_pure")
        if _lp not in _sys.path:
            _sys.path.insert(0, _lp)
        from hijridate import Gregorian as _Gregorian
except Exception:
    _Gregorian = None


# ---------------------------------------------------------------------------
# Feature switches. ClaritySynth maps these to NVDA settings; every one can be
# turned off independently so a user who dislikes, say, Hijri dates can keep
# the rest.
# ---------------------------------------------------------------------------
DEFAULTS = {
    "numbers": True,        # 2026 -> ألفان وستة وعشرون
    "ordinals": True,       # الـ21 -> الحادي والعشرون
    "dates": True,          # 25/7/2026 -> الخامس والعشرين من يوليو...
    "hijri": False,         # also append the Hijri equivalent of a date
    "times": True,          # 14:30 -> الساعة الثانية والنصف (24h aware)
    "currency": True,       # 1500$ / 20 ر.س -> ... دولار / ... ريال
    "percent": True,        # 15% -> خمسة عشر بالمئة
    "phone": True,          # long digit runs read digit-by-digit
    "symbols": True,        # & @ + = ... spoken as Arabic words
    "abbrev": True,         # د. -> دكتور ، ص -> صفحة (context aware)
    "math": True,           # 3+2 -> ثلاثة زائد اثنان
    "roman": True,          # القرن XXI -> الحادي والعشرون
}


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
_ARABIC_INDIC = "٠١٢٣٤٥٦٧٨٩"
_EASTERN = "۰۱۲۳۴۵۶۷۸۹"
_ASCII = "0123456789"
_DIGIT_XLAT = {ord(a): d for a, d in zip(_ARABIC_INDIC, _ASCII)}
_DIGIT_XLAT.update({ord(a): d for a, d in zip(_EASTERN, _ASCII)})


def _to_ascii_digits(s):
    """Fold Arabic-Indic / eastern digits to ASCII so one code path handles
    all three. Spoken output is Arabic regardless of the input glyphs."""
    return s.translate(_DIGIT_XLAT)


def _spell_digits(digits):
    """Read a run of digits one by one (phone numbers, codes, IDs)."""
    names = {"0": "صفر", "1": "واحد", "2": "اثنان", "3": "ثلاثة",
             "4": "أربعة", "5": "خمسة", "6": "ستة", "7": "سبعة",
             "8": "ثمانية", "9": "تسعة"}
    return " ".join(names.get(c, c) for c in digits)


# ---------------------------------------------------------------------------
# Arabic cardinals, written for Fasih: gender agreement, case, joined waw.
# ---------------------------------------------------------------------------
_U_M = ["صفر", "واحد", "اثنان", "ثلاثة", "أربعة", "خمسة", "ستة", "سبعة", "ثمانية", "تسعة", "عشرة"]
_U_F = ["صفر", "واحدة", "اثنتان", "ثلاث", "أربع", "خمس", "ست", "سبع", "ثماني", "تسع", "عشر"]
_TENS = {2: "عشرون", 3: "ثلاثون", 4: "أربعون", 5: "خمسون", 6: "ستون", 7: "سبعون", 8: "ثمانون", 9: "تسعون"}
_HUND = {1: "مئة", 2: "مئتان", 3: "ثلاثمئة", 4: "أربعمئة", 5: "خمسمئة", 6: "ستمئة", 7: "سبعمئة", 8: "ثمانمئة", 9: "تسعمئة"}
# singular, dual, plural (3-10), accusative singular (11-99)
_SCALES = [(10 ** 12, ("تريليون", "تريليونان", "تريليونات", "تريليونا")),
           (10 ** 9, ("مليار", "ملياران", "مليارات", "مليارا")),
           (10 ** 6, ("مليون", "مليونان", "ملايين", "مليونا")),
           (10 ** 3, ("ألف", "ألفان", "آلاف", "ألفا"))]


def _below100(n, fem):
    if n <= 10:
        return (_U_F if fem else _U_M)[n]
    if n < 20:
        u = n - 10
        if u == 1:
            return "إحدى عشرة" if fem else "أحد عشر"
        if u == 2:
            return "اثنتا عشرة" if fem else "اثنا عشر"
        return (_U_F[u] + " عشرة") if fem else (_U_M[u] + " عشر")
    t, u = divmod(n, 10)
    if not u:
        return _TENS[t]
    if u == 1:
        unit = "إحدى" if fem else "واحد"
    else:
        unit = (_U_F if fem else _U_M)[u]
    return unit + " و" + _TENS[t]


def _below1000(n, fem):
    h, r = divmod(n, 100)
    parts = []
    if h:
        parts.append(_HUND[h])
    if r or not h:
        parts.append(_below100(r, fem))
    return " و".join(parts)


def _ar_cardinal(n, fem=False, gen=False):
    """MSA cardinal. fem: the counted noun is feminine (affects the last
    group only). gen: genitive/accusative forms (عشرين، اثنين، ألفين)."""
    n = int(n)
    if n == 0:
        return "صفر"
    neg = n < 0
    n = abs(n)
    parts = []
    for size, (one, two, few, acc) in _SCALES:
        m, n = divmod(n, size)
        if not m:
            continue
        if m == 1:
            parts.append(one)
        elif m == 2:
            parts.append(two)
        else:
            r = m % 100
            word = few if 3 <= r <= 10 else (acc if r >= 11 else one)
            parts.append(_below1000(m, False) + " " + word)
    if n:
        parts.append(_below1000(n, fem))
    out = " و".join(parts)
    if gen:
        out = _genitive(out)
    return ("سالب " + out) if neg else out


def _genitive(s):
    w = s.split(" ")
    rep = {"اثنان": "اثنين", "اثنتان": "اثنتين", "اثنا": "اثني", "اثنتا": "اثنتي",
           "مئتان": "مئتين", "ألفان": "ألفين", "مليونان": "مليونين",
           "ملياران": "مليارين", "تريليونان": "تريليونين"}
    out = []
    for x in w:
        pre = "و" if x.startswith("و") and x[1:] in rep or (x.startswith("و") and x[1:] in _TENS.values()) else ""
        core = x[len(pre):]
        if core in rep:
            core = rep[core]
        elif core in _TENS.values():
            core = core[:-2] + "ين"
        out.append(pre + core)
    return " ".join(out)


# Plurals in -ات whose singular is masculine (ثلاثة ملفات), and broken
# plurals whose singular is feminine (ثلاث دقائق).
_MASC_AT = set("""ملفات حسابات مطارات فيديوهات مستشفيات حيوانات مشروعات منتجات مسلسلات
مليارات مستويات سجلات مجلدات مؤتمرات مكونات محركات متغيرات مخططات قطارات جنيهات دولارات
كيلومترات سنتيمترات مترات بايتات ميغابايتات جيجابايتات ميجابايتات تيرابايتات مقررات
مهرجانات معسكرات مختبرات مسدسات موتورات تلفزيونات تليفونات باصات أوتوبيسات بنطلونات
إعلانات حمامات نباتات ملحقات مرفقات مقترحات مواعيد""".split())
_FEM_BROKEN = set("""دقائق رسائل مدن دول نساء ليال ليالي جوائز وسائل قصص صور غرف نقاط
قطع مدارس قرى سور أعين أيد أرجل حروب شمس أنفس نفوس أرض أراض أراضي طرق مرات سنين
سيوف حقائب جزر ذخائر ركائز فرائض شرائح خرائط حدائق كنائس مساجد بطاقات""".split())


def _noun_gender(word, n):
    """Guess whether the noun counted by n is feminine."""
    w = re.sub("[ً-ْـ]", "", word or "")
    w = re.sub("^(?:ال)", "", w)
    if not w:
        return False
    if w in _MASC_AT:
        return False
    if w in _FEM_BROKEN:
        return True
    if w.endswith("ات") and len(w) > 3:
        # تفعيلات / افتعالات / استفعالات are verbal nouns, masculine
        if (w.startswith("ت") and len(w) >= 7) or re.match("^(?:است|ا.ت)", w):
            return False
        return True
    r = n % 100
    if w.endswith("ة"):
        # 3-10 take a plural; a plural in ة (طلبة، أساتذة) is masculine
        return not (3 <= r <= 10)
    return False


_GEN_BEFORE = set("""في من إلى الى على عن حتى منذ مذ بعد قبل خلال حوالي حوالى نحو لمدة مدة
عام سنة العام السنة بين أكثر أقل أي كل بحوالي بنحو وفي ومن وإلى وعلى وعن وبعد وقبل وخلال وحوالي
فمن ففي لكل بكل""".split())

_ORD_M = ["", "الأول", "الثاني", "الثالث", "الرابع", "الخامس", "السادس", "السابع", "الثامن", "التاسع", "العاشر"]
_ORD_F = ["", "الأولى", "الثانية", "الثالثة", "الرابعة", "الخامسة", "السادسة", "السابعة", "الثامنة", "التاسعة", "العاشرة"]


def _ordinal_def(n, fem=False):
    """الأول ... التاسع والتسعون (definite), otherwise ال + cardinal."""
    n = int(n)
    tbl = _ORD_F if fem else _ORD_M
    if 1 <= n <= 10:
        return tbl[n]
    if n < 20:
        if n == 11:
            return "الحادية عشرة" if fem else "الحادي عشر"
        u = tbl[n - 10]
        return u + (" عشرة" if fem else " عشر")
    if n < 100:
        t, u = divmod(n, 10)
        tens = "ال" + _TENS[t]
        if not u:
            return tens
        unit = ("الحادية" if fem else "الحادي") if u == 1 else tbl[u]
        return unit + " و" + tens
    if n == 100:
        return "المئة"
    return "ال" + _ar_cardinal(n)


def _cardinal(n, fem=False, gen=False):
    try:
        return _ar_cardinal(int(n), fem, gen)
    except Exception:
        return str(n)


def _ordinal(n, feminine=False):
    """Plain ordinal (num2words gives masculine; light feminine touch)."""
    w = _num2words(int(n), to="ordinal")
    if not w:
        return str(n)
    if feminine and w and not w.endswith("ة"):
        fem = {"أول": "أولى", "ثاني": "ثانية", "ثالث": "ثالثة",
               "رابع": "رابعة", "خامس": "خامسة", "سادس": "سادسة",
               "سابع": "سابعة", "ثامن": "ثامنة", "تاسع": "تاسعة",
               "عاشر": "عاشرة"}
        return fem.get(w, w)
    return w


def _ordinal_definite(n, feminine=True):
    """Definite ordinal for dates: 25 -> الخامس والعشرون.

    num2words gives the cardinal-style compound for >10 ('واحد وعشرون'); for
    dates Arabic uses an ordinal on the units joined to the tens, all made
    definite. We build 1..10 from a table and compose the tens.
    """
    units_m = ["", "الأول", "الثاني", "الثالث", "الرابع", "الخامس",
               "السادس", "السابع", "الثامن", "التاسع", "العاشر"]
    units_f = ["", "الأولى", "الثانية", "الثالثة", "الرابعة", "الخامسة",
               "السادسة", "السابعة", "الثامنة", "التاسعة", "العاشرة"]
    tens = {20: "العشرون", 30: "الثلاثون"}
    tbl = units_f if feminine else units_m
    n = int(n)
    if 1 <= n <= 10:
        return tbl[n]
    if n in (11, 12):
        if feminine:
            return "الحادية عشرة" if n == 11 else "الثانية عشرة"
        return "الحادي عشر" if n == 11 else "الثاني عشر"
    if 13 <= n <= 19:
        u = tbl[n - 10].replace("ال", "", 1)
        return ("ال%s عشر" % u) if not feminine else ("ال%s عشرة" % u)
    if n in (20, 30):
        return tens[n]
    if 21 <= n <= 39:
        u = tbl[n % 10]
        t10 = tens[(n // 10) * 10]
        return "%s و%s" % (u, t10)
    # fall back to plain ordinal
    return _ordinal(n, feminine=feminine)
    # num2words 'ordinal' gives masculine; a light feminine touch for dates.
    w = _num2words(int(n), to="ordinal")
    if not w:
        return str(n)
    if feminine and w and not w.endswith("ة"):
        # naive but effective for the 1..10 range that dates use
        fem = {"أول": "أولى", "ثاني": "ثانية", "ثالث": "ثالثة",
               "رابع": "رابعة", "خامس": "خامسة", "سادس": "سادسة",
               "سابع": "سابعة", "ثامن": "ثامنة", "تاسع": "تاسعة",
               "عاشر": "عاشرة"}
        return fem.get(w, w)
    return w


# Gregorian month names as spoken across most of the Arab world (Levant/Gulf
# use these borrowed forms in everyday MSA; the classic Levantine names are
# offered as an alternate table for future dialect work).
_MONTHS = ["", "يناير", "فبراير", "مارس", "أبريل", "مايو", "يونيو",
           "يوليو", "أغسطس", "سبتمبر", "أكتوبر", "نوفمبر", "ديسمبر"]

_HIJRI_MONTHS = ["", "محرم", "صفر", "ربيع الأول", "ربيع الآخر",
                 "جمادى الأولى", "جمادى الآخرة", "رجب", "شعبان",
                 "رمضان", "شوال", "ذو القعدة", "ذو الحجة"]

_WEEKDAYS = ["الإثنين", "الثلاثاء", "الأربعاء", "الخميس",
             "الجمعة", "السبت", "الأحد"]


# ---------------------------------------------------------------------------
# Currency and symbol tables
# ---------------------------------------------------------------------------
_CURRENCY = {
    "$": "دولار", "USD": "دولار", "£": "جنيه", "GBP": "جنيه",
    "€": "يورو", "EUR": "يورو", "¥": "ين",
    "ر.س": "ريال", "ريال": "ريال", "SAR": "ريال",
    "د.إ": "درهم", "درهم": "درهم", "AED": "درهم",
    "ج.م": "جنيه مصري", "د.ك": "دينار كويتي", "د.ع": "دينار عراقي",
    "ل.ل": "ليرة لبنانية", "ل.س": "ليرة سورية", "د.أ": "دينار أردني",
    "ر.ق": "ريال قطري", "د.ب": "دينار بحريني", "ر.ع": "ريال عماني",
}

_SYMBOLS = {
    "&": "و", "@": "آت", "#": "رقم", "%": "بالمئة", "‰": "بالألف",
    "+": "زائد", "=": "يساوي", "≠": "لا يساوي", "<": "أصغر من",
    ">": "أكبر من", "≤": "أصغر أو يساوي", "≥": "أكبر أو يساوي",
    "±": "زائد أو ناقص", "×": "ضرب", "÷": "قسمة", "√": "جذر",
    "∞": "ما لا نهاية", "π": "باي", "°": "درجة", "µ": "ميكرو",
    "©": "حقوق محفوظة", "®": "علامة مسجلة", "™": "علامة تجارية",
    "§": "فقرة", "†": "خنجر", "•": "نقطة", "★": "نجمة", "☆": "نجمة",
    "→": "يؤدي إلى", "←": "من", "↔": "ذهابا وإيابا", "⇒": "إذن",
}

# Abbreviations that are safe to expand in MSA. These are expanded ONLY when
# they appear as a complete token (bounded by non-letters), and the dotted
# forms only when the dot is actually present — otherwise short forms would
# match the START of ordinary words (e.g. "مج" inside "مجلة", "مجلس",
# "مجموعة"). Bare-letter forms that are common word prefixes are deliberately
# NOT listed, because they cannot be told apart from a normal word without a
# terminating dot.
_ABBREV = {
    "أ.د": "أستاذ دكتور", "ص.ب": "صندوق بريد",
    "ق.م": "قبل الميلاد", "ب.م": "بعد الميلاد", "إلخ": "إلى آخره",
    "ا.هـ": "انتهى",
    "د.": "دكتور", "م.": "مهندس", "أ.": "أستاذ",
    "ت.": "توفي", "ولد.": "ولد",
}

_ROMAN = [("XXI", 21), ("XIX", 19), ("XVIII", 18), ("XVII", 17),
          ("XVI", 16), ("XIV", 14), ("XIII", 13), ("XII", 12),
          ("XI", 11), ("VIII", 8), ("VII", 7), ("VI", 6),
          ("XX", 20), ("XV", 15), ("IX", 9), ("IV", 4),
          ("X", 10), ("V", 5), ("III", 3), ("II", 2), ("I", 1)]


# ---------------------------------------------------------------------------
# Individual transformers. Each takes the working string and returns it.
# Order matters: dates/times/currency must run before the bare-number pass so
# their digits are consumed with the right meaning.
# ---------------------------------------------------------------------------
def _x_dates(t, opts):
    if not opts.get("dates"):
        return t

    def repl(m):
        d, mo, y = int(m.group(1)), int(m.group(2)), m.group(3)
        if not (1 <= d <= 31 and 1 <= mo <= 12):
            return m.group(0)
        y = int(y)
        if y < 100:                       # 25/7/26 -> 2026 heuristic
            y += 2000 if y < 50 else 1900
        out = "%s من %s %s" % (_ordinal_def(d), _MONTHS[mo],
                               _cardinal(y, gen=True))
        if opts.get("hijri") and _Gregorian is not None:
            try:
                h = _Gregorian(y, mo, d).to_hijri()
                out += " الموافق %s من %s %s هجري" % (
                    _ordinal_definite(h.day),
                    _HIJRI_MONTHS[h.month], _cardinal(h.year))
            except Exception:
                pass
        return out

    # 2 مايو 2024 / 15 كانون الثاني -> الثاني من مايو ألفين وأربعة وعشرين
    names = "|".join(sorted(set(_MONTHS[1:] + _HIJRI_MONTHS[1:] + [
        "أبريل", "ابريل", "إبريل", "اغسطس", "اكتوبر", "أيار", "حزيران", "تموز", "آب", "أيلول",
        "تشرين الأول", "تشرين الثاني", "كانون الأول", "كانون الثاني", "شباط", "آذار", "نيسان",
        "جانفي", "فيفري", "أفريل", "ماي", "جوان", "جويلية", "أوت"]), key=len, reverse=True))
    t = re.sub(r"(?<!\d)(\d{1,2})\s+(" + names + r")(?![\u0621-\u064A])(?:\s+(\d{3,4})(?!\d))?",
               lambda m: "%s من %s%s" % (_ordinal_def(int(m.group(1))), m.group(2),
                                        (" " + _cardinal(m.group(3), gen=True)) if m.group(3) else "")
               if 1 <= int(m.group(1)) <= 31 else m.group(0), t)
    # d/m/y or d-m-y with 1-2 digit day/month and 2-4 digit year
    t = re.sub(r"\b(\d{1,2})[/\-](\d{1,2})[/\-](\d{2,4})\b", repl, t)
    return t


def _x_times(t, opts):
    if not opts.get("times"):
        return t
    period_words = r"(?:صباحا|صباحًا|صباحاً|مساء|مساءً|مساءا|ظهرا|ظهرًا|ليلا|ليلًا|فجرا|فجرًا|عصرا|عصرًا|ص|م)(?![\u0621-\u064A])"

    def repl(m):
        h, mi = int(m.group(1)), int(m.group(2))
        if not (0 <= h <= 23 and 0 <= mi <= 59):
            return m.group(0)
        given = m.group(3)
        if h == 0:
            hh, period = 12, "منتصف الليل"
        elif h < 12:
            hh, period = h, "صباحا"
        elif h == 12:
            hh, period = 12, "ظهرا"
        else:
            hh, period = h - 12, "مساء"
        if given:
            g = given.strip()
            period = {"ص": "صباحا", "م": "مساء"}.get(g, g)
        base = "الساعة %s" % _ordinal_def(hh, fem=True)
        if mi == 15:
            base += " والربع"
        elif mi == 30:
            base += " والنصف"
        elif mi == 45:
            base += " إلا ربعا"
        elif mi == 1:
            base += " ودقيقة"
        elif mi == 2:
            base += " ودقيقتان"
        elif mi:
            base += " و%s %s" % (_cardinal(mi, fem=True), "دقائق" if 3 <= mi <= 10 else "دقيقة")
        return base + " " + period

    t = re.sub(r"(?:ال)?ساعة\s+(?=\d{1,2}:\d{2})", "", t)
    return re.sub(r"(?<![\d:])(\d{1,2}):(\d{2})(?![\d:])(\s*" + period_words + r")?", repl, t)


def _x_currency(t, opts):
    if not opts.get("currency"):
        return t
    # symbol-suffixed or symbol-prefixed amounts: 1500$  $1500  20 ر.س
    keys = sorted(_CURRENCY.keys(), key=len, reverse=True)
    for sym in keys:
        esym = re.escape(sym)
        # amount then symbol
        t = re.sub(r"(\d+(?:\.\d+)?)\s*" + esym,
                   lambda m: _amount_words(m.group(1), _CURRENCY[sym]), t)
        # symbol then amount (mostly $, £, €)
        if sym in ("$", "£", "€", "¥"):
            t = re.sub(esym + r"\s*(\d+(?:\.\d+)?)",
                       lambda m: _amount_words(m.group(1), _CURRENCY[sym]), t)
    return t


def _amount_words(num, unit):
    if "." in num:
        whole, frac = num.split(".", 1)
        w = _cardinal(whole)
        # read fractional part as sub-unit count (piastres/fils/cents feel)
        if frac and int(frac):
            return "%s %s و%s" % (w, unit, _cardinal(frac.ljust(2, "0")[:2]))
        return "%s %s" % (w, unit)
    return "%s %s" % (_cardinal(num), unit)


def _x_percent(t, opts):
    if not opts.get("percent"):
        return t
    t = re.sub(r"(\d+(?:\.\d+)?)\s*%",
               lambda m: _cardinal_dec(m.group(1)) + " بالمئة", t)
    t = re.sub(r"(\d+(?:\.\d+)?)\s*‰",
               lambda m: _cardinal_dec(m.group(1)) + " بالألف", t)
    return t


def _cardinal_dec(num):
    if "." in num:
        whole, frac = num.split(".", 1)
        return "%s فاصلة %s" % (_cardinal(whole), _spell_digits(frac))
    return _cardinal(num)


def _x_math(t, opts):
    if not opts.get("math"):
        return t
    # only between digits, so we never touch a hyphen inside words
    t = re.sub(r"(?<=\d)\s*\+\s*(?=\d)", " زائد ", t)
    t = re.sub(r"(?<=\d)\s*[-−]\s*(?=\d)", " ناقص ", t)
    t = re.sub(r"(?<=\d)\s*[×x]\s*(?=\d)", " ضرب ", t)
    t = re.sub(r"(?<=\d)\s*÷\s*(?=\d)", " قسمة ", t)
    fr = {(1, 2): "نصف", (1, 3): "ثلث", (2, 3): "ثلثان", (1, 4): "ربع", (3, 4): "ثلاثة أرباع",
          (1, 5): "خمس", (1, 8): "ثمن", (1, 10): "عشر"}
    t = re.sub(r"(?<![\d/])(\d)/(\d{1,2})(?![\d/])",
               lambda m: fr.get((int(m.group(1)), int(m.group(2))), m.group(0)), t)
    t = re.sub(r"(?<=\d)\s*/\s*(?=\d)", " على ", t)
    t = re.sub(r"(?<=\d)\s*=\s*", " يساوي ", t)
    return t


def _x_phone(t, opts):
    if not opts.get("phone"):
        return t
    # 7+ digit runs (optionally +country) read digit by digit
    def repl(m):
        s = m.group(0)
        plus = s.startswith("+")
        digits = re.sub(r"\D", "", s)
        out = _spell_digits(digits)
        return ("زائد " + out) if plus else out
    return re.sub(r"\+?\d[\d\s\-]{6,}\d", repl, t)


def _x_ordinals(t, opts):
    if not opts.get("ordinals"):
        return t

    def repl(m):
        fem = bool(m.group(1)) and m.group(1).rstrip().endswith("ة")
        return (m.group(1) or "") + _ordinal_def(int(m.group(2)), fem)
    # الـ21 / الـ 21 / ال21 -> definite ordinal, feminine after a feminine noun
    return re.sub(r"([\u0621-\u064A]+\s+)?(?<![\u0621-\u064A])ال(?:ـ\s*)?(\d{1,3})(?!\d)", repl, t)


def _x_roman(t, opts):
    if not opts.get("roman"):
        return t
    # only uppercase standalone tokens, to avoid hitting English words

    def repl(m):
        tok = m.group(0)
        for r, v in _ROMAN:
            if tok == r:
                return _ordinal(v)
        return tok
    return re.sub(r"\b(?=[IVX]+\b)[IVX]{1,5}\b", repl, t)


def _x_numbers(t, opts):
    if not opts.get("numbers"):
        return t
    # versions and addresses: 2.5.1 -> اثنان نقطة خمسة نقطة واحد
    t = re.sub(r"(?<![\d.])\d+(?:\.\d+){2,}(?![\d.])",
               lambda m: " نقطة ".join(_cardinal(x) if not (len(x) > 1 and x[0] == "0") else _spell_digits(x)
                                       for x in m.group(0).split(".")), t)
    # simple fractions
    fr = {(1, 2): "نصف", (1, 3): "ثلث", (2, 3): "ثلثان", (1, 4): "ربع", (3, 4): "ثلاثة أرباع",
          (1, 5): "خمس", (1, 8): "ثمن", (1, 10): "عشر"}
    t = re.sub(r"(?<![\d/])(\d)/(\d{1,2})(?![\d/])",
               lambda m: fr.get((int(m.group(1)), int(m.group(2))), m.group(0)), t)
    t = re.sub(r"\d+\.\d+", lambda m: _cardinal_dec(m.group(0)), t)

    def _int_repl(m):
        s = m.group(2)
        before, after = m.group(1) or "", m.group(3) or ""
        # A number with a leading zero (06, 007) is a code, not a cardinal;
        # a long run (phone numbers, IDs) is read digit by digit.
        if (len(s) > 1 and s[0] == "0") or len(s) >= 10:
            return before + _spell_digits(s) + after
        prev = re.findall("[\u0621-\u064A]+", before)
        gen = bool(prev) and prev[-1] in _GEN_BEFORE
        noun = re.match(r"\s*([\u0621-\u064A\u064B-\u0652]+)", after)
        # one and two follow their noun (كتاب واحد), so the next word says nothing
        fem = _noun_gender(noun.group(1), int(s)) if noun and int(s) % 100 not in (1, 2) else False
        return before + _cardinal(s, fem=fem, gen=gen) + after

    # look at the word before (case) and the word after (gender)
    t = re.sub(r"((?:^|[\u0621-\u064A]+)\s*)?(?<!\d)(\d+)(?!\d)(\s*[\u0621-\u064A\u064B-\u0652]*)", _int_repl, t)
    return t


def _x_symbols(t, opts):
    if not opts.get("symbols"):
        return t
    for sym, word in _SYMBOLS.items():
        if sym in t:
            t = t.replace(sym, " %s " % word)
    return t


def _x_abbrev(t, opts):
    if not opts.get("abbrev"):
        return t
    # Anything that can be part of a word: letters, harakat, tatweel, digits,
    # Persian/Urdu letters and presentation forms. An abbreviation must stand
    # alone: whitespace, punctuation or the edge of the text on both sides.
    word = r"[\u0610-\u061A\u0620-\u065F\u0660-\u0669\u0670-\u06D3\u06D5-\u06ED\u06FA-\u06FF\uFB50-\uFDFF\uFE70-\uFEFFA-Za-z0-9_]"
    # years: "632م" / "1990 م." / "1445هـ" -> ميلادي / هجري
    t = re.sub(r"(?<![\d:])(\d{3,4})\s*م(?=\.?(?:$|[\s.,،؛:!?؟)\]»]))", r"\1 ميلادي", t)
    t = re.sub(r"(?<![\d:])(\d{3,4})\s*(?:هـ|ه)(?=\.?(?:$|[\s.,،؛:!?؟)\]»]))", r"\1 هجري", t)
    for ab, full in sorted(_ABBREV.items(), key=lambda kv: -len(kv[0])):
        esc = re.escape(ab)
        if ab.endswith(".") and len(ab) == 2:
            # a one-letter title (د. م. أ. ت.) only before a following word,
            # never at the end of a sentence where it is just a word's last letter
            pattern = r"(?<!" + word + r")" + esc + r"(?=\s*[\u0621-\u064A])"
        else:
            pattern = r"(?<!" + word + r")" + esc + r"(?!" + word + r")"
        t = re.sub(pattern, full + " ", t)
    return re.sub(r" +(?=[.,،؛:!?؟])", "", t)


# ---------------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------------
def normalize(text, opts=None):
    """Expand numbers, dates, symbols, etc. in *text* to spoken MSA.

    Fails soft: on any error the original text is returned unchanged. ``opts``
    overrides individual switches from ``DEFAULTS``.
    """
    if not text or not text.strip():
        return text
    o = dict(DEFAULTS)
    if opts:
        o.update({k: v for k, v in opts.items() if k in DEFAULTS})
    try:
        t = _to_ascii_digits(text)
        t = t.replace("\u066B", ".").replace("\u066C", ",")
        # 1,250,000 -> 1250000 (groups of exactly three digits only)
        t = re.sub(r"(?<![\d,])\d{1,3}(?:,\d{3})+(?![\d,]|\.\d)", lambda m: m.group(0).replace(",", ""), t)
        # -5 at the start of a number (not a range like 3-5)
        t = re.sub(r"(?:(?<=^)|(?<=[\s(:=]))[-−](?=\d)", "سالب ", t)
        # ص 45 / ج 2 / ع 3 before a number
        t = re.sub(r"(?<![\u0600-\u06FF\w])(ص|ج|ع|ط)\.?\s*(\d{1,4})(?!\d)",
                   lambda m: {"ص": "صفحة", "ج": "الجزء", "ع": "العدد", "ط": "الطبعة"}[m.group(1)] + " " + _cardinal(m.group(2)), t)
        # "و 11" -> "و11": the waw belongs to the number
        t = re.sub(r"(?<![\u0621-\u064A])و\s+(?=\d)", "و", t)
        # 3-5 is a range
        t = re.sub(r"(?<=\d)[-–](?=\d)(?!\d{1,2}[-/]\d)", " إلى ", t)
        # Order is important — see module docstring.
        t = _x_abbrev(t, o)
        t = _x_dates(t, o)
        t = _x_times(t, o)
        t = _x_currency(t, o)
        t = _x_percent(t, o)
        t = _x_math(t, o)
        t = _x_roman(t, o)
        t = _x_ordinals(t, o)
        t = _x_phone(t, o)
        t = _x_numbers(t, o)
        t = _x_symbols(t, o)
        t = re.sub(r"\s{2,}", " ", t).strip()
        return t or text
    except Exception:
        return text


# convenience for callers that only want the digit-fold
def fold_digits(text):
    return _to_ascii_digits(text) if text else text
