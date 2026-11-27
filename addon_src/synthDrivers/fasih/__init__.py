# Fasih's NVDA synthesizer driver.
# It began as RHVoice's NVDA driver (third_party/rhvoice-nvda-driver, copyright notices below kept)
# with Fasih's changes applied by a patch script; since Fasih 2.0 this file is the source itself
# and is edited directly. Riad Assoum and contributors, GPL (as the original).
# Copyright (C) 2010, 2011, 2012, 2013, 2018, 2019  Olga Yakovleva <yakovleva.o.v@gmail.com>
# Copyright (C) 2019, 2023, 2025  Beka Gozalishvili <beqaprogger@gmail.com>
# Copyright (C) 2022 Alexander Linkov <kvark128@yandex.ru>

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

import sys
import os.path
import queue
from collections import OrderedDict, defaultdict
import threading
import ctypes
from ctypes import c_char_p, c_wchar_p, c_void_p, c_short, c_int, c_uint, c_double, POINTER, Structure, sizeof, string_at, CFUNCTYPE, byref, cast
import re
import copy

from io import StringIO

import config
import globalVars
import nvwave
from logHandler import log
import speechXml
from synthDriverHandler import (
    SynthDriver,
    synthDoneSpeaking,
    synthIndexReached,
    VoiceInfo
)
from speech.commands import (
    CharacterModeCommand,
    IndexCommand,
    LangChangeCommand,
    PitchCommand,
    RateCommand,
    VolumeCommand
)
import languageHandler
import array
import struct
import unicodedata
from autoSettingsUtils.driverSetting import BooleanDriverSetting, DriverSetting, NumericDriverSetting
from autoSettingsUtils.utils import StringParameterInfo
import addonHandler
try:
    addonHandler.initTranslation()
except Exception:
    pass
try:
    _
except NameError:
    def _(text):
        return text

import addonAPIVersion
api_version = addonAPIVersion.CURRENT
import buildVersion

module_dir = os.path.dirname(__file__)
config_path = os.path.join(globalVars.appArgs.configPath, "Fasih-config")
_MOVED_TO_PACKS = ("Jamal", "Nour", "Sami", "Faris", "Huda", "Karim", "Rima", "Lamees", "Zayd", "Reem")
data_dir = os.path.join(module_dir, "data")
from . import arabic_pipeline
try:
    # high-pass (after PlanetBlindTech's post-filter) + make-up gain + look-ahead limiter,
    # pure Python: louder output without clipping, and no numpy inside NVDA
    from . import loudness as _loud
    _post_filter = _loud.Output(6.0)
except Exception:
    _post_filter = None


data_addon_name_pattern = re.compile("^RHVoice-.*(voice|language).*")


class RHVoice_tts_engine_struct(Structure):
    pass


RHVoice_tts_engine = POINTER(RHVoice_tts_engine_struct)


class RHVoice_message_struct(Structure):
    pass
RHVoice_message = POINTER(RHVoice_message_struct)

class RHVoice_callback_types:
    set_sample_rate = CFUNCTYPE(c_int, c_int, c_void_p)
    play_speech = CFUNCTYPE(c_int, POINTER(c_short), c_uint, c_void_p)
    process_mark = CFUNCTYPE(c_int, c_char_p, c_void_p)
    word_starts = CFUNCTYPE(c_int, c_uint, c_uint, c_void_p)
    word_ends = CFUNCTYPE(c_int, c_uint, c_uint, c_void_p)
    sentence_starts = CFUNCTYPE(c_int, c_uint, c_uint, c_void_p)
    sentence_ends = CFUNCTYPE(c_int, c_uint, c_uint, c_void_p)
    play_audio = CFUNCTYPE(c_int, c_char_p, c_void_p)
    done = CFUNCTYPE(None, c_void_p)

