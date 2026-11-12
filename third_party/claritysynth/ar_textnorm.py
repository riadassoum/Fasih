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


def _cardinal(n):
    w = _num2words(int(n))
    return w if w else str(n)


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
        out = "%s من %s %s" % (_ordinal_definite(d), _MONTHS[mo],
                               _cardinal(y))
        if opts.get("hijri") and _Gregorian is not None:
            try:
                h = _Gregorian(y, mo, d).to_hijri()
                out += " الموافق %s من %s %s هجري" % (
                    _ordinal_definite(h.day),
                    _HIJRI_MONTHS[h.month], _cardinal(h.year))
            except Exception:
                pass
        return out

    # d/m/y or d-m-y with 1-2 digit day/month and 2-4 digit year
    t = re.sub(r"\b(\d{1,2})[/\-](\d{1,2})[/\-](\d{2,4})\b", repl, t)
    return t


def _x_times(t, opts):
    if not opts.get("times"):
        return t

    def repl(m):
        h, mi = int(m.group(1)), int(m.group(2))
        if not (0 <= h <= 23 and 0 <= mi <= 59):
            return m.group(0)
        period = ""
        if h == 0:
            hh, period = 12, "منتصف الليل"
        elif h < 12:
            hh, period = h, "صباحا"
        elif h == 12:
            hh, period = 12, "ظهرا"
        else:
            hh, period = h - 12, "مساء"
        base = "الساعة %s" % _ordinal_definite(hh)
        if mi == 0:
            pass
        elif mi == 15:
            base += " والربع"
        elif mi == 30:
            base += " والنصف"
        elif mi == 45:
            base += " إلا ربعا"
        else:
            base += " و%s دقيقة" % _cardinal(mi)
        return base + " " + period

    # remove an immediately preceding "الساعة" so we do not say it twice
    t = re.sub(r"الساعة\s+(?=\d{1,2}:\d{2})", "", t)
    return re.sub(r"(?<!\d)(\d{1,2}):(\d{2})(?!\d)", repl, t)


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
    t = re.sub(r"(?<=\d)\s*[÷/]\s*(?=\d)", " قسمة ", t)
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
    # الـ21 / الـ 21  -> definite ordinal (ـ is Arabic tatweel, not \b-safe)
    t = re.sub(r"الـ\s*(\d{1,3})",
               lambda m: "ال" + _ordinal(int(m.group(1))), t)
    return t


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
    # remaining bare integers / decimals
    t = re.sub(r"\d+\.\d+", lambda m: _cardinal_dec(m.group(0)), t)

    def _int_repl(m):
        s = m.group(0)
        # A number with a leading zero (06, 007) is a code/time/ID, not a
        # cardinal — int("06") would silently drop the zero and read "six".
        # Read every digit by name instead: "صفر ستة". A bare "0" stays the
        # word صفر. Long digit runs (>= 7, e.g. phone numbers) are also read
        # digit-by-digit, which is how people expect them spoken.
        if (len(s) > 1 and s[0] == "0") or len(s) >= 7:
            return _spell_digits(s)
        return _cardinal(s)

    t = re.sub(r"\d+", _int_repl, t)
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
    # An Arabic letter that could continue a word. If the character right
    # after the abbreviation is one of these, it is NOT an abbreviation but
    # the start of a longer word (e.g. "م." inside "م.ك" is fine, but the
    # bare form must never eat into a real word), so we require a non-letter
    # (space, punctuation, digit, end of string) to follow.
    ar_letter = r"[\u0621-\u064A\u0670\u0671]"
    for ab, full in sorted(_ABBREV.items(), key=lambda kv: -len(kv[0])):
        esc = re.escape(ab)
        # left boundary: not preceded by an Arabic letter (so we don't match
        # the tail of a word); right boundary: not followed by an Arabic
        # letter (so "مج" can never turn "مجلة" into "مجلدلة"). A trailing dot
        # in the abbreviation already supplies part of that boundary.
        pattern = r"(?<!" + ar_letter + r")" + esc + r"(?!" + ar_letter + r")"
        t = re.sub(pattern, full, t)
    return t


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
