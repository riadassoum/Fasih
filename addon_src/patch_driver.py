"""Fork RHVoice's official NVDA synthDriver into a self-contained Arabic driver.
usage: patch_driver.py official/__init__.py out/__init__.py"""
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding="utf-8").read()


def rep(old, new, count=1):
    global s
    assert s.count(old) >= 1, "pattern not found: " + old[:60]
    s = s.replace(old, new, count)


rep('config_path = os.path.join(globalVars.appArgs.configPath, "RHVoice-config")',
    'config_path = os.path.join(globalVars.appArgs.configPath, "Fasih-config")\n'
    'data_dir = os.path.join(module_dir, "data")\n'
    'from . import arabic_pipeline')
rep('''    name = "RHVoice"
    description = "RHVoice"''', '''    name = "fasih"
    description = "Fasih (\u0641\u0635\u064a\u062d), RHVoice Arabic"''')
rep('''        return any(re.match(r"RHVoice.*-voice", addon.name) for addon in addonHandler.getRunningAddons())''',
    '''        return os.path.isdir(os.path.join(data_dir, "voices"))''')
rep('''    def __get_resource_paths(self):
        return [''', '''    def __get_resource_paths(self):
        # our own voices/languages come through data_path; voices installed as
        # official RHVoice voice add-ons (e.g. via RHVoice Voice Manager) are added here
        return [''')
rep('''        init_params = RHVoice_init_params(
            None,''', '''        try:
            _apply_quality(_quality_state(), packs_only=True)   # packs installed since the last start
        except Exception:
            log.error("Fasih: cannot prepare the voice packs", exc_info=True)
        init_params = RHVoice_init_params(
            data_dir.encode("utf-8"),''')
# Arabic preprocessing runs on the TTS worker thread, never on NVDA's main thread.
# Language switching follows ClaritySynth: the text is split by script; Arabic runs
# go to the primary (Arabic) voice and other runs to the secondary voice, each as a
# plain single-voice message (no mixed RHVoice profiles).
rep('''        if msg:
            self.__player.on_new_message()
            self.__lib.RHVoice_speak(msg)
            self.__player.idle()''', '''        if msg:
            self.__player.on_new_message()
            self.__lib.RHVoice_speak(msg)
            if getattr(self, "final", True):
                self.__player.idle()''')
