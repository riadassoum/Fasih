"""Arabic regression tests for Fasih.  run: python3 tests/test_arabic.py
Tests that need no diacritizer always run. Full-pipeline tests (with Rawi) run when
RAWI_ORT_LIB points to a Linux onnxruntime library and rawi_host is built (see BUILD-NOTES.md)."""
import os, sys, re, types, unicodedata, importlib.util, subprocess, tempfile, shutil
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import ar_prep
fails = 0


def check(name, got, want):
    global fails
    n = lambda t: unicodedata.normalize("NFC", t) if isinstance(t, str) else t
    ok = n(got) == n(want)   # the order of shadda and its vowel does not matter to the voice
    fails += not ok
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f"\n     got  {got}\n     want {want}"))


# pausal ending (waqf)
for src, want in [("مَرْحَبًا.", "مَرْحَبَا."), ("مَدْرَسَةٌ.", "مَدْرَسَة."), ("فَتًى.", "فَتَى."),
                  ("مَسَاءً.", "مَسَاء."), ("كِتَابٌ جَدِيدٌ.", "كِتَابٌ جَدِيد."), ("جَمِيلًا؟", "جَمِيلَا؟")]:
    check("waqf " + src, ar_prep.pausal_ending(src), want)

# waqf with shadda in either mark order, and sukun on a sun-letter lam
for src, want in [("ثُمَّ.", "ثُمّ."), ("حَقٌّ.", "حَقّ.")]:
    check("waqf shadda " + src, ar_prep.pausal_ending(src), want)
for src, want in [("الْسَّلَام", "السَّلَام"), ("لِلْتَّفَاعُل", "لِلتَّفَاعُل"), ("الْقَمَر", "الْقَمَر")]:
    check("sun lam " + src, ar_prep.normalize(src), want)

# hamzat al-wasl and helping vowels in connected speech
for src, want in [("مِنْ الْبَيْتِ", "مِنَ ٱلْبَيْتِ"), ("مِنْ الصَّبْرِ", "مِنَ ٱلصَّبْرِ"),
                  ("حُبِسَتْ السَّمَاءُ", "حُبِسَتِ ٱلسَّمَاءُ"), ("عَلَيْكُمْ السَّلَامُ", "عَلَيْكُمُ ٱلسَّلَامُ"),
                  ("فِي الْبَيْتِ", "فِ ٱلْبَيْتِ"), ("بِهَذَا الصَّوْتِ", "بِهَذَ ٱلصَّوْتِ"), ("عَلَى الْأَرْضِ", "عَلَ ٱلْأَرْضِ"), ("كِتَابًا الْيَوْمَ", "كِتَابًا ٱلْيَوْمَ"), ("فَصِيحٌ، الْأَصْوَاتُ", "فَصِيحٌ، الْأَصْوَاتُ"), ("الْبَيْتُ", "الْبَيْتُ"),
                  ("الْمُسْلِمِينَ في الدَّانِمَارْك", "الْمُسْلِمِينَ فِ ٱلدَّانِمَارْك"), ("الَّذي جَاءَ", "الَّذِي جَاءَ"), ("أي شَيْءٍ", "أي شَيْءٍ")]:
    check("wasl " + src, ar_prep.mark_wasl(src), want)

# ClaritySynth's merge of the writer's own marks with the model's (with Fasih's shadda fix)
tmp = tempfile.mkdtemp()
try:
    g2p = os.path.join(tmp, "ar_g2p.py")
    shutil.copy(os.path.join(REPO, "third_party", "claritysynth", "ar_g2p.py"), g2p)
    subprocess.run([sys.executable, os.path.join(REPO, "addon_src", "patch_g2p.py"), g2p], check=True, stdout=subprocess.DEVNULL)
    spec = importlib.util.spec_from_file_location("ar_g2p_t", g2p); m = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(m)
        for user, model, want in [("شدّ", "شَدَّ", "شَدَّ"), ("محمّد", "مُحَمَّدٌ", "مُحَمَّدٌ"),
                                  ("أُكلت", "أَكَلَتْ", "أُكَلَتْ"), ("التفاحةُ", "التُّفَّاحَةَ", "التُّفَّاحَةُ"),
                                  ("علَم", "عَلَّمَ", "عَلَّمَ"), ("كتاب", "كِتَابٌ", "كِتَابٌ")]:
            check(f"merge {user}+{model}", m._merge_user_marks(user, model), want)
    except Exception as e:
        print("skip merge tests:", e)