class RHVoice_callbacks(Structure):
    _fields_ = [
        ("set_sample_rate", RHVoice_callback_types.set_sample_rate),
        ("play_speech", RHVoice_callback_types.play_speech),
        ("process_mark", RHVoice_callback_types.process_mark),
        ("word_starts", RHVoice_callback_types.word_starts),
        ("word_ends", RHVoice_callback_types.word_ends),
        ("sentence_starts", RHVoice_callback_types.sentence_starts),
        ("sentence_ends", RHVoice_callback_types.sentence_ends),
        ("play_audio", RHVoice_callback_types.play_audio),
        ("done", RHVoice_callback_types.done)
    ]

class RHVoice_init_params(Structure):
    _fields_ = [("data_path", c_char_p),
              ("config_path", c_char_p),
              ("resource_paths", POINTER(c_char_p)),
              ("callbacks", RHVoice_callbacks),
              ("options", c_uint)]

class RHVoice_message_type:
    text = 0
    ssml = 1
    characters = 2

class RHVoice_voice_gender:
    unknown = 0
    male = 1
    female = 2

class RHVoice_voice_info(Structure):
    _fields_ = [("language", c_char_p),
              ("name", c_char_p),
              ("gender", c_int),
              ("country", c_char_p)]

class RHVoice_punctuation_mode:
    default = 0
    none = 1
    all = 2
    some = 3

class RHVoice_capitals_mode:
    default = 0
    off = 1
    word = 2
    pitch = 3
    sound = 4

class RHVoice_synth_flag:
    dont_clip_rate = 1

class RHVoice_synth_params(Structure):
    _fields_ = [("voice_profile", c_char_p),
              ("absolute_rate", c_double),
              ("absolute_pitch", c_double),
              ("absolute_volume", c_double),
              ("relative_rate", c_double),
              ("relative_pitch", c_double),
              ("relative_volume", c_double),
              ("punctuation_mode", c_int),
              ("punctuation_list", c_char_p),
              ("capitals_mode", c_int),
              ("flags", c_int)]

IMAGE_FILE_MACHINE_ARM64 = 0xAA64

def runningOnArm64():
    """Whether the host machine is ARM64, no matter what the current process is."""
    try:
        processMachine = ctypes.c_ushort()
        nativeMachine = ctypes.c_ushort()
        if not ctypes.windll.kernel32.IsWow64Process2(ctypes.windll.kernel32.GetCurrentProcess(), byref(processMachine), byref(nativeMachine)):
            return False
        return nativeMachine.value == IMAGE_FILE_MACHINE_ARM64
    except Exception:
        # IsWow64Process2 requires Windows 10 1709 or later.
        return False

def getLibraryPaths():
    """The libraries we can use, the most preferable one first."""
    if sys.maxsize > 2**32:
        # An ARM64EC library uses the x64 ABI, so a 64 bit process can load it,
        # but unlike the x64 one it is not emulated.
        archs = ["arm64ec", "x64"] if runningOnArm64() else ["x64"]
    else:
        archs = ["x86"]
    paths = [os.path.join(module_dir, "lib", arch, "RHVoice.dll") for arch in archs]
    return [path for path in paths if os.path.isfile(path)]