rep('''    def speak(self, speechSequence):
        conv = SsmlConverter(self, self.language)
        text = conv.convertToXml(speechSequence)
        task = SpeakText(self.__lib, self.__tts_engine, text, self.__cancel_flag, self.__player)
        task.set_voice_profile(self.__profile)
        task.set_rate(self.__rate)
        task.set_pitch(self.__pitch)
        task.set_volume(self.__volume)
        task.configure_rate_boost(self.__rate_boost)
        self.__tts_queue.put(task)''', '''    def speak(self, speechSequence):
        seq = list(speechSequence)
        primary = self.__profile.split("+")[0]
        secondary = self._secondary_profile()
        # a French secondary voice: liaison and abbreviations written for it (fr_prep)
        fr_sec = bool(secondary) and self.__voice_languages.get(secondary, "").lower().startswith("fr")
        rate, pitch, volume, boost = self.__rate, self.__pitch, self.__volume, self.__rate_boost
        langs = self.__voice_languages

        def job():
            # Streaming: each finished segment (one voice) is spoken at once, while the next
            # is still being prepared, and a cancel is noticed between every step.
            if _post_filter is not None:
                _post_filter.trim_next = True      # 1.3: no dead air before the first sound
                _post_filter._trim_tail = []
            cancel = self.__cancel_flag
            state = {}         # last pitch/rate/volume/char-mode command, re-sent when the voice changes
            cur = [None, []]   # voice, items of the segment being built
            spoke = [False]
            char_mode = False

            def say(voice, items, final):
                if cancel.is_set():
                    return
                conv = SsmlConverter(self, langs.get(voice, "ar"))
                task = SpeakText(self.__lib, self.__tts_engine, conv.convertToXml(items), cancel, self.__player)
                task.final = final
                task.set_voice_profile(voice)
                task.set_rate(rate)
                task.set_pitch(pitch)
                task.set_volume(volume)
                task.configure_rate_boost(boost)
                task()
                spoke[0] = True

            def has_text(items):
                return any(isinstance(x, str) and x.strip() for x in items)

            def emit(voice, item):
                if cur[0] != voice:
                    if cur[0] is None or not has_text(cur[1]):
                        cur[0] = voice           # nothing to say yet: just relabel
                    else:
                        say(cur[0], cur[1], False)
                        cur[0], cur[1] = voice, list(state.values())
                cur[1].append(item)

            latin = [secondary]

            def emit_latin(chunk):
                v = latin[0]
                if not v:
                    emit(primary, arabic_pipeline.latin_to_arabic(chunk))
                elif self.__voice_languages.get(v, "").lower().startswith("fr"):
                    emit(v, _fr(chunk))
                else:
                    emit(v, chunk)

            for item in seq:
                if cancel.is_set():
                    return
                if isinstance(item, LangChangeCommand):
                    # an explicit language (a web page's lang, NVDA's automatic language switching)
                    # picks the voice for Latin text when one of that language is installed;
                    # otherwise, and for Arabic, the script decides as before
                    latin[0] = self._voice_for_language(item.lang) or secondary
                    continue
                if isinstance(item, str) and not char_mode and (len(item) > 300 or len(_sentences(item)) > 1):
                    # a long text (say all, a whole page) is prepared and spoken sentence
                    # group by sentence group: speech starts after the first one, and a
                    # cancel stops the work at the next sentence instead of after the page
                    for piece in _sentences(item):
                        if cancel.is_set():
                            return
                        try:
                            new = arabic_pipeline.process(piece, diacritize=True, is_cancelled=cancel.is_set)
                        except Exception:
                            log.error("Fasih: text pipeline failed", exc_info=True)
                            new = piece
                        for is_ar, chunk in _split_by_script(new):
                            emit(primary, chunk) if is_ar else emit_latin(chunk)
                        if cancel.is_set():
                            return
                        if has_text(cur[1]):
                            say(cur[0], cur[1], False)
                            cur[1] = list(state.values())
                    continue
                if isinstance(item, str):
                    try:
                        new = arabic_pipeline.process(item, diacritize=not char_mode)
                    except Exception:
                        log.error("Fasih: text pipeline failed", exc_info=True)
                        new = item
                    if cancel.is_set():
                        return
                    if char_mode and new != item:
                        # a letter/symbol name: read it as a word, then return to character mode
                        emit(primary, CharacterModeCommand(False))
                        emit(primary, new)
                        emit(primary, CharacterModeCommand(True))
                        continue
                    for is_ar, chunk in _split_by_script(new):
                        emit(primary, chunk) if is_ar else emit_latin(chunk)
                    continue
                if isinstance(item, CharacterModeCommand):
                    char_mode = item.state
                    state["char"] = item
                elif isinstance(item, PitchCommand):
                    state["pitch"] = item
                elif isinstance(item, RateCommand):
                    state["rate"] = item
                elif isinstance(item, VolumeCommand):
                    state["volume"] = item
                if cur[0] is None:
                    cur[0] = primary
                cur[1].append(item)
            if cancel.is_set():
                return
            if cur[0] is not None and (has_text(cur[1]) or any(not isinstance(x, str) for x in cur[1])):
                say(cur[0], cur[1], True)
            else:
                if spoke[0]:
                    self.__player.idle()
                synthDoneSpeaking.notify(synth=self)

        self.__tts_queue.put(job)''')
rep('''        log.info("Using RHVoice version {}".format(self.__lib.RHVoice_get_version().decode()))''',
    '''        log.info("Using RHVoice version {}".format(self.__lib.RHVoice_get_version().decode()))
        arabic_pipeline.preload()''')
# Primary voice = an Arabic voice; secondary voice = any non-Arabic voice (default Bdl)
rep('''        if self.__profile is None:
            self.__profile = self.__profiles[0]''', '''        self.__profiles = [p for p in self.__profiles if "+" not in p]
        ar = self._arabic_voices()
        if self.__profile not in ar:
            self.__profile = next((v for v in ("Aseel", "Riad", "Jamal") if v in ar), ar[0] if ar else self.__profiles[0])
        if not hasattr(self, "_secondary"):
            self._secondary = "auto"''')