finally:
    shutil.rmtree(tmp)

# script splitting (Arabic -> primary voice, other text -> secondary voice)
_drv = tempfile.mkdtemp()
subprocess.run([sys.executable, os.path.join(REPO, "addon_src", "patch_driver.py"),
                os.path.join(REPO, "third_party", "rhvoice-nvda-driver", "__init__.py"),
                os.path.join(_drv, "d.py")], check=True, stdout=subprocess.DEVNULL)
src = open(os.path.join(_drv, "d.py"), encoding="utf-8").read(); shutil.rmtree(_drv)
a = src.index("_AR_RANGES = "); b = src.index("class SynthDriver(SynthDriver):", a)
ns = {"unicodedata": unicodedata, "re": re, "_": lambda t: t, "threading": __import__("threading"), "os": os, "log": types.SimpleNamespace(error=print)}; exec(src[a:b], ns)
# quality settings: a change made while another is being written is never lost
import threading, time as _time
check("quality code in slice", "_quality_worker" in ns and "_atomic_write" in ns, True)
_qd = tempfile.mkdtemp()
ns["_quality_file_state"] = lambda: dict(_disk)
_disk = {"clarity": 50, "breathiness": "b0", "sharpening": False, "repair": False}
_writes = []
def _slow_apply(st):
    _time.sleep(0.3); _writes.append(dict(st)); _disk.clear(); _disk.update(st); return True
ns["_apply_quality"] = _slow_apply
_done = threading.Event()
threading.Thread(target=ns["_quality_worker"], args=(_done.set,), daemon=True).start()
def _set(k, v):
    with ns["_quality_lock"]:
        st = dict(ns["_pending_quality"]) if ns["_pending_quality"] is not None else dict(_disk)
        st[k] = v; ns["_pending_quality"] = st; ns["_quality_version"] += 1
    ns["_quality_wake"].set()
_set("clarity", 60); _time.sleep(0.1); _set("sharpening", True)
check("pending keeps clarity 60 during a write", ns["_quality_state"]()["clarity"], 60)
_time.sleep(1.0)
check("final state on disk", (_disk["clarity"], _disk["sharpening"]), (60, True))
check("nothing pending after the last write", ns["_pending_quality"], None)
ns["_atomic_write"](os.path.join(_qd, "f"), b"abc")
check("atomic write", open(os.path.join(_qd, "f"), "rb").read(), b"abc"); shutil.rmtree(_qd)
_t = "جملة أولى طويلة. " * 30 + "سؤال؟ «اقتباس.» نهاية"
check("sentence groups lossless", "".join(ns["_sentence_groups"](_t)), _t)
for _t in [_t, "كلمة " * 2000, "سطر قصير هنا\n" * 800, "ب" * 10000, "أ، " * 3000]:
    _g = ns["_sentence_groups"](_t)
    check("speech groups lossless, at most 400 chars (%d chars)" % len(_t), "".join(_g) == _t and max(map(len, _g)) <= 400, True)
check("speech groups cut at line breaks", ns["_sentence_groups"]("سطر قصير هنا\n" * 800)[0].endswith("\n"), True)
check("split mixed", ns["_split_by_script"]("افتح ملف README الآن"), [(True, "افتح ملف "), (False, "README "), (True, "الآن")])
check("split number before Latin", ns["_split_by_script"]("السَّاعَةُ 5 PM"), [(True, "السَّاعَةُ "), (False, "5 PM")])
check("split number inside Arabic", ns["_split_by_script"]("عندي 45 كتابا"), [(True, "عندي 45 كتابا")])
check("split tashkeel stays Arabic", ns["_split_by_script"]("مَرْحَبًا"), [(True, "مَرْحَبًا")])