def load_tts_library():
    lib = None
    for path in getLibraryPaths():
        try:
            lib = ctypes.CDLL(path)
            break
        except OSError:
            log.warning("Unable to load {}, trying the next library".format(path), exc_info=True)
    if lib is None:
        raise RuntimeError("No usable RHVoice library")
    lib.RHVoice_get_version.restype = c_char_p
    lib.RHVoice_new_tts_engine.argtypes = (POINTER(RHVoice_init_params),)
    lib.RHVoice_new_tts_engine.restype = RHVoice_tts_engine
    lib.RHVoice_delete_tts_engine.argtypes = (RHVoice_tts_engine,)
    lib.RHVoice_delete_tts_engine.restype = None
    lib.RHVoice_get_number_of_voices.argtypes = (RHVoice_tts_engine,)
    lib.RHVoice_get_number_of_voices.restype = c_uint
    lib.RHVoice_get_voices.argtypes = (RHVoice_tts_engine,)
    lib.RHVoice_get_voices.restype = POINTER(RHVoice_voice_info)
    lib.RHVoice_get_number_of_voice_profiles.argtypes = (RHVoice_tts_engine,)
    lib.RHVoice_get_number_of_voice_profiles.restype = c_uint
    lib.RHVoice_get_voice_profiles.argtypes = (RHVoice_tts_engine,)
    lib.RHVoice_get_voice_profiles.restype = POINTER(c_char_p)
    lib.RHVoice_are_languages_compatible.argtypes = (RHVoice_tts_engine, c_char_p, c_char_p)
    lib.RHVoice_are_languages_compatible.restype = c_int
    lib.RHVoice_new_message.argtypes = (RHVoice_tts_engine, c_char_p, c_uint, c_int, POINTER(RHVoice_synth_params), c_void_p)
    lib.RHVoice_new_message.restype = RHVoice_message
    lib.RHVoice_delete_message.arg_types = (RHVoice_message,)
    lib.RHVoice_delete_message.restype = None
    lib.RHVoice_speak.argtypes = (RHVoice_message,)
    lib.RHVoice_speak.restype = c_int
    return lib


class AudioPlayer:
    def __init__(self, synth, cancel_flag):
        self.__synth = synth
        self.__cancel_flag = cancel_flag
        self.__sample_rate = 0
        self.__players = {}
        self.__lead = True
        self.__held = []
        self.__prev_data = None
        self.__lock = threading.Lock()
        self.__closed = False

    def do_get_player(self):
        if self.__closed:
            return None
        if self.__sample_rate == 0:
            return None
        player = self.__players.get(self.__sample_rate, None)
        if player is None:
            player = nvwave.WavePlayer(channels=1, samplesPerSec=self.__sample_rate, bitsPerSample=16, outputDevice=self.__synth.outputDeviceSection["outputDevice"])
            self.__players[self.__sample_rate] = player
        return player

    def get_player(self):
        with self.__lock:
            return self.do_get_player()

    def close(self):
        with self.__lock:
            self.__closed = True
            players = list(self.__players.values())
        for p in players:
            p.close()

    def set_sample_rate(self, sr):
        if log.isEnabledFor(log.DEBUG):
            log.debug("Fasih: engine sample rate %s (player %s)" % (sr, self.__sample_rate))
        with self.__lock:
            if self.__closed:
                return
            if self.__sample_rate == 0:
                self.__sample_rate = sr
                return
            if self.__sample_rate == sr:
                return
            old_player = self.__players.get(self.__sample_rate, None)
        if old_player is not None:
            old_player.idle()
        with self.__lock:
            self.__sample_rate = sr

    def do_play(self, data, index=None):
        player = self.get_player()
        if player is not None and not self.__cancel_flag.is_set():
            if _post_filter is not None and data:
                try:
                    data = _post_filter.process(data)
                except Exception:
                    pass
            if not data and index is not None:
                data = bytes(48)              # trimmed to nothing: keep NVDA's index callback
            if index is None:
                player.feed(data)
            else:
                player.feed(data, onDone=lambda next_index=index: synthIndexReached.notify(synth=self.__synth, index=next_index))
            if self.__cancel_flag.is_set():
                player.stop()

    final = True
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

    mute = False

    def play(self, data):
        if self.mute:
            return
        try:
            data = self._gate(data)
        except Exception:
            pass
        if not data:
            return
        if self.__prev_data is None:
            self.__prev_data = data
            return
        self.do_play(self.__prev_data)
        self.__prev_data = data

    def _flush_output(self):
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

    def stop(self):
        player = self.get_player()
        if player is not None:
            player.stop()

    def pause(self, switch):
        player = self.get_player()
        if player is not None:
            player.pause(switch)

    def idle(self):
        player = self.get_player()
        if player is not None:
            player.idle()

    def on_new_message(self):
        self.__lead = True
        self.__held = []
        self.__prev_data = None
        if _post_filter is not None:
            _post_filter.reset()

    def on_done(self):
        tail = self._tail()
        if self.__prev_data is not None:
            self.do_play(self.__prev_data + tail)
        self._flush_output()
        self.__prev_data = None

    def on_index(self, index):
        data = self.__prev_data
        self.__prev_data = None
        if data is None:
            data = b""
        if self.__held and not self.__lead:      # keep pauses in step with the index marks
            data += b"".join(self.__held)
            self.__held = []
        self.do_play(data, index)


