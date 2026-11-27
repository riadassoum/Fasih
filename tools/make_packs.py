"""make_packs.py VERSION OUTDIR
The Fasih voices that are not bundled in the add-on, one zip each (data/ for the engine,
variants/ for Fasih's quality settings), and voices.json listing them for Tools > Fasih >
Download voices. Upload everything in OUTDIR to the "voices" release of the repository.
Compact voices are the HTS voices in voices/. Premium voices are concatenative (unit-selection)
voices built with unit/train_voice.sh, too large for the repository: list their folders in
FASIH_PREMIUM (space separated); a voice's NOTICE.md / README.md / about.txt go in its pack.
Author: Riad Assoum. License: GPL-3.0."""
import base64, hashlib, json, os, sys, zipfile

VER, OUT = sys.argv[1], sys.argv[2]
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CORE = os.environ.get("FASIH_CORE", "Riad Aseel Elise").split()
BASE = "https://github.com/riadassoum/Fasih/releases/download/voices/"
INFO = {
    "Jamal": ("جمال", "male", "Classical Arabic, trained on the ClArTTS corpus (MBZUAI)."),
    "Nour": ("نور", "female", "Trained on NeoBoy/arabic-tts-wav-24k."),
    "Sami": ("سامي", "male", "Modern Standard Arabic with a Damascene colour, trained on Nawar Halabi's Arabic Speech Corpus."),
    "Faris": ("فارس", "male", "Modern Standard Arabic, trained on ArVoice (MBZUAI)."),
    "Huda": ("هدى", "female", "Modern Standard Arabic, trained on ArVoice (MBZUAI)."),
    "Karim": ("كريم", "male", "Trained on ArVoice (MBZUAI)."),
    "Rima": ("ريما", "female", "Trained on ArVoice (MBZUAI)."),
    "Lamees": ("لميس", "female", "A young voice, trained on speech generated with Nabra-82M."),
    "Zayd": ("زيد", "male", "PlanetBlindTech's voice, retrained with Fasih's recipe on their corpus."),
    "Reem": ("ريم", "female", "Trained by PlanetBlindTech with Fasih's recipe."),
    "Amjad": ("أمجد", "male", "Modern Standard Arabic, trained on the Arabic MSA 25K Saudi Male corpus (moesaif, CC BY 4.0)."),
    "Nabil": ("نبيل", "male", "Clear Modern Standard Arabic, trained on about 7.7 hours read for Fasih by the Piper voice Kareem (rhasspy, MIT), mostly two to four sentences at a time so long passages stay steady. Non-commercial use only."),
    "Evan": ("إيفان", "male", "English voice for reading English text (choose it as Fasih's English voice instead of BDL), trained on speech from OpenEVV's Adult Male 1. Its voice data is IBM's (Embedded ViaVoice), not covered by Fasih's licence: personal, non-commercial use only; see the NOTICE in the pack."),
    "Aisha": ("عائشة", "female", "Trained by PlanetBlindTech with Fasih's recipe."),
    "Shahrazad": ("شهرزاد", "female", "A storyteller's voice, trained on Aida Boumaza's LibriVox reading of Alf Layla wa Layla (public domain)."),
}
ONLY = os.environ.get("FASIH_ONLY", "").split()
os.makedirs(OUT, exist_ok=True)
voices = []
for name in sorted(os.listdir(os.path.join(REPO, "voices"))):
    if name in CORE or (ONLY and name not in ONLY):
        continue
    src = os.path.join(REPO, "voices", name)
    zpath = os.path.join(OUT, "Fasih-voice-%s.zip" % name)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for f in ("voice.info", "voice.params"):
            z.write(os.path.join(src, f), "data/" + f)
        z.write(os.path.join(src, "24000", "bpf.txt"), "data/24000/bpf.txt")
        for f in sorted(os.listdir(os.path.join(src, "variants"))):
            if f == "sharp.patch" and not os.environ.get("FASIH_SHARP"):
                continue
            z.write(os.path.join(src, "variants", f), "variants/" + f)
        for f in ("NOTICE.md", "README.md"):       # a voice whose rights are not Fasih's carries its notice
            if os.path.exists(os.path.join(src, f)):
                z.write(os.path.join(src, f), f)
        if not os.path.exists(os.path.join(src, "variants", "gain.txt")):
            z.writestr("variants/gain.txt", "0.7\n")
    data = open(zpath, "rb").read()
    ar, gender, desc = INFO.get(name, (name, "male", ""))
    voices.append({"id": name.lower(), "name": name, "name_ar": ar, "gender": gender, "description": desc, "quality": "compact",
                   "version": VER, "dataUrl": BASE + os.path.basename(zpath),
                   "dataMd5": base64.b64encode(hashlib.md5(data).digest()).decode(), "size": len(data)})
    print("%-8s %6.1f MB" % (name, len(data) / 1e6))
# premium voices: the voice folder itself is data/
PREMIUM_FILES = ("voice.info", "voice.params", "units.bin", "audio.opa", "marks.bin", "layout.txt", "phones.txt", "context.txt", "weights.txt")
for src in os.environ.get("FASIH_PREMIUM", "").split():
    info = dict(l.strip().split("=", 1) for l in open(os.path.join(src, "voice.info"), encoding="utf-8") if "=" in l)
    name = info["name"]
    if ONLY and name not in ONLY:
        continue
    zpath = os.path.join(OUT, "Fasih-voice-%s.zip" % name)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in PREMIUM_FILES:
            if os.path.exists(os.path.join(src, f)):
                # the Opus audio does not compress further: store it
                z.write(os.path.join(src, f), "data/" + f, compress_type=zipfile.ZIP_STORED if f == "audio.opa" else zipfile.ZIP_DEFLATED)
        for f in ("NOTICE.md", "README.md"):
            if os.path.exists(os.path.join(src, f)):
                z.write(os.path.join(src, f), f)
    data = open(zpath, "rb").read()
    about = {}
    if os.path.exists(os.path.join(src, "about.txt")):
        about = dict(l.rstrip("\n").split("=", 1) for l in open(os.path.join(src, "about.txt"), encoding="utf-8") if "=" in l)
    voices.append({"id": name.lower(), "name": name, "name_ar": about.get("name_ar", name), "gender": info.get("gender", "male"),
                   "description": about.get("description", "Premium concatenative voice."), "quality": "premium",
                   "version": VER, "dataUrl": BASE + os.path.basename(zpath),
                   "dataMd5": base64.b64encode(hashlib.md5(data).digest()).decode(), "size": len(data)})
    print("%-8s %6.1f MB (premium)" % (name, len(data) / 1e6))
json.dump({"version": 1, "voices": voices}, open(os.path.join(OUT, "voices.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print("voices.json:", len(voices), "voices")