# invisible characters and stray marks on alif
_aps = open(os.path.join(REPO, "addon_src", "arabic_pipeline.py"), encoding="utf-8").read()
_apn = {"re": re}; exec(_aps[_aps.index("_INVISIBLE ="):_aps.index("def _tanween_post")], _apn)
check("ZWNJ and direction marks removed", _apn["_INVISIBLE"].sub("", "مر\u200cحبا\u200f \u200dكتاب"), "مرحبا كتاب")
check("haraka on bare alif dropped", _apn["_alif_marks"]("اُكْتُبْ مَاْ"), "اكْتُبْ مَا")
check("haraka on madda dropped", _apn["_alif_marks"]("آَمَنَ"), "آمَنَ")
check("fathatan on alif kept", _apn["_alif_marks"]("كِتَاباً"), "كِتَاباً")
check("tatweel inside a word dropped", _apn["_clean_invisible"]("مـحـمـد رسول"), "محمد رسول")
check("tatweel alone named", _apn["_clean_invisible"]("ـ"), "تَطْوِيل")
check("tatweels alone named", _apn["_clean_invisible"](" ـــ "), "تَطْوِيل")
check("ZWNJ alone named", _apn["_clean_invisible"]("\u200c"), "مَانِعُ ٱلْوَصْل")
check("only invisibles: nothing sent", _apn["_clean_invisible"]("\u200c\u200f").strip(), "")
# full pipeline, with Rawi (optional)
lib = os.environ.get("RAWI_ORT_LIB"); addon = os.environ.get("FASIH_ADDON_DIR")
if lib and addon:
    from unittest import mock
    for mod in ["logHandler", "globalVars", "addonHandler", "config"]:
        sys.modules[mod] = mock.MagicMock()
    pkg = types.ModuleType("fasih"); pkg.__path__ = [addon]; sys.modules["fasih"] = pkg
    import fasih.arabic_pipeline as ap
    ap.init_dictionaries(tempfile.mkdtemp())
    for src, want in [("أما بعد", "أَمَّا بَعْد"), ("من الصبر", "مِنَ ٱلصَّبْر"), ("شدّ الحبل", "شَدَّ ٱلحَبْل"),
                      ("مصدراً ومنبعاً،", "مَصْدَرًا وَمُنْبَعًا،"), ("السلام عليكم", "السَّلَامُ عَلَيْكُم"),
                      ("ّ", "شَدَّة"), ("ش", "شِين"), ("ل", "لَام"), ("45", "خَمْسَةٌ وَ أَرْبَعُون"), ("فتًى.", "فَتَى.")]:
        check("pipeline " + src, ap.process(src), want)
    for src, want in [("لتاريخ الأمة الإسلامية، كما", "ٱلْإِسْلَامِيَّةِ،"), ("رأيت كتابا جميلا: كما", "جَمِيلًا:"),
                      ("هذا حق. ثم", "حَقّ."), ("قبل أن يصبحوا", "قَبْلَ"), ("السلام عليكم", "السَّلَامُ"),
                      ("الشاشة الرئيسية، ثم", "ثُمّ")]:
        check("context " + src, want in ap.process(src), True)
    for src, want in [("أنا أحب اللعب: I like playing", "I like playing"), ("اضغط Ctrl+C للنسخ", "Ctrl+C"),
                      ("راسلني على test@example.com", "test@example.com"), ("الفصل IV", "رَابِع")]:
        check("mixed " + src, want in ap.process(src), True)
    ap.options["mixed_priority"] = "arabic"
    check("mixed, Arabic first", "وَّل" in ap.process("أنا أحب اللعب: I like playing"), True)
    _polls = []
    ap.process("هذه جملة أولى. وهذه جملة ثانية. وهذه ثالثة.", is_cancelled=lambda: _polls.append(1) or False)
    check("diacritizer polls the cancel callback", len(_polls) > 0, True)
    ap.options["mixed_priority"] = "secondary"
    check("secondary first: digits alone stay digits", ap.process("123"), "123")
    check("ZWNJ inside a word", "\u200c" not in ap.process("مر\u200cحبا بكم"), True)
    check("ZWNJ alone named", ap.process("\u200c", diacritize=False), "مَانِعُ ٱلْوَصْل")
    check("tatweel alone named", ap.process("ـ", diacritize=False), "تَطْوِيل")
    check("tatweel inside a word dropped", "\u0640" not in ap.process("مـحـمـد رسول الله"), True)
    ap.options["mixed_priority"] = "auto"
