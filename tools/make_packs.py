"""make_packs.py VERSION OUTDIR
The Fasih voices that are not bundled in the add-on, one zip each (data/ for the engine,
variants/ for Fasih's quality settings), and voices.json listing them for Tools > Fasih >
Download voices. Upload everything in OUTDIR to the "voices" release of the repository.
Compact voices are the HTS voices in voices/. Premium voices are concatenative (unit-selection)
voices of real recordings built with unit/train_voice.sh, too large for the repository: list their
folders in FASIH_PREMIUM (separated by os.pathsep: ":", or ";" on Windows); a voice's NOTICE.md / README.md / about.txt go in its pack.
Author: Riad Assoum. License: GPL-3.0."""
import shutil, base64, hashlib, json, os, sys, zipfile

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
    "Hadi": ("هادي", "male", "Trained on speech cloned with OmniVoice (k2-fsa, Apache-2.0) from Yahya Alnwsany's Arabic professional voice dataset (NightPrince, CC0)."),
    "Bassel": ("باسل", "male", "Trained on about 5.9 hours read for Fasih by speaker 1 of nipponjo's Arabic FastPitch (tts_arabic), the voice family Sami and Nour come from."),
    "Rana": ("رنا", "female", "Trained on about 7 hours read for Fasih by speaker 2 of nipponjo's Arabic FastPitch (tts_arabic), the voice family Sami and Nour come from."),
    "Haytham": ("هيثم", "male", "Trained on about 7.5 hours generated for Fasih with Google's Gemini text-to-speech and prepared by Riad Assoum: the most natural, widest-ranging Fasih voice."),
    "Leen": ("لين", "female", "Trained on the Leen Arabic Dataset from PlanetBlindTech's BayanTTS (10,000 sentences), with Shamseddin's permission."),
    "Shahrazad": ("شهرزاد", "female", "A storyteller's voice, trained on Aida Boumaza's LibriVox reading of Alf Layla wa Layla (public domain)."),
}
_only = os.environ.get("FASIH_ONLY", "")
ONLY = [x.strip() for x in _only.split(",")] if "," in _only else _only.split()   # commas allow names with spaces
os.makedirs(OUT, exist_ok=True)
voices = []
REG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "voice_versions.json")
REG = json.load(open(REG_PATH, encoding="utf-8")) if os.path.exists(REG_PATH) else {}
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
    # Versions live in tools/voice_versions.json: a voice keeps its version until its own files change,
    # so "check for updates" only offers voices that really were updated.
    h = hashlib.sha256()
    for root, _, files in sorted(os.walk(src)):
        for f in sorted(files):
            h.update(os.path.relpath(os.path.join(root, f), src).encode()); h.update(open(os.path.join(root, f), "rb").read())
    sha = h.hexdigest(); rec = REG.get(name)
    if rec is None or rec["sha256"] != sha:
        rec = REG[name] = {"version": VER, "sha256": sha}
    ar, gender, desc = INFO.get(name, (name, "male", ""))
    voices.append({"id": name.lower(), "name": name, "name_ar": ar, "gender": gender, "description": desc, "quality": "compact",
                   "version": rec["version"], "dataUrl": BASE + os.path.basename(zpath),
                   "dataMd5": base64.b64encode(hashlib.md5(data).digest()).decode(), "size": len(data)})
    # voices newer than the add-on's bundled samples get an online one, uploaded next to the packs
    smp = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "samples_online", name + ".wav")
    if os.path.isfile(smp):
        shutil.copy(smp, os.path.join(OUT, "Fasih-sample-%s.wav" % name))
        voices[-1]["sampleUrl"] = BASE + "Fasih-sample-%s.wav" % name
    elif rec.get("entry", {}).get("sampleUrl"):
        voices[-1]["sampleUrl"] = rec["entry"]["sampleUrl"]
    rec["entry"] = voices[-1]
    print("%-8s %6.1f MB" % (name, len(data) / 1e6))
def vq_units(path, M=4):
    """units.bin (FSUV0003) for a pack (FSUQ0003): fixed fields column by column, and each unit edge's join
    features as M one-byte codebook indices (product vector quantization, unit/vq_units.py). The add-on
    rebuilds a normal units.bin from it. Join features only steer the search: in tests on held-out text the
    joins were as smooth as with the full numbers (Rojc & Mlakar 2019 found the same above 50% compression)."""
    import struct, numpy as np
    sys.path.insert(0, os.path.join(REPO, "unit"))
    from vq_units import load, quantize
    d, nu, D, nc, rec, h = load(path)
    fx = np.frombuffer(d, np.uint8, nu * rec, h).reshape(nu, rec)
    jl = fx[:, 16 + nc:].copy().view("<f2").reshape(nu, D)
    rest = np.frombuffer(d, "<f2", offset=h + nu * rec).reshape(-1, D)
    X = np.concatenate([jl, rest]).astype(np.float32)
    groups, books, codes = quantize(X, M)
    o = [b"FSUQ0003", d[8:h], struct.pack("<II", M, len(rest))]
    for g, b in zip(groups, books):
        o += [struct.pack("<II", int(g[0]), len(g)), b.astype("<f2").tobytes()]
    o.append(fx[:, :16 + nc].T.copy().tobytes())          # the fixed fields, column by column
    o.append(codes.T.copy().tobytes())                     # codes, group by group (left edges, then the rest)
    return b"".join(o)