class SampleRateCallback:
    def __init__(self, lib, player):
        self.__lib = lib
        self.__player = player

    def __call__(self, sample_rate, user_data):
        try:
            self.__player.set_sample_rate(sample_rate)
            return 1
        except Exception:
            log.error("RHVoice sample rate callback", exc_info=True)
            return 0


class SpeechCallback:
    def __init__(self, lib, player, cancel_flag):
        self.__lib = lib
        self.__player = player
        self.__cancel_flag = cancel_flag

    def __call__(self, samples, count, user_data):
        try:
            if self.__cancel_flag.is_set():
                return 0
            try:
                self.__player.play(string_at(samples, count*sizeof(c_short)))
            except Exception:
                log.debugWarning("Error feeding audio to nvWave", exc_info=True)
            return 1
        except Exception:
            log.error("RHVoice speech callback", exc_info=True)
            return 0


class MarkCallback:
    def __init__(self, lib, player):
        self.__lib = lib
        self.__player = player
        self.__lock = threading.Lock()
        self.__index = None

    @property
    def index(self):
        with self.__lock:
            return self.__index

    @index.setter
    def index(self, value):
        with self.__lock:
            self.__index = value

    def __call__(self, name, user_data):
        try:
            index = int(name)
            self.__player.on_index(index)
            return 1
        except Exception:
            log.error("RHVoice mark callback", exc_info=True)
            return 0


class DoneCallback:
    def __init__(self, synth, lib, player, cancel_flag):
        self.__synth = synth
        self.__lib = lib
        self.__player = player
        self.__cancel_flag = cancel_flag

    def __call__(self, user_data):
        try:
            if self.__cancel_flag.is_set():
                return
            self.__player.on_done()
            if not self.__player.final:
                return
            self.__player.idle()
            if self.__cancel_flag.is_set():
                return
            synthDoneSpeaking.notify(synth=self.__synth)
        except Exception:
            log.error("RHVoice done callback", exc_info=True)


class SpeakText:
    def __init__(self, lib, tts_engine, text, cancel_flag, player):
        self.__lib = lib
        self.__tts_engine = tts_engine
        self.__text = text.encode("utf-8", errors="ignore")
        self.__cancel_flag = cancel_flag
        self.__player = player
        self.__synth_params = RHVoice_synth_params(
            voice_profile=None,
            absolute_rate=0,
            relative_rate=1,
            absolute_pitch=0,
            relative_pitch=1,
            absolute_volume=0,
            relative_volume=1,
            punctuation_mode=RHVoice_punctuation_mode.default,
            punctuation_list=None,
            capitals_mode=RHVoice_capitals_mode.default,
            flags=0
        )

    def set_rate(self, rate):
        self.__synth_params.absolute_rate = rate/50.0-1

    def set_pitch(self, pitch):
        self.__synth_params.absolute_pitch = pitch/50.0-1

    def set_volume(self, volume):
        self.__synth_params.absolute_volume = volume/50.0-1

    def set_voice_profile(self, name):
        self.__synth_params.voice_profile = name.encode("utf-8")

    def configure_rate_boost(self, flag):
        if not flag:
            return
        self.__synth_params.relative_rate = 2.5
        self.__synth_params.flags |= RHVoice_synth_flag.dont_clip_rate

    def __call__(self):
        if self.__cancel_flag.is_set():
            return
        msg = self.__lib.RHVoice_new_message(
            self.__tts_engine,
            self.__text,
            len(self.__text),
            RHVoice_message_type.ssml,
            byref(self.__synth_params),
            None
        )
        if msg:
            self.__player.final = getattr(self, "final", True)
            self.__player.on_new_message()
            self.__lib.RHVoice_speak(msg)
            if getattr(self, "final", True):
                self.__player.idle()
            self.__lib.RHVoice_delete_message(msg)