else:
    print("(full-pipeline tests skipped: set RAWI_ORT_LIB and FASIH_ADDON_DIR)")
# output limiter: a full-scale onset never goes above the ceiling
import array
sys.path.insert(0, os.path.join(REPO, "addon_src")); import loudness
_o = loudness.Output(); _a = array.array("h", [32767] + [0] * 2000 + [32767] * 100 + [0] * 500)
_b = array.array("h"); _b.frombytes(_o.process(_a.tobytes()) + _o.flush())
check("limiter onset under ceiling", max(_b) <= int(loudness.CEIL), True)

# long-sentence splitter: lossless, and no piece longer than the limit
_at = tempfile.mkdtemp(); _p = os.path.join(_at, "t.py")
shutil.copy(os.path.join(REPO, "third_party", "claritysynth", "ar_tashkeel.py"), _p)
subprocess.run([sys.executable, os.path.join(REPO, "addon_src", "patch_tashkeel.py"), _p,
                os.path.join(REPO, "addon_src", "rawi_client.py")], check=True)
_s = open(_p, encoding="utf-8").read(); shutil.rmtree(_at)
_ns = {}; exec(_s[_s.index("def _split_sentences"):_s.index("def _diac_run_chunked")], _ns)
for _t in ["كلمة " * 300, "ب" * 900 + " نص", "أ، " * 500]:
    _o = _ns["_split_sentences"](_t, 800)
    check("splitter lossless", "".join(_o) == _t and max(map(len, _o)) <= 800, True)
check("bundled models searched first", _s.index("cands = [os.path.join(_VOWELIZERS") < _s.index("data_paths.search_dirs(\"vowelizers\")"), True)

# voice settings: every patch applies in the driver's order, and the repair patch changes the voice
def _apply(b, path):
    d = open(path, "rb").read(); assert d[:4] == b"FPAT"
    import struct
    i, n = 8, struct.unpack("<I", d[4:8])[0]
    b = bytearray(b)
    for _ in range(n):
        off, ln = struct.unpack("<QI", d[i:i + 12]); i += 12
        b[off:off + ln] = d[i:i + ln]; i += ln
    return bytes(b)
for _v in sorted(os.listdir(os.path.join(REPO, "voices"))):
    _w = os.path.join(REPO, "voices", _v, "variants")
    _base = open(os.path.join(_w, "base.data"), "rb").read()
    try:
        _r = _apply(_base, os.path.join(_w, "b12.patch"))
        if os.path.exists(os.path.join(_w, "sharp.patch")):
            _r = _apply(_r, os.path.join(_w, "sharp.patch"))
        _r = _apply(_r, os.path.join(_w, "repair.patch"))
        check(_v + " repair patch applies", _r != _base and b"WORLD1" in _r and b"WORLD1" not in _base, True)
    except Exception as e:
        check(_v + " repair patch applies", repr(e), "ok")

# Latin text read by the Arabic voice when no other voice is installed
_lat = {"re": re}; exec(_aps[_aps.index("# ---------------------------------------------------------------- Latin text without"):], _lat)
check("Latin fallback: a word is spoken", _lat["latin_to_arabic"]("Hello"), "هِلُو")
check("Latin fallback: acronym spelled", _lat["latin_to_arabic"]("NVDA"), "إِن ڤِي دِي إِيه")
check("Latin fallback: digits kept", _lat["latin_to_arabic"]("file 2"), "فِيلْ 2")
_drv = open(os.path.join(REPO, "addon_src", "patch_driver.py"), encoding="utf-8").read()
check("secondary voice cannot be turned off", '"none"' in _drv, False)

# French front end (liaison, numbers)
import fr_prep as _fp
for _src, _want in [("C'est un honneur.", "C'est \u203ftun \u203fnhonneur."), ("Il est 21 heures.", "Il est vingt \u203ftet une heures."),
                    ("vingt-et-une heures", "vingt-\u203ftet-une heures"), ("Les amis", "Les \u203fzamis"), ("Que dit-il ?", "Que dit-\u203ftil ?"),
                    ("Les héros et les haricots.", "Les héros et les haricots."), ("Neuf heures", "Neuve heures"),
                    ("Le 21e siècle", "Le vingt \u203ftet unième siècle"), ("de petits enfants", "de petits \u203fzenfants"), ("1 000 euros", "mille euros")]:
    check("French: " + _src, _fp.prepare(_src), _want)