rep('''class SynthDriver(SynthDriver):''', '''_AR_RANGES = ((0x0600, 0x06FF), (0x0750, 0x077F), (0x08A0, 0x08FF), (0xFB50, 0xFDFF), (0xFE70, 0xFEFF))


def _is_ar(ch):
    o = ord(ch)
    return any(a <= o <= b for a, b in _AR_RANGES)


_SENT_END = re.compile("[.!?\\u061f\\u06d4]+[\\"'\\u00bb\\u201d)\\\\]]*(?:\\\\s+|$)|\\n\\\\s*")
_SOFT_CUT = re.compile("[,;:\\u060c\\u061b]\\\\s+|\\\\s+")
_SENT_END_RUN = re.compile("[.!?\\u061f\\u06d4](?:\\\\s*[.!?\\u061f\\u06d4])*[\\"'\\u00bb\\u201d)\\\\]]*(?:\\\\s+|$)|\\n\\\\s*")


def _sentence_groups(text, size=200, most=400):
    """Cut a long text into pieces of about `size` characters: after a sentence end or a
    line break when there is one, else at a comma or a space before `most` characters,
    else (one huge word) at `most`. Nothing is lost: joined back, the pieces are the text."""
    out, start, n = [], 0, len(text)
    while n - start > most:
        cut = None
        for m in _SENT_END.finditer(text, start + 1, start + most):
            if m.end() - start >= size:
                cut = m.end()
                break
        if cut is None:
            soft = [m.end() for m in _SOFT_CUT.finditer(text, start + 1, start + most) if m.end() - start >= size // 2]
            cut = soft[-1] if soft else start + most
        out.append(text[start:cut])
        start = cut
    out.append(text[start:])
    return [p for p in out if p]


def _sentences(text, shortest=25):
    """1.3: one sentence per piece, so every sentence is its own utterance. The voices were
    trained one sentence per recording; deep into a long utterance some (Nabil) lose their
    pitch and voicing. A piece shorter than `shortest` characters joins the next one, and a
    sentence longer than 400 characters is cut further. Joined back, the pieces are the text."""
    out, start = [], 0
    for m in _SENT_END_RUN.finditer(text):
        if m.end() - start >= shortest:
            out.append(text[start:m.end()])
            start = m.end()
    if start < len(text):
        if out and len(text) - start < shortest:
            out[-1] += text[start:]
        else:
            out.append(text[start:])
    res = []
    for piece in out:
        res.extend(_sentence_groups(piece) if len(piece) > 400 else [piece])
    return [x for x in res if x]


def _fr(text):
    try:
        from . import fr_prep
        return fr_prep.prepare(text)
    except Exception:
        log.error("Fasih: French front end failed", exc_info=True)
        return text


def _split_by_script(text):
    # (is_arabic, chunk) runs, after ClaritySynth. Spaces and punctuation ride with the current
    # run; marks (tashkeel) always belong to Arabic. A number placed between Arabic and Latin text
    # goes with the Latin word that follows it (5 PM, 11 Pro), otherwise with the current run.
    runs, cur, cur_ar, pend = [], [], None, []
    for ch in text:
        if unicodedata.category(ch) == "Mn" and _is_ar(ch):
            want = True
        elif ch.isalpha():
            want = _is_ar(ch)
        else:
            pend.append(ch)
            continue
        if cur_ar is None:
            cur_ar = want
        if want != cur_ar:
            give = []
            if not want:
                k = next((i for i, c in enumerate(pend) if c.isdigit()), None)
                if k is not None:
                    pend, give = pend[:k], pend[k:]
            cur += pend
            runs.append((cur_ar, "".join(cur)))
            cur, cur_ar, pend = give, want, []
        cur += pend
        pend = []
        cur.append(ch)
    cur += pend
    if cur:
        runs.append((bool(cur_ar) if cur_ar is not None else True, "".join(cur)))
    return runs


class SynthDriver(SynthDriver):''')
rep('''    def _get_availableVoices(self):
        return OrderedDict((profile, VoiceInfo(profile, profile, self.__voice_languages[profile.split("+")[0]])) for profile in self.__profiles)''', '''    def _arabic_voices(self):
        return [p for p in self.__profiles if self.__voice_languages.get(p, "").split("_")[0] == "ar"]

    def _get_availableVoices(self):
        ar = self._arabic_voices() or self.__profiles
        return OrderedDict((p, VoiceInfo(p, p, self.__voice_languages[p])) for p in ar)

    def _voice_for_language(self, lang):
        """An installed non-Arabic voice of this language (a LangChangeCommand), else None."""
        code = (lang or "").lower().replace("-", "_").split("_")[0]
        if not code or code == "ar":
            return None
        sec = self._secondary_profile()
        if sec and self.__voice_languages.get(sec, "").lower()[:2] == code:
            return sec                    # the chosen secondary voice first, when it fits
        for p in self.__profiles:
            if p not in self._arabic_voices() and self.__voice_languages.get(p, "").lower()[:2] == code:
                return p
        return None

    def _secondary_profile(self):
        sv = getattr(self, "_secondary", "auto")
        if sv in self.__profiles and sv not in self._arabic_voices():
            return sv
        # automatic (or a saved voice that is no longer installed): a voice in NVDA's own language
        # (French NVDA -> a French voice), then an English voice, then the first non-Arabic voice found
        others = [p for p in self.__profiles if p not in self._arabic_voices()]
        others.sort(key=lambda p: p != "Bdl")      # the English voice that comes with Fasih first
        try:
            ui = (languageHandler.getLanguage() or "").lower()[:2]
        except Exception:
            ui = ""
        for want in (ui, "en"):
            for p in others:
                if want and want != "ar" and self.__voice_languages.get(p, "").lower()[:2] == want:
                    return p
        for p in others:
            return p
        return ""

    def _get_availableSecondaryvoices(self):
        out = OrderedDict()
        out["auto"] = StringParameterInfo("auto", "Automatic (a voice in NVDA's language, else English)")
        for p in self.__profiles:
            if p not in self._arabic_voices():
                out[p] = StringParameterInfo(p, "%s (%s)" % (p, self.__voice_languages[p]))
        return out

    def _get_secondaryVoice(self):
        sv = getattr(self, "_secondary", "auto")
        return sv if sv in self._get_availableSecondaryvoices() else "auto"

    def _set_secondaryVoice(self, v):
        self._secondary = v if v in self._get_availableSecondaryvoices() else "auto"''')