class TTSThread(threading.Thread):
    def __init__(self, tts_queue):
        self.__queue = tts_queue
        threading.Thread.__init__(self)
        self.daemon = True

    def run(self):
        while True:
            try:
                task = self.__queue.get()
                if task is None:
                    break
                else:
                    task()
            except Exception:
                log.error("RHVoice: error while executing a tts task", exc_info=True)


class SsmlConverter(speechXml.SsmlConverter):
    """ This class removes xml:lang attribute from ssml string to make profiles work correctly."""

    def __init__(self, synth, defaultLanguage):
        self.synth = synth
        super().__init__(defaultLanguage)

    def generateBalancerCommands(self, speechSequence):
        attrs = OrderedDict((("version", "1.0"), ("xmlns", "http://www.w3.org/2001/10/synthesis")))
        yield speechXml.EncloseAllCommand("speak", attrs)
        for command in super(speechXml.SsmlConverter, self).generateBalancerCommands(speechSequence):
            yield command

    def convertLangChangeCommand(self, command):
        lang=None
        if command is None:
            return
        if not command.lang:
            return
        lang="_".join(command.lang.split("_")[:2])
        if lang not in self.synth._SynthDriver__languages:
            return
        if self.synth._SynthDriver__languages_match(lang,self.synth._SynthDriver__voice_languages[self.synth._SynthDriver__profile.split("+")[0]]):
            return speechXml.DelAttrCommand("voice", "xml:lang")
        lang = speechXml.toXmlLang(lang)
        return speechXml.SetAttrCommand("voice", "xml:lang", lang)


_AR_RANGES = ((0x0600, 0x06FF), (0x0750, 0x077F), (0x08A0, 0x08FF), (0xFB50, 0xFDFF), (0xFE70, 0xFEFF))


def _is_ar(ch):
    o = ord(ch)
    return any(a <= o <= b for a, b in _AR_RANGES)


_SENT_END = re.compile("[.!?\u061f\u06d4]+[\"'\u00bb\u201d)\\]]*(?:\\s+|$)|\n\\s*")
_SOFT_CUT = re.compile("[,;:\u060c\u061b]\\s+|\\s+")
_SENT_END_RUN = re.compile("[.!?\u061f\u06d4](?:\\s*[.!?\u061f\u06d4])*[\"'\u00bb\u201d)\\]]*(?:\\s+|$)|\n\\s*")


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


import time
_reload_timer = None


def _fasih_has_sharp():
    """Sharpening variants are left out of the released add-on (they double its size); the
    setting is shown only when a build from source included them."""
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "variants")
    try:
        return any(os.path.isfile(os.path.join(d, v, "sharp.patch")) for v in os.listdir(d))
    except OSError:
        return False
_MIXED = [("auto", _("Automatic: Latin text as written, by the secondary voice")),
          ("arabic", _("Arabic first: numbers, symbols and Roman numerals in Arabic")),
          ("secondary", _("Secondary voice first: numbers too"))]
# Diacritizer choices. The second engine's display name is only in this line, the translation
# list below and the .po files, so a rename is quick; "shams" is just the internal id.
_DIAC = [("rawi", _("Rawi (most accurate)")),
         ("shams", _("Barq (fastest)"))]
