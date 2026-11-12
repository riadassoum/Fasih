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

# hamzat al-wasl and helping vowels in connected speech
for src, want in [("مِنْ الْبَيْتِ", "مِنَ ٱلْبَيْتِ"), ("مِنْ الصَّبْرِ", "مِنَ ٱلصَّبْرِ"),
                  ("حُبِسَتْ السَّمَاءُ", "حُبِسَتِ ٱلسَّمَاءُ"), ("عَلَيْكُمْ السَّلَامُ", "عَلَيْكُمُ ٱلسَّلَامُ"),
                  ("فِي الْبَيْتِ", "فِي ٱلْبَيْتِ"), ("الْبَيْتُ", "الْبَيْتُ")]:
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
src = open(os.path.join(REPO, "addon_src", "patch_driver.py"), encoding="utf-8").read()
a = src.index("_AR_RANGES = "); b = src.index("class SynthDriver(SynthDriver):", a)
ns = {"unicodedata": unicodedata}; exec(src[a:b], ns)
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
                      ("ّ", "شَدَّة"), ("ش", "شِين"), ("45", "خَمْسَةٌ وَ أَرْبَعُون"), ("فتًى.", "فَتَى.")]:
        check("pipeline " + src, ap.process(src), want)
    for src, want in [("أنا أحب اللعب: I like playing", "I like playing"), ("اضغط Ctrl+C للنسخ", "Ctrl+C"),
                      ("راسلني على test@example.com", "test@example.com"), ("الفصل IV", "رَابِع")]:
        check("mixed " + src, want in ap.process(src), True)
    ap.options["mixed_priority"] = "arabic"
    check("mixed, Arabic first", "أَوَّل" in ap.process("أنا أحب اللعب: I like playing"), True)
    ap.options["mixed_priority"] = "auto"
else:
    print("(full-pipeline tests skipped: set RAWI_ORT_LIB and FASIH_ADDON_DIR)")
print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