rep('''    def _set_voice(self, voice):
        if voice in self.__profiles:''', '''    def _set_voice(self, voice):
        voice = voice.split("+")[0]          # profiles saved by earlier builds, e.g. "Jamal+SLT"
        if voice in self.__profiles:''')
rep('''import languageHandler
''', '''import languageHandler
import unicodedata
from autoSettingsUtils.driverSetting import BooleanDriverSetting, DriverSetting, NumericDriverSetting
from autoSettingsUtils.utils import StringParameterInfo
''')
# ---- voice quality settings in NVDA (clarity / breathiness / sharpening) ----
rep('''    supportedSettings = [
        SynthDriver.VoiceSetting(),
        SynthDriver.RateSetting(),
        SynthDriver.RateBoostSetting(),
        SynthDriver.PitchSetting(),
        SynthDriver.VolumeSetting()
    ]''', '''    supportedSettings = [
        SynthDriver.VoiceSetting(),
        DriverSetting("secondaryVoice", "&Secondary voice (for non-Arabic text)", defaultVal="auto", availableInSettingsRing=True),
        SynthDriver.RateSetting(),
        SynthDriver.RateBoostSetting(),
        SynthDriver.PitchSetting(),
        SynthDriver.VolumeSetting(),
        NumericDriverSetting("clarity", "Clarity (postfilter)", defaultVal=50, availableInSettingsRing=True),
        DriverSetting("breathiness", "Breathiness", defaultVal="b0", availableInSettingsRing=True),
        BooleanDriverSetting("sharpening", "Sharpening (crisper sound)", defaultVal=False, availableInSettingsRing=True),
    ]''')