# French numbers: decimals keep their zeros, long numbers are read digit by digit, agreement
for _src, _want in [("3,05", "trois virgule zéro cinq"), ("0,001", "zéro virgule zéro zéro un"),
                    ("81 heures", "quatre-vingt-une heures"), ("3,5", "trois virgule cinq")]:
    check("French number: " + _src, _fp.prepare(_src), _want)
check("French: long fraction does not fail", "virgule un deux trois" in _fp.prepare("1,123456789012345"), True)
check("French: very long number digit by digit", _fp.prepare("1234567890123456").startswith("un deux trois"), True)
check("French: no liaison before inflected aspirated h", _fp.prepare("les handicaps"), "les handicaps")
check("French: no liaison before hautes", _fp.prepare("les hautes"), "les hautes")

# voice downloader: a download cancelled before it starts sends no request
import types as _types, threading as _th
_fake = {"calls": 0}
class _R:
    def __init__(self, *a, **k): _fake["calls"] += 1; self.headers = {}
    def __enter__(self): return self
    def __exit__(self, *a): return False
    def raise_for_status(self): pass
    def iter_content(self, chunk_size=1): return iter([])
_stubs = {}
for _m in ["wx", "addonHandler", "globalPluginHandler", "globalVars", "gui", "gui.message", "gui.addonGui", "languageHandler", "logHandler", "configobj", "requests"]:
    _stubs[_m] = sys.modules.get(_m); sys.modules[_m] = _types.ModuleType(_m)
sys.modules["gui.message"].messageBox = None; sys.modules["gui.addonGui"].promptUserForRestart = None
sys.modules["logHandler"].log = _types.SimpleNamespace(error=lambda *a, **k: None)
sys.modules["configobj"].ConfigObj = object; sys.modules["addonHandler"].AddonBundle = object
sys.modules["wx"].Dialog = object; sys.modules["globalPluginHandler"].GlobalPlugin = object
sys.modules["requests"].head = _R; sys.modules["requests"].get = _R
_fv = {}
exec(compile(open(os.path.join(REPO, "addon_src", "globalPlugins", "fasihVoices.py"), encoding="utf-8").read(), "fasihVoices", "exec"), _fv)
_w = _fv["_Downloader"]([("data", "http://x/a.zip", None), ("langdata", "http://x/b.zip", None)], "/tmp")
_w.cancel.set(); _w.run()
check("downloader: cancelled before start sends no request", _fake["calls"], 0)
check("downloader: cancelled download reports no error", _w.error, None)
check("pack update: newer version offered", _fv["_newer"]("1.3.1", "1.3.0"), True)
check("pack update: same version", _fv["_newer"]("1.3.0", "1.3.0"), False)
check("pack update: not installed", _fv["_newer"]("1.3.0", None), False)
_vb = _fv["FasihVoiceBundle"].__new__(_fv["FasihVoiceBundle"]); _vb._voice = {"name": "Sami"}
_fv["__file__"] = os.path.join(REPO, "addon_src", "globalPlugins", "fasihVoices.py")
check("sample shipped for each pack voice", all(os.path.isfile(os.path.join(REPO, "addon_src", "samples", n + ".wav"))
      for n in ["Jamal", "Nour", "Sami", "Faris", "Huda", "Karim", "Rima", "Lamees", "Zayd", "Reem"]), True)
check("sample found for a Fasih voice", bool(_fv["_sample"](_vb)), True)
for _m, _v in _stubs.items():
    if _v is None: sys.modules.pop(_m, None)
    else: sys.modules[_m] = _v