_BREATH = [("b0", _("Natural (recommended)")), ("b6", _("Less")), ("b12", _("Low")), ("b20", _("Very low"))]


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
    return ("clarity=%d\nbreathiness=%s\nsharpening=%d\nrepair=%d\nms=%d\n" % (st["clarity"], st["breathiness"], 1 if st["sharpening"] else 0, 1 if st.get("repair") else 0, 1 if st.get("ms", True) else 0)).encode()


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
    _atomic_write(os.path.join(vd, "voice.params"), ("beta=%.3f\ngain=%.3f\n" % (beta, gain)).encode())
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


class SynthDriver(SynthDriver):
    name = "fasih"
    description = "Fasih (فصيح), RHVoice Arabic"

    supportedSettings = [
        SynthDriver.VoiceSetting(),
        DriverSetting("secondaryVoice", _("&Secondary voice (for non-Arabic text)"), defaultVal="auto", availableInSettingsRing=True),
        SynthDriver.RateSetting(),
        SynthDriver.RateBoostSetting(),
        SynthDriver.PitchSetting(),
        SynthDriver.VolumeSetting(),
        NumericDriverSetting("clarity", _("Clarity (postfilter)"), defaultVal=50, availableInSettingsRing=True),
        DriverSetting("breathiness", _("Breathiness"), defaultVal="b0", availableInSettingsRing=True),
        BooleanDriverSetting("sharpening", _("Sharpening (crisper sound)"), defaultVal=False, availableInSettingsRing=True),
        BooleanDriverSetting("consonantRepair", _("Consonant repair (crisper ق ط ش, may sound muffled)"), defaultVal=False, availableInSettingsRing=True),
        BooleanDriverSetting("modulationPostfilter", _("Modulation postfilter (livelier, less muffled)"), defaultVal=True, availableInSettingsRing=True),
        BooleanDriverSetting("readEmojis", _("Read emojis and symbols by name"), defaultVal=True),
        BooleanDriverSetting("pausalEnding", _("Pausal ending (waqf) at end of sentences"), defaultVal=True),
        BooleanDriverSetting("fullCaseEndings", _("Full case endings (classical reading)"), defaultVal=False, availableInSettingsRing=True),
        DriverSetting("mixedPriority", _("Mixed Arabic and Latin text"), defaultVal="auto", availableInSettingsRing=True),
        DriverSetting("diacritizer", _("Diacritizer"), defaultVal="rawi", availableInSettingsRing=True),
        BooleanDriverSetting("trimSilence", _("Start speaking without leading silence"), defaultVal=True),
    ]
    # Sharpening only when its variants were built in (they are left out of the released add-on)
    supportedSettings = [st for st in supportedSettings if st.id != "sharpening" or _fasih_has_sharp()]

    supportedCommands = {
        CharacterModeCommand,
        IndexCommand,
        LangChangeCommand,
        PitchCommand,
        RateCommand,
        VolumeCommand
	}
    supportedNotifications = {synthIndexReached, synthDoneSpeaking}

    @classmethod
    def check(cls):
        return os.path.isdir(os.path.join(data_dir, "voices"))

    def __languages_match(self, code1, code2, full=True):
        lang1 = code1.split("_")
        lang2 = code2.split("_")
        if lang1[0] != lang2[0]:
            return False
        if len(lang1) < 2 or len(lang2) < 2:
            return True
        if not full:
            return True
        return (lang1[1] == lang2[1])

    def __get_resource_paths(self):
        # our own voices/languages come through data_path; voices installed as
        # official RHVoice voice add-ons (e.g. via RHVoice Voice Manager) are added here
        return [os.path.join(addon.path, name).encode("utf-8")
            for addon in addonHandler.getRunningAddons()
            if data_addon_name_pattern.match(addon.name)
            for name in ["data", "langdata", "lang2data"]
            if os.path.isdir(os.path.join(addon.path, name))]

    def __init__(self):
        self.__lib = load_tts_library()
        self.__cancel_flag = threading.Event()
        self.outputDeviceSection = config.conf["speech"] if buildVersion.version_year < 2025 else config.conf["audio"]
        self.__player = AudioPlayer(self, self.__cancel_flag)
        self.__sample_rate_callback = SampleRateCallback(self.__lib, self.__player)
        self.__c_sample_rate_callback = RHVoice_callback_types.set_sample_rate(self.__sample_rate_callback)
        self.__speech_callback = SpeechCallback(self.__lib,self.__player, self.__cancel_flag)
        self.__c_speech_callback = RHVoice_callback_types.play_speech(self.__speech_callback)
        self.__mark_callback = MarkCallback(self.__lib, self.__player)
        self.__c_mark_callback = RHVoice_callback_types.process_mark(self.__mark_callback)
        self.__done_callback = DoneCallback(self, self.__lib, self.__player, self.__cancel_flag)
        self.__c_done_callback = RHVoice_callback_types.done(self.__done_callback)
        resource_paths = self.__get_resource_paths()
        c_resource_paths = (c_char_p*(len(resource_paths)+1))(*(resource_paths+[None]))
        try:
            _apply_quality(_quality_state(), packs_only=True)   # packs installed since the last start
        except Exception:
            log.error("Fasih: cannot prepare the voice packs", exc_info=True)
        init_params = RHVoice_init_params(
            data_dir.encode("utf-8"),
            config_path.encode("utf-8"),
            c_resource_paths,
            RHVoice_callbacks(
                self.__c_sample_rate_callback,
                self.__c_speech_callback,
                self.__c_mark_callback,
                cast(None, RHVoice_callback_types.word_starts),
                cast(None, RHVoice_callback_types.word_ends),
                cast(None, RHVoice_callback_types.sentence_starts),
                cast(None, RHVoice_callback_types.sentence_ends),
                cast(None, RHVoice_callback_types.play_audio),
                self.__c_done_callback
            ),
            0
        )
        self.__tts_engine = self.__lib.RHVoice_new_tts_engine(byref(init_params))
        if not self.__tts_engine:
            raise RuntimeError("RHVoice: initialization error")
        nvda_language = languageHandler.getLanguage()
        number_of_voices = self.__lib.RHVoice_get_number_of_voices(self.__tts_engine)
        native_voices = self.__lib.RHVoice_get_voices(self.__tts_engine)
        self.__voice_languages = dict()
        self.__languages = set()
        for i in range(number_of_voices):
            native_voice = native_voices[i]
            voice_language = native_voice.language.decode("utf-8")
            if native_voice.country:
                self.__languages.add(voice_language)
                voice_language = voice_language+"_"+native_voice.country.decode("utf-8")
            self.__voice_languages[native_voice.name.decode("utf-8")] = voice_language
            self.__languages.add(voice_language)
        self.__profile = None
        self.__profiles = list()
        number_of_profiles = self.__lib.RHVoice_get_number_of_voice_profiles(self.__tts_engine)
        native_profile_names = self.__lib.RHVoice_get_voice_profiles(self.__tts_engine)
        for i in range(number_of_profiles):
            name = native_profile_names[i].decode("utf-8")
            self.__profiles.append(name)
            if (self.__profile is None) and self.__languages_match(nvda_language, self.__voice_languages[name.split("+")[0]]):
                self.__profile = name
        self.__profiles = [p for p in self.__profiles if "+" not in p]
        ar = self._arabic_voices()
        if self.__profile not in ar:
            self.__profile = next((v for v in ("Aseel", "Riad", "Jamal") if v in ar), ar[0] if ar else self.__profiles[0])
        if not hasattr(self, "_secondary"):
            self._secondary = "auto"
        self.__rate = 50
        self.__pitch = 50
        self.__volume = 50
        self.__rate_boost = False
        self.__tts_queue = queue.Queue()
        self.__tts_thread = TTSThread(self.__tts_queue)
        self.__tts_thread.start()
        log.info("Using RHVoice version {}".format(self.__lib.RHVoice_get_version().decode()))
        try:   # for diagnosing problems that depend on NVDA's language (number format, etc.)
            import locale as _loc
            log.info("Fasih: NVDA language %s, number format %r, decimal point %r"
                     % (languageHandler.getLanguage(), _loc.setlocale(_loc.LC_NUMERIC), _loc.localeconv().get("decimal_point")))
        except Exception:
            pass
        arabic_pipeline.init_dictionaries(config_path)
        arabic_pipeline.preload()
        self._warm(*[v for v in (self.__profile, self._secondary_profile()) if v])

    # quality settings: rewrite voice files, then reload this synth so the engine reads them
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

    def _get_readEmojis(self):
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

    def terminate(self):
        self.cancel()
        self.__tts_queue.put(None)
        self.__tts_thread.join(1.0)        # never hang NVDA if the worker is stuck; it is a daemon thread
        if self.__tts_thread.is_alive():
            log.error("Fasih: speech thread did not stop; leaving the engine to it")
            return
        self.__player.close()
        self.__lib.RHVoice_delete_tts_engine(self.__tts_engine)
        self.__tts_engine = None

    def speak(self, speechSequence):
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
                if log.isEnabledFor(log.DEBUG):   # diagnostics: NVDA's log level set to "debug"
                    try:
                        _ui = languageHandler.getLanguage()
                    except Exception:
                        _ui = "?"
                    log.debug("Fasih: voice=%s lang=%s nvda-ui=%s rate=%s pitch=%s volume=%s boost=%s text=%r"
                              % (voice, langs.get(voice, "?"), _ui, rate, pitch, volume, boost,
                                 "".join(x for x in items if isinstance(x, str))[:60]))
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

        self.__tts_queue.put(job)

    def pause(self, switch):
        self.__player.pause(switch)

    def cancel(self):
        try:
            while True:
                self.__tts_queue.get_nowait()
        except queue.Empty:
            self.__cancel_flag.set()
            self.__tts_queue.put(self.__cancel_flag.clear)
            self.__player.stop()

    def clamp(self, value, minValue=0, maxValue=100):
        return max(minValue, min(maxValue, value))

    def _get_lastIndex(self):
        return self.__mark_callback.index

    def _arabic_voices(self):
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
        out["auto"] = StringParameterInfo("auto", _("Automatic (a voice in NVDA's language, else English)"))
        for p in self.__profiles:
            if p not in self._arabic_voices():
                out[p] = StringParameterInfo(p, "%s (%s)" % (p, self.__voice_languages[p]))
        return out

    def _get_secondaryVoice(self):
        sv = getattr(self, "_secondary", "auto")
        return sv if sv in self._get_availableSecondaryvoices() else "auto"

    def _set_secondaryVoice(self, v):
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
                    text = "<speak>%s</speak>" % ("\u0644\u0627" if lang.startswith("ar") else "a")
                    task = SpeakText(self.__lib, self.__tts_engine, text, self.__cancel_flag, player)
                    task.final = False
                    task.set_voice_profile(v)
                    task()
                except Exception:
                    log.debugWarning("Fasih: warm-up of %s failed" % v, exc_info=True)
                finally:
                    player.mute = False
        self.__tts_queue.put(job)

    def _get_language(self):
        return self.__voice_languages[self.__profile.split("+")[0]]

    def _get_rate(self):
        return self.__rate

    def _set_rate(self, rate):
        self.__rate = self.clamp(rate)

    def _get_pitch(self):
        return self.__pitch

    def _set_pitch(self, pitch):
        self.__pitch = self.clamp(pitch)

    def _get_volume(self):
        return self.__volume

    def _set_volume(self, volume):
        self.__volume = self.clamp(volume)

    def _get_voice(self):
        return self.__profile

    def _set_voice(self, voice):
        voice = voice.split("+")[0]          # profiles saved by earlier builds, e.g. "Jamal+SLT"
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
                pass

    def _get_rateBoost(self):
        return self.__rate_boost

    def _set_rateBoost(self, flag):
        self.__rate_boost = flag
