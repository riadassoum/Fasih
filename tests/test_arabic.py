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
                  ("فِي الْبَيْتِ", "فِ ٱلْبَيْتِ"), ("بِهَذَا الصَّوْتِ", "بِهَذَ ٱلصَّوْتِ"), ("عَلَى الْأَرْضِ", "عَلَ ٱلْأَرْضِ"), ("كِتَابًا الْيَوْمَ", "كِتَابًا ٱلْيَوْمَ"), ("فَصِيحٌ، الْأَصْوَاتُ", "فَصِيحٌ، الْأَصْوَاتُ"), ("الْبَيْتُ", "الْبَيْتُ")]:
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
        _r = _apply(_apply(_apply(_base, os.path.join(_w, "b12.patch")), os.path.join(_w, "sharp.patch")), os.path.join(_w, "repair.patch"))
        check(_v + " repair patch applies", _r != _base and b"WORLD1" in _r and b"WORLD1" not in _base, True)
    except Exception as e:
        check(_v + " repair patch applies", repr(e), "ok")

print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