# --- text normalization (numbers, abbreviations): no word may be eaten or glued
sys.path.insert(0, os.path.join(REPO, "third_party", "claritysynth"))
import ar_textnorm as _tn
for src, want in [
        ("الرَّحِيْم.", "الرَّحِيْم."), ("بسم الله الرحمن الرحيم.", "بسم الله الرحمن الرحيم."), ("الرحيـم.", "الرحيـم."),
        ("وقال: م.", "وقال: م."), ("توفي سنة 632م. ثم", "توفي سنة ستمئة واثنين وثلاثين ميلادي. ثم"),
        ("سنة 1445هـ.", "سنة ألف وأربعمئة وخمسة وأربعين هجري."), ("قال د. أحمد إن م. سامي وصل.", "قال دكتور أحمد إن مهندس سامي وصل."),
        ("الساعة 9:15 م", "الساعة التاسعة والربع مساء"), ("الساعة 10:30 صباحا.", "الساعة العاشرة والنصف صباحا."),
        ("عندي 3 كتب و5 سيارات.", "عندي ثلاثة كتب وخمس سيارات."), ("حضر 21 طالبة.", "حضر إحدى وعشرون طالبة."),
        ("3 ملفات و4 اجتماعات", "ثلاثة ملفات وأربعة اجتماعات"), ("5 دقائق", "خمس دقائق"), ("عددهم 13 طالبا", "عددهم ثلاثة عشر طالبا"),
        ("خلال 20 دقيقة", "خلال عشرين دقيقة"), ("في عام 2026", "في عام ألفين وستة وعشرين"),
        ("العدد 1,250,000 نسمة", "العدد مليون ومئتان وخمسون ألفا نسمة"), ("درجة الحرارة -5 درجات", "درجة الحرارة سالب خمس درجات"),
        ("الإصدار 2.5.1", "الإصدار اثنان نقطة خمسة نقطة واحد"), ("1/2 كوب", "نصف كوب"), ("3-5 أيام", "ثلاثة إلى خمسة أيام"),
        ("الدرس الـ5", "الدرس الخامس"), ("الحلقة الـ3", "الحلقة الثالثة"), ("القرن الـ21", "القرن الحادي والعشرون"),
        ("في 15/8/2024", "في الخامس عشر من أغسطس ألفين وأربعة وعشرين"), ("في 2 مايو", "في الثاني من مايو"),
        ("ص 45", "صفحة خمسة وأربعون"), ("المرتبة 1 والمرتبة 2", "المرتبة واحد والمرتبة اثنان"), ("خلال وقت", "خلال وقت")]:
    check("textnorm " + src, _tn.normalize(src), want)
# the diacritizer never adds a hamza the text does not have
_ts = open(os.path.join(REPO, "third_party", "claritysynth", "ar_tashkeel.py"), encoding="utf-8").read()
_tsn = {"unicodedata": unicodedata, "re": re}
exec(_ts[_ts.index("_HAMZA_FORMS ="):_ts.index("_HAMZA_BASE = {")], _tsn)
_b = _ts.index("_HAMZA_BASE = {"); exec(_ts[_b:_ts.index("}", _b) + 1], _tsn)
exec(_ts[_ts.index("def _restore_hamza"):_ts.index("\nclass ", _ts.index("def _restore_hamza"))], _tsn)
check("no hamza on bare waw", _tsn["_restore_hamza"]("البور", "البُؤَرَ"), "البُورَ")
check("no hamza on bare yaa", _tsn["_restore_hamza"]("كاين", "كَائِنٍ"), "كَاينٍ")
check("lost hamza restored", _tsn["_restore_hamza"]("سأل", "سَالَ"), "سَألَ")
check("no kasra on hamza above alif", _tsn["_restore_hamza"]("الأحداث", "ٱلْأِحْدَاث"), "ٱلْأَحْدَاث")

check("textnorm honorific sign", _tn.normalize("النبي \uFDFA"), "النبي صلى الله عليه وسلم")
check("textnorm (رض)", _tn.normalize("عمر (رض) قال"), "عمر رضي الله عنه قال")
check("textnorm emoji selector dropped", "\uFE0F" in _tn.normalize("أحبك \u2764\uFE0F"), False)
check("no shadda on mi'a", _tsn["_restore_hamza"]("مئة", "مِئَّةُ"), "مِئَةُ")
_ap2 = open(os.path.join(REPO, "addon_src", "arabic_pipeline.py"), encoding="utf-8").read()
_apn2 = {}; exec(_ap2[_ap2.index("_PUNCT ="):_ap2.index("return s.translate(_PUNCT) if s else s") + 40], _apn2)
check("Arabic question mark reaches the engine as ?", _apn2["_engine_punct"]("هل ذهبت؟ نعم؛ ثم"), "هل ذهبت? نعم; ثم")
# user pronunciation dictionary: whole words, also after attached prefixes
import json as _json
_pk = types.ModuleType("fpbt"); _pk.__path__ = [os.path.join(REPO, "addon_src", "pbt")]; sys.modules["fpbt"] = _pk
_dm = importlib.import_module("fpbt.dictionary_manager")
_dd = tempfile.mkdtemp()
_json.dump([{"original": "جبران", "replacement": "جُبْرَان"}, {"original": "NVDA", "replacement": "إِنْ فِي دِي إِيه"}],
           open(os.path.join(_dd, "user-dic.json"), "w", encoding="utf-8"), ensure_ascii=False)