rep('''class SynthDriver(SynthDriver):''', '''import time
_reload_timer = None


def _fasih_has_sharp():
    """Sharpening variants are left out of the released add-on (they double its size); the
    setting is shown only when a build from source included them."""
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "variants")
    try:
        return any(os.path.isfile(os.path.join(d, v, "sharp.patch")) for v in os.listdir(d))
    except OSError:
        return False
_MIXED = [("auto", "Automatic: Latin text as written, by the secondary voice"),
          ("arabic", "Arabic first: numbers, symbols and Roman numerals in Arabic"),
          ("secondary", "Secondary voice first: numbers too")]
# Diacritizer choices. The second engine's display name is only in this line, the translation
# list below and the .po files, so a rename is quick; "shams" is just the internal id.
_DIAC = [("rawi", "Rawi (most accurate)"),
         ("shams", "Barq (fastest)")]
_BREATH = [("b0", "Natural (recommended)"), ("b6", "Less"), ("b12", "Low"), ("b20", "Very low")]


# Quality settings are written by one worker thread. _pending_quality is the complete
# wanted state; it is kept until the files really hold that exact state (a newer change
# made meanwhile keeps it pending), so a change never reads stale values from disk.
_pending_quality = None
_quality_version = 0
_quality_lock = threading.Lock()
_quality_wake = threading.Event()
_quality_thread = None


def _quality_state():
    """Current quality settings: a change waiting to be written, else what the files hold."""
    with _quality_lock:
        if _pending_quality is not None:
            return dict(_pending_quality)
    return _quality_file_state()


def _quality_worker(on_done):
    global _pending_quality
    while True:
        _quality_wake.wait()
        _quality_wake.clear()
        with _quality_lock:
            want = dict(_pending_quality) if _pending_quality is not None else None
            ver = _quality_version
        if want is None:
            continue
        try:
            ok = _apply_quality(want)
        except Exception:
            log.error("Fasih: cannot apply quality setting", exc_info=True)
            ok = False
        with _quality_lock:
            if ok and _quality_version == ver:
                _pending_quality = None          # the files now hold exactly this state
        if ok:
            on_done()


def _atomic_write(dst, data):
    """Write a file so it is either the old or the new content, never half of each.
    Windows refuses to replace a file another program has open: try again for a while,
    then give up (the old file stays whole and the change stays pending)."""
    tmp = dst + ".new"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    for _i in range(20):
        try:
            os.replace(tmp, dst)
            return
        except OSError:
            time.sleep(0.1)
    try:
        os.remove(tmp)
    except OSError:
        pass
    raise OSError("cannot replace " + dst)


def _quality_file_state():
    """Quality settings as stored in the voice files."""
    st = {"clarity": 50, "breathiness": "b0", "sharpening": False, "repair": False, "ms": True}
    try:
        with open(os.path.join(data_dir, "quality.txt"), encoding="utf-8") as f:
            for line in f:
                k, _, v = line.strip().partition("=")
                if k == "clarity":
                    st[k] = int(v)
                elif k == "breathiness":
                    st[k] = v
                elif k in ("sharpening", "repair", "ms"):
                    st[k] = v == "1"
    except Exception:
        pass
    return st


def _stamp(st):
    return ("clarity=%d\\nbreathiness=%s\\nsharpening=%d\\nrepair=%d\\nms=%d\\n" % (st["clarity"], st["breathiness"], 1 if st["sharpening"] else 0, 1 if st.get("repair") else 0, 1 if st.get("ms", True) else 0)).encode()


def _voice_dirs():
    """(name, voice dir, variants dir, is_pack): the bundled voices, then the Fasih voice packs
    installed from Tools > Fasih > Download voices (add-ons named RHVoice-voice-fasih-*)."""
    vdir = os.path.join(data_dir, "voices")
    for name in os.listdir(vdir):
        yield name, os.path.join(vdir, name), os.path.join(data_dir, "variants", name), False
    try:
        for addon in addonHandler.getRunningAddons():
            if addon.name.lower().startswith("rhvoice-voice-fasih-"):
                yield addon.name, os.path.join(addon.path, "data"), os.path.join(addon.path, "variants"), True
    except Exception:
        log.error("Fasih: cannot list the voice packs", exc_info=True)


def _write_voice(st, name, vd, vdir2):
    var = os.path.join(vdir2, "base.data")
    if not os.path.isfile(var):
        return False                      # e.g. the English voice keeps its own files
    beta = st["clarity"] * 0.6 / 100.0
    with open(var, "rb") as f:
        b = bytearray(f.read())
    patches = []
    if st["breathiness"] != "b0":
        patches.append(st["breathiness"] + ".patch")
    if st["sharpening"]:
        patches.append("sharp.patch")
    if st.get("repair"):
        patches.append("repair.patch")      # last: it also covers spectra Sharpening changed
    for pn in patches:
        pf = os.path.join(vdir2, pn)
        if not os.path.isfile(pf):
            continue
        with open(pf, "rb") as f:
            pd = f.read()
        if pd[:4] != b"FPAT":
            continue
        (cnt,) = struct.unpack_from("<I", pd, 4)
        pos = 8
        for _i in range(cnt):
            off, ln = struct.unpack_from("<QI", pd, pos)
            pos += 12
            b[off:off + ln] = pd[pos:pos + ln]
            pos += ln
    os.makedirs(os.path.join(vd, "24000"), exist_ok=True)
    # every file is replaced in one step: never a half-written voice
    _atomic_write(os.path.join(vd, "24000", "voice.data"), bytes(b))
    gain = 0.7
    try:
        with open(os.path.join(vdir2, "gain.txt")) as gf:
            gain = float(gf.read().strip())
    except Exception:
        pass
    _atomic_write(os.path.join(vd, "voice.params"), ("beta=%.3f\\ngain=%.3f\\n" % (beta, gain)).encode())
    # modulation postfilter: the engine applies ms_gain.txt when it sits next to voice.data
    msrc = os.path.join(vdir2, "ms_gain.txt")
    mdst = os.path.join(vd, "24000", "ms_gain.txt")
    try:
        if st.get("ms", True) and os.path.isfile(msrc):
            with open(msrc, "rb") as f:
                _atomic_write(mdst, f.read())
        elif os.path.exists(mdst):
            os.remove(mdst)
    except OSError:
        log.error("Fasih: cannot update the modulation postfilter of %s" % name, exc_info=True)
    _atomic_write(os.path.join(vdir2, "quality.txt"), _stamp(st))
    return True


def _pack_state_ok(st, vd, vdir2):
    try:
        with open(os.path.join(vdir2, "quality.txt"), "rb") as f:
            return f.read() == _stamp(st) and os.path.isfile(os.path.join(vd, "24000", "voice.data"))
    except OSError:
        return False


def _apply_quality(st, packs_only=False):
    """Write the voice files for these settings. Each voice pack is checked against its own
    stamp, so a pack installed later gets the current settings. True if anything changed."""
    core = not packs_only and st != _quality_file_state()
    changed = False
    for name, vd, vdir2, is_pack in _voice_dirs():
        if is_pack:
            if not _pack_state_ok(st, vd, vdir2):
                changed = _write_voice(st, name, vd, vdir2) or changed
        elif core:
            changed = _write_voice(st, name, vd, vdir2) or changed
    if core:
        _atomic_write(os.path.join(data_dir, "quality.txt"), _stamp(st))
        changed = True
    return changed


class SynthDriver(SynthDriver):''')
rep('''        self.__tts_queue.put(None)
        self.__tts_thread.join()''', '''        self.__tts_queue.put(None)
        self.__tts_thread.join(1.0)        # never hang NVDA if the worker is stuck; it is a daemon thread
        if self.__tts_thread.is_alive():
            log.error("Fasih: speech thread did not stop; leaving the engine to it")
            return''')
