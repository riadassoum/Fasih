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
            None,''', '''        init_params = RHVoice_init_params(
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
        rate, pitch, volume, boost = self.__rate, self.__pitch, self.__volume, self.__rate_boost
        langs = self.__voice_languages

        def job():
            segs = []          # [voice, items]
            state = {}         # last pitch/rate/volume/char-mode command, re-sent when the voice changes
            char_mode = False

            def emit(voice, item):
                if not segs or segs[-1][0] != voice:
                    if segs and not any(isinstance(x, str) and x.strip() for x in segs[-1][1]):
                        segs[-1][0] = voice      # nothing spoken yet: just relabel
                    else:
                        segs.append([voice, list(state.values())])
                segs[-1][1].append(item)

            for item in seq:
                if isinstance(item, LangChangeCommand):
                    continue                     # the script decides the voice
                if isinstance(item, str):
                    try:
                        new = arabic_pipeline.process(item, diacritize=not char_mode)
                    except Exception:
                        log.error("Fasih: text pipeline failed", exc_info=True)
                        new = item
                    if char_mode and new != item:
                        # a letter/symbol name: read it as a word, then return to character mode
                        emit(primary, CharacterModeCommand(False))
                        emit(primary, new)
                        emit(primary, CharacterModeCommand(True))
                        continue
                    for is_ar, chunk in _split_by_script(new):
                        emit(primary if (is_ar or not secondary) else secondary, chunk)
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
                if segs:
                    segs[-1][1].append(item)
                else:
                    segs.append([primary, [item]])
            for i, (voice, items) in enumerate(segs):
                if self.__cancel_flag.is_set():
                    return
                conv = SsmlConverter(self, langs.get(voice, "ar"))
                task = SpeakText(self.__lib, self.__tts_engine, conv.convertToXml(items), self.__cancel_flag, self.__player)
                task.final = (i == len(segs) - 1)
                task.set_voice_profile(voice)
                task.set_rate(rate)
                task.set_pitch(pitch)
                task.set_volume(volume)
                task.configure_rate_boost(boost)
                task()

        self.__tts_queue.put(job)''')
rep('''        log.info("Using RHVoice version {}".format(self.__lib.RHVoice_get_version().decode()))''',
    '''        log.info("Using RHVoice version {}".format(self.__lib.RHVoice_get_version().decode()))
        arabic_pipeline.preload()''')
# Primary voice = an Arabic voice; secondary voice = any non-Arabic voice (default SLT)
rep('''        if self.__profile is None:
            self.__profile = self.__profiles[0]''', '''        self.__profiles = [p for p in self.__profiles if "+" not in p]
        ar = self._arabic_voices()
        if self.__profile not in ar:
            self.__profile = "Jamal" if "Jamal" in ar else (ar[0] if ar else self.__profiles[0])
        if not hasattr(self, "_secondary"):
            self._secondary = "auto"''')
rep('''class SynthDriver(SynthDriver):''', '''_AR_RANGES = ((0x0600, 0x06FF), (0x0750, 0x077F), (0x08A0, 0x08FF), (0xFB50, 0xFDFF), (0xFE70, 0xFEFF))


def _is_ar(ch):
    o = ord(ch)
    return any(a <= o <= b for a, b in _AR_RANGES)


def _split_by_script(text):
    # (is_arabic, chunk) runs, after ClaritySynth. Spaces, punctuation and digits ride
    # with the current run; marks (tashkeel) always belong to Arabic.
    runs, cur, cur_ar = [], [], None
    for ch in text:
        if _is_ar(ch) and not ch.isalpha() and not ch.isdigit() and unicodedata.category(ch) == "Mn":
            want = True
        elif ch.isalpha():
            want = _is_ar(ch)
        else:
            cur.append(ch)
            continue
        if cur_ar is None:
            cur_ar = want
        elif want != cur_ar:
            runs.append((cur_ar, "".join(cur)))
            cur, cur_ar = [], want
        cur.append(ch)
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

    def _secondary_profile(self):
        sv = getattr(self, "_secondary", "auto")
        if sv == "none":
            return ""
        if sv in self.__profiles and sv not in self._arabic_voices():
            return sv
        # automatic (or a saved voice that is no longer installed): the first non-Arabic voice found
        for p in self.__profiles:
            if p not in self._arabic_voices():
                return p
        return ""

    def _get_availableSecondaryvoices(self):
        out = OrderedDict()
        out["auto"] = StringParameterInfo("auto", "Automatic (first non-Arabic voice found)")
        out["none"] = StringParameterInfo("none", "None (primary voice reads everything)")
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
rep('''class SynthDriver(SynthDriver):''', '''_reload_timer = None
_BREATH = [("b0", "Natural (recommended)"), ("b6", "Less"), ("b12", "Low"), ("b20", "Very low")]


def _quality_state():
    """Current quality settings as stored in the voice files."""
    st = {"clarity": 50, "breathiness": "b0", "sharpening": False}
    try:
        with open(os.path.join(data_dir, "quality.txt"), encoding="utf-8") as f:
            for line in f:
                k, _, v = line.strip().partition("=")
                if k == "clarity":
                    st[k] = int(v)
                elif k == "breathiness":
                    st[k] = v
                elif k == "sharpening":
                    st[k] = v == "1"
    except Exception:
        pass
    return st


def _apply_quality(st):
    """Write the voice files for these settings. Returns True if anything changed."""
    if st == _quality_state():
        return False
    vdir = os.path.join(data_dir, "voices")
    beta = st["clarity"] * 0.6 / 100.0
    for name in os.listdir(vdir):
        vd = os.path.join(vdir, name)
        vdir2 = os.path.join(data_dir, "variants", name)
        var = os.path.join(vdir2, "base.data")
        if not os.path.isfile(var):
            continue                      # e.g. the English voice keeps its own files
        with open(var, "rb") as f:
            b = bytearray(f.read())
        patches = []
        if st["breathiness"] != "b0":
            patches.append(st["breathiness"] + ".patch")
        if st["sharpening"]:
            patches.append("sharp.patch")
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
        with open(os.path.join(vd, "24000", "voice.data"), "wb") as f:
            f.write(b)
        with open(os.path.join(vd, "voice.params"), "w", encoding="utf-8") as f:
            gain = 0.7
            try:
                with open(os.path.join(data_dir, "variants", name, "gain.txt")) as gf:
                    gain = float(gf.read().strip())
            except Exception:
                pass
            f.write("beta=%.3f\\ngain=%.3f\\n" % (beta, gain))
    with open(os.path.join(data_dir, "quality.txt"), "w", encoding="utf-8") as f:
        f.write("clarity=%d\\nbreathiness=%s\\nsharpening=%d\\n" % (st["clarity"], st["breathiness"], 1 if st["sharpening"] else 0))
    return True


class SynthDriver(SynthDriver):''')
rep('''        self.__tts_queue.put(None)
        self.__tts_thread.join()''', '''        self.__tts_queue.put(None)
        self.__tts_thread.join(1.0)        # never hang NVDA if the worker is stuck; it is a daemon thread
        if self.__tts_thread.is_alive():
            log.error("Fasih: speech thread did not stop; leaving the engine to it")
            return''')
rep('''    def terminate(self):''', '''    # quality settings: rewrite voice files, then reload this synth so the engine reads them
    def _quality_set(self, key, value):
        st = _quality_state()
        st[key] = value
        try:
            if _apply_quality(st):
                import wx, synthDriverHandler
                # one reload after the last change (several quick changes -> one reload)
                global _reload_timer
                try:
                    if _reload_timer is not None and _reload_timer.IsRunning():
                        _reload_timer.Stop()
                except Exception:
                    pass
                _reload_timer = wx.CallLater(1500, synthDriverHandler.setSynth, self.name)
        except Exception:
            log.error("Fasih: cannot apply quality setting", exc_info=True)

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
                    pass''')
rep('''    def on_new_message(self):
        self.__prev_data = None''', '''    def on_new_message(self):
        self.__prev_data = None
        if _post_filter is not None:
            _post_filter.reset()''')
rep('''from . import arabic_pipeline''', '''from . import arabic_pipeline
try:
    from .pbt import audio_filter as _af
    _post_filter = _af.AudioPostFilter()
except Exception:
    _post_filter = None''')
# 3) settings: read emojis, pausal ending; dictionaries
rep('''        BooleanDriverSetting("sharpening", "Sharpening (crisper sound)", defaultVal=False, availableInSettingsRing=True),''', '''        BooleanDriverSetting("sharpening", "Sharpening (crisper sound)", defaultVal=False, availableInSettingsRing=True),
        BooleanDriverSetting("readEmojis", "Read emojis and symbols by name", defaultVal=True),
        BooleanDriverSetting("pausalEnding", "Pausal ending (waqf) at end of sentences", defaultVal=True),''')
rep('''    def terminate(self):''', '''    def _get_readEmojis(self):
        return arabic_pipeline.options["read_emojis"]

    def _set_readEmojis(self, v):
        arabic_pipeline.options["read_emojis"] = bool(v)

    def _get_pausalEnding(self):
        return arabic_pipeline.options["pausal_ending"]

    def _set_pausalEnding(self, v):
        arabic_pipeline.options["pausal_ending"] = bool(v)

    def terminate(self):''')
rep('''        arabic_pipeline.preload()''', '''        arabic_pipeline.init_dictionaries(config_path)
        arabic_pipeline.preload()''')
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
        if self.__prev_data is None:
            return
        self.do_play(self.__prev_data + tail)''')
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
# an utterance with nothing to say must still report that it is done
rep('''            for i, (voice, items) in enumerate(segs):
                if self.__cancel_flag.is_set():
                    return''', '''            if not segs:
                synthDoneSpeaking.notify(synth=self)
                return
            for i, (voice, items) in enumerate(segs):
                if self.__cancel_flag.is_set():
                    return''')
compile(s, dst, "exec")  # fail the build if the driver does not parse
open(dst, "w", encoding="utf-8").write(s)
print("patched driver written")