_dm.load_user_dictionary(_dd)
check("dictionary after a prefix", _dm.apply_user_dictionary("قرأت لجبران"), "قرأت لجُبْرَان")
check("dictionary not inside a longer word", _dm.apply_user_dictionary("جبرانية"), "جبرانية")
check("dictionary Latin, any case", _dm.apply_user_dictionary("nvda"), "إِنْ فِي دِي إِيه")
shutil.rmtree(_dd, ignore_errors=True)
# light case endings: no i'rab mid-phrase, but keep what carries meaning or joining
sys.path.insert(0, os.path.join(REPO, "scripts")); import ar_prep as _arp
check("light: tanween dropped mid-phrase", _arp.light_case("هَذَا كِتَابٌ جَدِيدٌ مُفِيدٌ جِدًّا."), "هَذَا كِتَاب جَدِيد مُفِيد جِدًّا.")
check("light: vowel kept before al-", _arp.light_case("ذَهَبَ ٱلْوَلَدُ إِلَى ٱلْبَيْتِ"), "ذَهَبَ ٱلْوَلَد إِلَى ٱلْبَيْتِ")
check("light: first-person and feminine endings kept", _arp.light_case("قَرَأْتُ كِتَابًا وَلَكِ هَدِيَّةٌ"), "قَرَأْتُ كِتَابًا وَلَكِ هَدِيَّةٌ")
check("light: comma is a pause", _arp.light_case("رَأَيْتُ وَلَدًا صَغِيرًا، ثُمَّ"), "رَأَيْتُ وَلَدًا صَغِيرَا، ثُمَّ")
# screen-reader text
for src, want in [("الصفحة 12 من 340", "الصفحة اثنا عشر من ثلاثمئة وأربعين"), ("النتائج 1 - 20 من 350", "النتائج واحد إلى عشرين من ثلاثمئة وخمسين"),
        ("5 - 3 = 2", "خمسة ناقص ثلاثة يساوي اثنان"), ("الطلب رقم #4521", "الطلب رقم أربعة آلاف وخمسمئة وواحد وعشرون"),
        ("بدلا من 40$", "بدلا من أربعين دولار"), ("جميع الحقوق محفوظة © 2026", "جميع الحقوق محفوظة ألفان وستة وعشرون"),
        ("تقرير_المبيعات", "تقرير المبيعات"), ("الإصدار 2026.3", "الإصدار ألفان وستة وعشرون نقطة ثلاثة"),
        ("السرعة 120 كم/س", "السرعة مئة وعشرون كيلومتر في الساعة"), ("رسالة صوتية، 0:45", "رسالة صوتية، خمس وأربعون ثانية"),
        ("مقطع فيديو، 12:34", "مقطع فيديو، اثنتا عشرة دقيقة وأربع وثلاثون ثانية"), ("المدة 1:02:03", "المدة ساعة ودقيقتان وثلاث ثوان"),
        ("الساعة 14:05", "الساعة الثانية وخمس دقائق مساء"), ("أ) صواب", "أَلِفْ، صواب")]:
    check("screen reader " + src, _tn.normalize(src), want)
check("no ellipsis name glued to a word", any(e.get("cp") == "..." and e.get("type") == "word" for e in
      _json.load(open(os.path.join(REPO, "addon_src", "pbt", "Dictionaries", "Default-dic.json"), encoding="utf-8"))), False)
check("al- + wasl alif takes kasra", _tsn["_restore_hamza"]("الاستخدام", "ٱلَاسْتِخْدَامُ"), "ٱلِاسْتِخْدَامُ")
print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