rep('''    def terminate(self):''', '''    # quality settings: rewrite voice files, then reload this synth so the engine reads them
    def _quality_set(self, key, value):
        # The setting is only recorded here. The voice files are rewritten once, 1.5 s after
        # the last change (several quick changes in the settings ring -> one write, one reload),
        # and the synth is reloaded only if Fasih is still the active synthesizer.
        global _pending_quality, _quality_version, _quality_thread, _reload_timer
        st = _quality_state()
        st[key] = value
        with _quality_lock:
            _pending_quality = st
            _quality_version += 1
        name = self.name
        try:
            import wx

            def reload():
                try:
                    import synthDriverHandler
                    cur = synthDriverHandler.getSynth()
                    if cur is not None and cur.name == name:
                        synthDriverHandler.setSynth(name)
                except Exception:
                    log.error("Fasih: cannot reload after a quality setting", exc_info=True)

            if _quality_thread is None:
                # the voice files are prepared off NVDA's main thread; only the reload runs there
                _quality_thread = threading.Thread(target=_quality_worker, args=(lambda: wx.CallAfter(reload),),
                                                   name="FasihQuality", daemon=True)
                _quality_thread.start()

            def commit():
                _quality_wake.set()
            try:
                if _reload_timer is not None and _reload_timer.IsRunning():
                    _reload_timer.Stop()
            except Exception:
                pass
            _reload_timer = wx.CallLater(1500, commit)
        except Exception:
            log.error("Fasih: cannot schedule quality setting", exc_info=True)

    def _get_clarity(self):
        return _quality_state()["clarity"]

    def _set_clarity(self, v):
        self._quality_set("clarity", int(v))

    def _get_breathiness(self):
        return _quality_state()["breathiness"]

    def _set_breathiness(self, v):
        if v in dict(_BREATH):
            self._quality_set("breathiness", v)

    def _get_availableBreathinesss(self):
        return OrderedDict((k, StringParameterInfo(k, n)) for k, n in _BREATH)

    def _get_sharpening(self):
        return _quality_state()["sharpening"]

    def _set_sharpening(self, v):
        self._quality_set("sharpening", bool(v))

    def _get_consonantRepair(self):
        return _quality_state()["repair"]

    def _set_consonantRepair(self, v):
        self._quality_set("repair", bool(v))

    def _get_modulationPostfilter(self):
        return _quality_state().get("ms", True)

    def _set_modulationPostfilter(self, v):
        self._quality_set("ms", bool(v))

    def terminate(self):''')
# 2) PlanetBlindTech audio post-filter (DC/rumble high-pass + soft limiter) in the audio path
rep('''    def do_play(self, data, index=None):
        player = self.get_player()
        if player is not None and not self.__cancel_flag.is_set():''', '''    def do_play(self, data, index=None):
        player = self.get_player()
        if player is not None and not self.__cancel_flag.is_set():
            if _post_filter is not None and data:
                try:
                    data = _post_filter.process(data)
                except Exception:
                    pass
            if not data and index is not None:
                data = bytes(48)              # trimmed to nothing: keep NVDA's index callback''')
rep('''    def on_new_message(self):
        self.__prev_data = None''', '''    def on_new_message(self):
        self.__prev_data = None
        if _post_filter is not None:
            _post_filter.reset()''')
rep('''from . import arabic_pipeline''', '''from . import arabic_pipeline
try:
    # high-pass (after PlanetBlindTech's post-filter) + make-up gain + look-ahead limiter,
    # pure Python: louder output without clipping, and no numpy inside NVDA
    from . import loudness as _loud
    _post_filter = _loud.Output(6.0)
except Exception:
    _post_filter = None''')
# 3) settings: read emojis, pausal ending; dictionaries
rep('''        BooleanDriverSetting("sharpening", "Sharpening (crisper sound)", defaultVal=False, availableInSettingsRing=True),''', '''        BooleanDriverSetting("sharpening", "Sharpening (crisper sound)", defaultVal=False, availableInSettingsRing=True),
        BooleanDriverSetting("consonantRepair", "Consonant repair (crisper ق ط ش, may sound muffled)", defaultVal=False, availableInSettingsRing=True),
        BooleanDriverSetting("modulationPostfilter", "Modulation postfilter (livelier, less muffled)", defaultVal=True, availableInSettingsRing=True),
        BooleanDriverSetting("readEmojis", "Read emojis and symbols by name", defaultVal=True),
        BooleanDriverSetting("pausalEnding", "Pausal ending (waqf) at end of sentences", defaultVal=True),
        BooleanDriverSetting("fullCaseEndings", "Full case endings (classical reading)", defaultVal=False, availableInSettingsRing=True),
        DriverSetting("mixedPriority", "Mixed Arabic and Latin text", defaultVal="auto", availableInSettingsRing=True),
        DriverSetting("diacritizer", "Diacritizer", defaultVal="rawi", availableInSettingsRing=True),
        BooleanDriverSetting("trimSilence", "Start speaking without leading silence", defaultVal=True),''')