def transpose_units(path):
    """units.bin (FSUV0003) with its fixed-size unit records stored column by column (FSUT0003)."""
    import struct
    d = open(path, "rb").read()
    assert d[:8] == b"FSUV0003", "run unit/compact_voice.py on the voice first"
    nu, nutt, jd, nc, sr = struct.unpack_from("<IIIII", d, 8)
    rec = 16 + nc + 2 * (jd + 1); h = 28 + 4 * jd + 20 * nutt + (nu + 7) // 8
    cols = b"".join(d[h + j:h + nu * rec:rec] for j in range(rec))
    return b"FSUT0003" + d[8:h] + cols + d[h + nu * rec:]


# premium voices: the voice folder itself is data/
PREMIUM_FILES = ("voice.info", "voice.params", "units.bin", "audio.opa", "mel.bin", "marks.bin", "models.bin", "phonesim.txt", "layout.txt", "phones.txt", "context.txt", "weights.txt", "eq.txt")
for src in [x for x in os.environ.get("FASIH_PREMIUM", "").split(os.pathsep) if x]:   # ":" between folders (";" on Windows), so names may have spaces
    info = dict(l.strip().split("=", 1) for l in open(os.path.join(src, "voice.info"), encoding="utf-8") if "=" in l)
    name = info["name"]
    if ONLY and name not in ONLY:
        continue
    slug = name.lower().replace(" ", "-")
    zpath = os.path.join(OUT, "Fasih-voice-%s.zip" % name.replace(" ", "-"))
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in PREMIUM_FILES:
            if not os.path.exists(os.path.join(src, f)):
                continue
            if f == "units.bin":
                # fixed fields column by column (about a third smaller), join features vector-quantized
                z.writestr("data/units.bin.q", vq_units(os.path.join(src, f)))
            else:
                # the Opus audio does not compress further: store it
                z.write(os.path.join(src, f), "data/" + f, compress_type=zipfile.ZIP_STORED if f == "audio.opa" else zipfile.ZIP_DEFLATED)
        for f in ("NOTICE.md", "README.md"):
            if os.path.exists(os.path.join(src, f)):
                z.write(os.path.join(src, f), f)
    data = open(zpath, "rb").read()
    h = hashlib.sha256()
    for f in PREMIUM_FILES:
        if os.path.exists(os.path.join(src, f)):
            h.update(f.encode()); h.update(open(os.path.join(src, f), "rb").read())
    rec = REG.get(name)
    if rec is None or rec["sha256"] != h.hexdigest():
        rec = REG[name] = {"version": VER, "sha256": h.hexdigest()}
    about = {}
    if os.path.exists(os.path.join(src, "about.txt")):
        about = dict(l.rstrip("\n").split("=", 1) for l in open(os.path.join(src, "about.txt"), encoding="utf-8") if "=" in l)
    voices.append({"id": slug, "name": name, "name_ar": about.get("name_ar", name), "gender": info.get("gender", "male"),
                   "description": about.get("description", "Premium concatenative voice."), "quality": "premium",
                   "version": rec["version"], "dataUrl": BASE + os.path.basename(zpath),
                   "dataMd5": base64.b64encode(hashlib.md5(data).digest()).decode(), "size": len(data)})
    smp = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "samples_online", name + ".wav")
    if os.path.isfile(smp):
        shutil.copy(smp, os.path.join(OUT, "Fasih-sample-%s.wav" % name.replace(" ", "-")))
        voices[-1]["sampleUrl"] = BASE + "Fasih-sample-%s.wav" % name.replace(" ", "-")
    rec["entry"] = voices[-1]
    print("%-8s %6.1f MB (premium)" % (name, len(data) / 1e6))
# Voices whose files live only on the releases page (not in this repository) keep the catalogue
# entry stored when their pack was last built (compact and premium alike).
built = {v["name"] for v in voices}
for name, rec in sorted(REG.items()):
    if name.startswith("__") or name in built or "entry" not in rec or name in CORE or (ONLY and name not in ONLY):
        continue
    voices.append(rec["entry"])
voices.sort(key=lambda v: (v.get("quality") == "premium", v["name"]))
# descriptions in Arabic, shown when NVDA's language is Arabic (the add-on falls back to English)
AR_DESC = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "voice_descriptions_ar.json"), encoding="utf-8"))
for v in voices:
    if v["name"] in AR_DESC:
        v["description_ar"] = AR_DESC[v["name"]]
json.dump(REG, open(REG_PATH, "w", encoding="utf-8"), indent=1, sort_keys=True)
json.dump({"version": 1, "voices": voices}, open(os.path.join(OUT, "voices.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print("voices.json:", len(voices), "voices")