rep('''    def terminate(self):''', '''    def _get_readEmojis(self):
        return arabic_pipeline.options["read_emojis"]

    def _set_readEmojis(self, v):
        arabic_pipeline.options["read_emojis"] = bool(v)

    def _get_fullCaseEndings(self):
        return arabic_pipeline.options.get("full_case", False)

    def _set_fullCaseEndings(self, v):
        arabic_pipeline.options["full_case"] = bool(v)

    def _get_pausalEnding(self):
        return arabic_pipeline.options["pausal_ending"]

    def _set_pausalEnding(self, v):
        arabic_pipeline.options["pausal_ending"] = bool(v)

    def _get_availableMixedprioritys(self):
        return OrderedDict((k, StringParameterInfo(k, n)) for k, n in _MIXED)

    def _get_mixedPriority(self):
        return arabic_pipeline.options.get("mixed_priority", "auto")

    def _set_mixedPriority(self, v):
        if v in dict(_MIXED):
            arabic_pipeline.options["mixed_priority"] = v

    def _get_trimSilence(self):
        return _post_filter is not None and _post_filter.trim_enabled

    def _set_trimSilence(self, v):
        if _post_filter is not None:
            _post_filter.trim_enabled = bool(v)

    def _get_availableDiacritizers(self):
        return OrderedDict((k, StringParameterInfo(k, n)) for k, n in _DIAC)

    def _get_diacritizer(self):
        return arabic_pipeline.options.get("diacritizer", "rawi")

    def _set_diacritizer(self, v):
        if v in dict(_DIAC):
            arabic_pipeline.set_diacritizer(v)

    def terminate(self):''')
rep('''        arabic_pipeline.preload()''', '''        arabic_pipeline.init_dictionaries(config_path)
        arabic_pipeline.preload()''')
# --- 1.1: the output stage holds back 2 ms (look-ahead); release it at the end
rep('''    def stop(self):''', '''    def _flush_output(self):
        if _post_filter is None:
            return
        player = self.get_player()
        if player is None or self.__cancel_flag.is_set():
            return
        try:
            rest = _post_filter.flush()
        except Exception:
            rest = b""
        if rest:
            player.feed(rest)

    def stop(self):''')
# --- 1.1: one "done" per utterance, and no dead air -------------------------------------
# An utterance may be several engine messages (Arabic part, English part...). Only the last
# one may tell NVDA that speech is done or let the audio device go idle.
rep('''            self.__player.on_done()
            self.__player.idle()
            if self.__cancel_flag.is_set():
                return
            synthDoneSpeaking.notify(synth=self.__synth)''', '''            self.__player.on_done()
            if not self.__player.final:
                return
            self.__player.idle()
            if self.__cancel_flag.is_set():
                return
            synthDoneSpeaking.notify(synth=self.__synth)''')
rep('''        if msg:
            self.__player.on_new_message()''', '''        if msg:
            self.__player.final = getattr(self, "final", True)
            self.__player.on_new_message()''')
# Silence gate: the engine starts every message with ~40 ms of silence and ends it with
# ~130 ms. The leading part is dropped (speech starts sooner), and the trailing part is
# shortened (and nearly removed between the parts of one utterance).
rep('''    def play(self, data):
        if self.__prev_data is None:''', '''    final = True
    _TH = 50            # 16-bit amplitude under which a sample counts as silence

    def _gate(self, data):
        a = array.array("h")
        a.frombytes(data[:len(data) & ~1])
        if not a:
            return b""
        quiet = max(a) < self._TH and min(a) > -self._TH
        if self.__lead:
            if quiet:
                return b""
            th = self._TH
            i = next(k for k, v in enumerate(a) if v >= th or v <= -th)
            i = max(0, i - self.__sample_rate // 125)          # keep 8 ms before the first sound
            self.__lead = False
            return a[i:].tobytes()
        if quiet:
            self.__held.append(data)
            return b""
        if self.__held:
            data = b"".join(self.__held) + data
            self.__held = []
        return data

    def _tail(self):
        # what is kept of the silence that ended the message
        held, self.__held = b"".join(self.__held), []
        keep = (self.__sample_rate * (60 if self.final else 15) // 1000) * 2
        return held[:keep]

    def play(self, data):
        try:
            data = self._gate(data)
        except Exception:
            pass
        if not data:
            return
        if self.__prev_data is None:''')
rep('''    def on_new_message(self):
        self.__prev_data = None''', '''    def on_new_message(self):
        self.__lead = True
        self.__held = []
        self.__prev_data = None''')
rep('''    def on_done(self):
        if self.__prev_data is None:
            return
        self.do_play(self.__prev_data)''', '''    def on_done(self):
        tail = self._tail()
        if self.__prev_data is not None:
            self.do_play(self.__prev_data + tail)
        self._flush_output()''')
rep('''        self.__players = {}
        self.__lock = threading.Lock()''', '''        self.__players = {}
        self.__lead = True
        self.__held = []
        self.__prev_data = None
        self.__lock = threading.Lock()''')
rep('''    def on_index(self, index):
        data = self.__prev_data
        self.__prev_data = None
        if data is None:
            data = b""''', '''    def on_index(self, index):
        data = self.__prev_data
        self.__prev_data = None
        if data is None:
            data = b""
        if self.__held and not self.__lead:      # keep pauses in step with the index marks
            data += b"".join(self.__held)
            self.__held = []''')
rep('''import languageHandler
''', '''import languageHandler
import array
import struct
''')
# --- 1.1: load voices before they are first needed ----------------------------------------
# RHVoice reads a voice's model on its first use (about 0.2 s), which delayed the first thing
# said after choosing a voice, and the first English word. A silent warm-up does it ahead.
rep('''    def play(self, data):
        try:
            data = self._gate(data)''', '''    mute = False

    def play(self, data):
        if self.mute:
            return
        try:
            data = self._gate(data)''')
rep('''        voice = voice.split("+")[0]          # profiles saved by earlier builds, e.g. "Jamal+SLT"
        if voice in self.__profiles:
            self.__profile = voice''', '''        voice = voice.split("+")[0]          # profiles saved by earlier builds, e.g. "Jamal+SLT"
        if voice in self.__profiles:
            changed = voice != self.__profile
            self.__profile = voice
            if changed:
                self._warm(voice)
        elif voice in _MOVED_TO_PACKS:
            # a voice from 1.2 that is now a download: speak with the default
            # voice and let the Fasih menu tell the user once where it went
            try:
                os.makedirs(config_path, exist_ok=True)
                with open(os.path.join(config_path, "missing_voice.txt"), "w", encoding="utf-8") as f:
                    f.write(voice)
            except Exception:
                pass''')
rep('''config_path = os.path.join(globalVars.appArgs.configPath, "Fasih-config")''', '''config_path = os.path.join(globalVars.appArgs.configPath, "Fasih-config")
_MOVED_TO_PACKS = ("Jamal", "Nour", "Sami", "Faris", "Huda", "Karim", "Rima", "Lamees", "Zayd", "Reem")''')
rep('''    def _set_secondaryVoice(self, v):
        self._secondary = v if v in self._get_availableSecondaryvoices() else "auto"''', '''    def _set_secondaryVoice(self, v):
        self._secondary = v if v in self._get_availableSecondaryvoices() else "auto"
        sv = self._secondary_profile()
        if sv:
            self._warm(sv)

    def _warm(self, *voices):
        player = self.__player

        def job():
            for v in voices:
                if self.__cancel_flag.is_set():
                    break
                player.mute = True
                try:
                    lang = self.__voice_languages.get(v, "ar")
                    text = "<speak>%s</speak>" % ("\\u0644\\u0627" if lang.startswith("ar") else "a")
                    task = SpeakText(self.__lib, self.__tts_engine, text, self.__cancel_flag, player)
                    task.final = False
                    task.set_voice_profile(v)
                    task()
                except Exception:
                    log.debugWarning("Fasih: warm-up of %s failed" % v, exc_info=True)
                finally:
                    player.mute = False
        self.__tts_queue.put(job)''')
rep('''        arabic_pipeline.preload()''', '''        arabic_pipeline.preload()
        self._warm(*[v for v in (self.__profile, self._secondary_profile()) if v])''')
# --- 1.1: translations (locale/<lang>/LC_MESSAGES/nvda.mo) ---------------------------------
s = s.replace("import addonHandler\n", "import addonHandler\ntry:\n    addonHandler.initTranslation()\nexcept Exception:\n    pass\ntry:\n    _\nexcept NameError:\n    def _(text):\n        return text\n", 1)
for _lit in ["&Secondary voice (for non-Arabic text)", "Clarity (postfilter)", "Breathiness",
             "Sharpening (crisper sound)", "Consonant repair (crisper ق ط ش, may sound muffled)", "Modulation postfilter (livelier, less muffled)", "Read emojis and symbols by name",
             "Pausal ending (waqf) at end of sentences", "Full case endings (classical reading)", "Mixed Arabic and Latin text",
             "Natural (recommended)", "Less", "Low", "Very low",
             "Automatic: Latin text as written, by the secondary voice",
             "Arabic first: numbers, symbols and Roman numerals in Arabic",
             "Secondary voice first: numbers too",
             "Start speaking without leading silence",
             "Diacritizer", "Rawi (most accurate)", "Barq (fastest)",
             "Automatic (a voice in NVDA's language, else English)"]:
    _q = '"%s"' % _lit
    assert _q in s, "string not found: " + _lit
    s = s.replace(_q, "_(%s)" % _q)
_end = """
    ]

    supportedCommands = {"""
assert s.count(_end) == 1, "end of settings list not found"
s = s.replace(_end, """
    ]
    # Sharpening only when its variants were built in (they are left out of the released add-on)
    supportedSettings = [st for st in supportedSettings if st.id != "sharpening" or _fasih_has_sharp()]

    supportedCommands = {""")
compile(s, dst, "exec")  # fail the build if the driver does not parse
open(dst, "w", encoding="utf-8").write(s)
print("patched driver written")
