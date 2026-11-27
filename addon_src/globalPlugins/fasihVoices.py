# Fasih (فصيح): download more RHVoice voices from NVDA's Tools menu (Tools > Fasih > Download voices).
# Author: Riad Assoum. License: GPL-3.0, like the rest of Fasih.
# The voice catalogue and the way a voice is packaged as an NVDA add-on come from Alexander Linkov's
# (kvark128) RHVoice Voice Manager (GPL): the same catalogue address and the same add-on names and
# versions, so voices installed here and there are the same add-ons and either tool sees them.
# Fasih picks the installed voices up as secondary voices.

import base64
import hashlib
import json
import shutil
import tempfile
import threading
import time
import zipfile
import datetime
from io import BytesIO
from pathlib import Path

import wx
import ui
import requests
from configobj import ConfigObj

import addonHandler
import globalPluginHandler
import globalVars
import gui
import languageHandler
from gui.message import messageBox
from gui.addonGui import promptUserForRestart
from logHandler import log

try:
	addonHandler.initTranslation()
except Exception:
	pass
try:
	_
except NameError:
	def _(text):
		return text

# Fasih's own voices beyond the ones bundled (Riad, Aseel, Elise): one zip each, attached to the
# "voices" release of the Fasih repository, listed in voices.json there
FASIH_URL = "https://github.com/riadassoum/Fasih/releases/download/voices/voices.json"
PKG_VERSION = "1.18"
PKG_URL = "https://rhvoice.kvark.crazedns.ru/packages-{version}.json"
NVDA_ADDON_BUILD_NUMBER = "12"
# (connect, read) timeouts: a stalled server ends the download instead of hanging it
TIMEOUT = (10, 20)

LANG_LABELS = {
	"az": _("Azerbaijani"), "be": _("Belarusian"), "cs": _("Czech"), "en": _("English"), "eo": _("Esperanto"),
	"es": _("Spanish"), "hr": _("Croatian"), "hu": _("Hungarian"), "ka": _("Georgian"), "ky": _("Kyrgyz"),
	"lb": _("Luxembourgish"), "mk": _("Macedonian"), "ne": _("Nepali"), "pl": _("Polish"),
	"pt": _("Brazilian Portuguese"), "ro": _("Romanian"), "ru": _("Russian"), "sk": _("Slovak"), "sq": _("Albanian"),
	"sr": _("Serbian"), "tk": _("Turkmen"), "tn": _("Setswana"), "tr": _("Turkish"), "tt": _("Tatar"),
	"uk": _("Ukrainian"), "uz": _("Uzbek"), "vi": _("Vietnamese"),
}


def _size(n):
	if n is None:
		return _("unknown")
	for unit in (_("bytes"), _("KB"), _("MB"), _("GB")):
		if n < 1024 or unit == _("GB"):
			return ("%d %s" % (n, unit)) if unit == _("bytes") else ("%.1f %s" % (n, unit))
		n /= 1024.0


def _duration(seconds):
	if seconds is None:
		return _("unknown")
	seconds = int(seconds + 0.5)
	m, s = divmod(seconds, 60)
	h, m = divmod(m, 60)
	return "%d:%02d:%02d" % (h, m, s) if h else "%d:%02d" % (m, s)


class VoiceBundle(addonHandler.AddonBundle):
	"""A catalogue voice as an NVDA add-on (same name and version as RHVoice Voice Manager's).
	The files are downloaded first (DownloadDialog); extract() only unpacks them."""

	def __init__(self, lang, voice):
		self._installExceptions = []
		self._lang, self._voice = lang, voice
		self.files = {}   # "data" / "langdata" -> downloaded zip path
		lang_version = str(1000 * int(lang["version"]["major"]) + int(lang["version"]["minor"]))
		voice_version = "%s.%s" % (voice["version"]["major"], voice["version"]["minor"])
		self._conf = ConfigObj(configspec=addonHandler.AddonManifest.configspec, encoding="utf-8", default_encoding="utf-8")
		self._conf["name"] = "RHVoice-voice-%s-%s" % (lang["id"], voice["id"])
		self._conf["summary"] = "RHVoice %s, %s voice" % (voice["name"], lang["name"])
		self._conf["description"] = self._conf["summary"]
		self._conf["author"] = "Contributors to the RHVoice project"
		self._conf["version"] = "%s.%s.%s" % (voice_version, lang_version, NVDA_ADDON_BUILD_NUMBER)
		self._conf["minimumNVDAVersion"] = "2012.2.0"
		self._conf["lastTestedNVDAVersion"] = "2099.4.0"
		buf = BytesIO()
		self._conf.write(outfile=buf)
		buf.seek(0)
		self._manifest = addonHandler.AddonManifest(buf)

	@property
	def assets(self):
		return [("data", self._voice), ("langdata", self._lang)]

	def extract(self, addonPath=None):
		path = Path(self.pendingInstallPath)
		path.mkdir(parents=True, exist_ok=True)
		with open(path.joinpath(addonHandler.MANIFEST_FILENAME), "wb") as f:
			self._conf.write(outfile=f)
		for dirName, _asset in self.assets:
			with zipfile.ZipFile(self.files[dirName], "r") as z:
				z.extractall(path.joinpath(dirName))

	@property
	def manifest(self):
		return self._manifest

	def __repr__(self):
		return "RHVoice %s, %s voice" % (self._voice["name"], self._lang["name"])


class FasihVoiceBundle(addonHandler.AddonBundle):
	"""A Fasih voice pack as an NVDA add-on (RHVoice-voice-fasih-<id>): data/ is the voice for the
	engine, variants/ the files Fasih's quality settings are built from (Fasih writes voice.data
	from them when NVDA starts)."""

	def __init__(self, voice):
		self._installExceptions = []
		self._voice = voice
		self.files = {}
		self._conf = ConfigObj(configspec=addonHandler.AddonManifest.configspec, encoding="utf-8", default_encoding="utf-8")
		self._conf["name"] = "RHVoice-voice-fasih-%s" % voice["id"]
		self._conf["summary"] = "Fasih %s (Arabic voice)" % voice["name"]
		self._conf["description"] = voice.get("description", self._conf["summary"])
		self._conf["author"] = voice.get("author", "Riad Assoum")
		self._conf["version"] = voice["version"]
		self._conf["minimumNVDAVersion"] = "2026.1.0"
		self._conf["lastTestedNVDAVersion"] = "2099.4.0"
		buf = BytesIO()
		self._conf.write(outfile=buf)
		buf.seek(0)
		self._manifest = addonHandler.AddonManifest(buf)

	@property
	def assets(self):
		return [("pack", self._voice)]

	def extract(self, addonPath=None):
		path = Path(self.pendingInstallPath)
		path.mkdir(parents=True, exist_ok=True)
		with open(path.joinpath(addonHandler.MANIFEST_FILENAME), "wb") as f:
			self._conf.write(outfile=f)
		with zipfile.ZipFile(self.files["pack"], "r") as z:
			z.extractall(path)

	@property
	def manifest(self):
		return self._manifest

	def __repr__(self):
		return "Fasih %s" % self._voice["name"]


class _Downloader(threading.Thread):
	"""Downloads every file of a voice in turn, checking each one; the dialog reads its state."""

	def __init__(self, items, folder):
		super().__init__(daemon=True)
		self.items, self.folder = items, folder      # items: (key, url, md5 base64)
		self.cancel = threading.Event()
		self.total = None
		self.done = 0
		self.start_time = None
		self.speed = 0.0
		self.error = None
		self.finished = False
		self.paths = {}
		self._samples = []

	def run(self):
		try:
			self.start_time = time.monotonic()
			sizes = []
			for _key, url, _md5 in self.items:
				if self.cancel.is_set():
					return
				try:
					r = requests.head(url, allow_redirects=True, timeout=TIMEOUT)
					sizes.append(int(r.headers.get("Content-Length", "0")) or None)
				except Exception:
					sizes.append(None)
			self.total = sum(sizes) if all(sizes) else None
			for key, url, md5 in self.items:
				if self.cancel.is_set():
					return
				path = Path(self.folder, "%s.zip" % key)
				digest = hashlib.md5()
				with requests.get(url, stream=True, timeout=TIMEOUT) as resp:
					resp.raise_for_status()
					with open(path, "wb") as f:
						for chunk in resp.iter_content(chunk_size=64 * 1024):
							if self.cancel.is_set():
								return
							if chunk:
								f.write(chunk)
								digest.update(chunk)
								self._progress(len(chunk))
				if md5 and digest.digest() != base64.b64decode(md5):
					raise ValueError(_("The downloaded file is damaged (checksum mismatch)."))
				self.paths[key] = str(path)
		except Exception as e:
			if not self.cancel.is_set():
				log.error("Fasih: voice download failed", exc_info=True)
				self.error = str(e) or e.__class__.__name__
		finally:
			self.finished = True

	def _progress(self, n):
		self.done += n
		now = time.monotonic()
		self._samples.append((now, self.done))
		while len(self._samples) > 2 and now - self._samples[0][0] > 3.0:
			self._samples.pop(0)
		t0, d0 = self._samples[0]
		if now > t0:
			self.speed = (self.done - d0) / (now - t0)


class DownloadDialog(wx.Dialog):
	"""Progress of one voice download: size, speed, time elapsed and remaining, and a Cancel button."""

	def __init__(self, parent, title, items, folder):
		super().__init__(parent, title=title)
		self.worker = _Downloader(items, folder)
		sizer = wx.BoxSizer(wx.VERTICAL)
		self.gauge = wx.Gauge(self, range=1000, size=(400, -1))
		sizer.Add(self.gauge, flag=wx.EXPAND | wx.ALL, border=8)
		label = wx.StaticText(self, label=_("&Progress:"))
		sizer.Add(label, flag=wx.LEFT | wx.RIGHT, border=8)
		self.status = wx.TextCtrl(self, style=wx.TE_MULTILINE | wx.TE_READONLY | wx.TE_DONTWRAP, size=(400, 100))
		sizer.Add(self.status, flag=wx.EXPAND | wx.ALL, border=8)
		self.cancelButton = wx.Button(self, wx.ID_CANCEL, _("&Cancel"))
		sizer.Add(self.cancelButton, flag=wx.ALIGN_CENTER | wx.ALL, border=8)
		self.Bind(wx.EVT_BUTTON, self.onCancel, self.cancelButton)
		self.Bind(wx.EVT_CLOSE, self.onCancel)
		self.SetSizerAndFit(sizer)
		self.timer = wx.Timer(self)
		self.Bind(wx.EVT_TIMER, self.onTimer, self.timer)
		self._lastText = ""
		self.CentreOnScreen()
		self.status.SetFocus()

	def ShowModal(self):
		self.worker.start()
		self.timer.Start(500)
		self.onTimer(None)
		return super().ShowModal()

	def _text(self):
		w = self.worker
		elapsed = time.monotonic() - w.start_time if w.start_time else 0.0
		remaining = None
		if w.total and w.speed > 0:
			remaining = max(w.total - w.done, 0) / w.speed
		lines = [
			_("Downloaded: {done} of {total}").format(done=_size(w.done), total=_size(w.total)),
			_("Speed: {speed}/s").format(speed=_size(int(w.speed))),
			_("Time elapsed: {t}").format(t=_duration(elapsed)),
			_("Time remaining: {t}").format(t=_duration(remaining)),
		]
		if w.total:
			lines.insert(0, _("{pct}% complete").format(pct=int(100 * w.done / w.total)))
		return "\n".join(lines)

	def onTimer(self, evt):
		w = self.worker
		if w.total:
			self.gauge.SetValue(min(1000, int(1000 * w.done / w.total)))
		else:
			self.gauge.Pulse()
		text = self._text()
		if text != self._lastText:
			pos = self.status.GetInsertionPoint()
			self.status.ChangeValue(text)
			self.status.SetInsertionPoint(min(pos, len(text)))
			self._lastText = text
		if w.finished:
			self.timer.Stop()
			self.EndModal(wx.ID_OK if not w.error and not w.cancel.is_set() else wx.ID_ABORT if w.error else wx.ID_CANCEL)

	def onCancel(self, evt):
		# the dialog closes at once; the worker stops at its next check (or when the network gives
		# up, at most TIMEOUT later) and its files are removed then (see VoicesDialog.onInstall)
		self.worker.cancel.set()
		self.timer.Stop()
		if self.IsModal():
			self.EndModal(wx.ID_CANCEL)


def _installed_version(bundle):
	try:
		return addonHandler.Addon(bundle.installPath).version
	except Exception:
		return None


def _newer(available, installed):
	"""True when the catalogue offers a newer version than the one installed."""
	def key(v):
		return tuple(int(x) if x.isdigit() else 0 for x in str(v).replace("-", ".").split("."))
	try:
		return bool(installed) and key(available) > key(installed)
	except Exception:
		return False


def _sample(bundle):
	"""A short recording of a Fasih voice, shipped with the add-on (samples/<Name>.wav)."""
	if not isinstance(bundle, FasihVoiceBundle):
		return None
	import os
	path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "samples", bundle._voice.get("name", "") + ".wav")
	if os.path.isfile(path):
		return path
	# voices added after the add-on was built keep their recording online, next to their pack
	url = bundle._voice.get("sampleUrl")
	return url if url and url.startswith("https://") else None


def _play_sample(path):
	"""Plays a bundled sample, or fetches an online one (cached in the temp folder) and plays it."""
	import nvwave, os, tempfile, threading
	if not path.startswith("https://"):
		nvwave.playWaveFile(path, asynchronous=True)
		return
	cache = os.path.join(tempfile.gettempdir(), "fasih-sample-" + os.path.basename(path.split("?")[0]))

	def run():
		try:
			if not os.path.isfile(cache):
				import urllib.request
				req = urllib.request.Request(path, headers={"User-Agent": "Fasih NVDA add-on"})
				with urllib.request.urlopen(req, timeout=20) as r:
					data = r.read()
				if data[:4] != b"RIFF":
					raise ValueError("not a WAV file")
				with open(cache + ".tmp", "wb") as f:
					f.write(data)
				os.replace(cache + ".tmp", cache)
			nvwave.playWaveFile(cache, asynchronous=True)
		except Exception:
			log.error("Fasih: could not fetch the online sample", exc_info=True)
			wx.CallAfter(ui.message, _("The sample could not be downloaded. Check your internet connection."))
	threading.Thread(target=run, daemon=True).start()


class VoicesDialog(wx.Dialog):
	"""Languages and voices of the RHVoice catalogue, with Install and Remove."""

	def __init__(self, parent, pkgData):
		super().__init__(parent, title=_("Fasih: download RHVoice voices"))
		self.pkgData = pkgData
		self.restart = False
		langs = pkgData.get("languages", [])
		ui = (languageHandler.getLanguage() or "").split("_")[0]
		if langs and langs[0].get("fasih") and ui not in [l.get("lang2code") for l in langs[1:]]:
			ui = "ar"                      # NVDA in a language without voices: Fasih's own first
		defIndex = next((i for i, l in enumerate(langs) if l.get("lang2code") == ui), 0)
		if not any(l.get("lang2code") == ui for l in langs):
			defIndex = next((i for i, l in enumerate(langs) if l.get("lang2code") == "en"), 0)
		sizer = wx.BoxSizer(wx.VERTICAL)
		sizer.Add(wx.StaticText(self, label=_("&Language:")), flag=wx.LEFT | wx.TOP, border=8)
		self.langChoice = wx.Choice(self, choices=[(_("Arabic: Fasih voices") if l.get("fasih") else LANG_LABELS.get(l.get("lang2code"), l.get("name", "?"))) for l in langs])
		self.langChoice.SetSelection(defIndex)
		self.langChoice.Bind(wx.EVT_CHOICE, lambda e: self.refresh())
		sizer.Add(self.langChoice, flag=wx.EXPAND | wx.ALL, border=8)
		sizer.Add(wx.StaticText(self, label=_("&Voices:")), flag=wx.LEFT, border=8)
		self.voiceList = wx.ListCtrl(self, style=wx.LC_REPORT | wx.LC_SINGLE_SEL, size=(480, 220))
		for label in (_("Name"), _("Status"), _("Version")):
			self.voiceList.AppendColumn(label, width=150)
		self.voiceList.Bind(wx.EVT_LIST_ITEM_SELECTED, lambda e: self.updateButtons())
		sizer.Add(self.voiceList, flag=wx.EXPAND | wx.ALL, border=8)
		buttons = wx.BoxSizer(wx.HORIZONTAL)
		self.installButton = wx.Button(self, label=_("&Install"))
		self.removeButton = wx.Button(self, label=_("&Remove"))
		self.listenButton = wx.Button(self, label=_("&Listen"))
		closeButton = wx.Button(self, wx.ID_CLOSE, _("Cl&ose"))
		for b in (self.installButton, self.removeButton, self.listenButton, closeButton):
			buttons.Add(b, flag=wx.ALL, border=4)
		sizer.Add(buttons, flag=wx.ALIGN_CENTER | wx.ALL, border=4)
		self.installButton.Bind(wx.EVT_BUTTON, self.onInstall)
		self.removeButton.Bind(wx.EVT_BUTTON, self.onRemove)
		self.listenButton.Bind(wx.EVT_BUTTON, self.onListen)
		closeButton.Bind(wx.EVT_BUTTON, lambda e: self.Close())
		self.Bind(wx.EVT_CLOSE, self.onClose)
		self.SetEscapeId(wx.ID_CLOSE)
		self.SetSizerAndFit(sizer)
		self.refresh()
		self.CentreOnScreen()
		self.voiceList.SetFocus()

	def lang(self):
		return self.pkgData["languages"][self.langChoice.GetSelection()]

	def refresh(self, select=0):
		lang = self.lang()
		self.voiceList.DeleteAllItems()
		self.bundles = []
		for voice in lang.get("voices", []):
			b = FasihVoiceBundle(voice) if lang.get("fasih") else VoiceBundle(lang, voice)
			self.bundles.append(b)
			if b.isInstalled:
				status = _("Will be removed after restart") if b.isPendingRemove else _("Installed")
				if not b.isPendingRemove and not b.isPendingInstall and _newer(b.version, _installed_version(b)):
					status = _("Update available")
			else:
				status = _("Will be installed after restart") if b.isPendingInstall else _("Not installed")
			label = voice["name"]
			if lang.get("fasih"):
				label = "%s (%s)" % (voice.get("name_ar", voice["name"]) if (languageHandler.getLanguage() or "").startswith("ar") else voice["name"],
					_("female") if voice.get("gender") == "female" else _("male"))
			self.voiceList.Append((label, status, b.version))
		if self.bundles:
			self.voiceList.Select(select)
			self.voiceList.Focus(select)
		self.updateButtons()

	def selected(self):
		i = self.voiceList.GetFirstSelected()
		return (i, self.bundles[i]) if 0 <= i < len(self.bundles) else (None, None)

	def updateButtons(self):
		_i, b = self.selected()
		pending = b is None or b.requiresRestart
		update = bool(b) and b.isInstalled and not pending and _newer(b.version, _installed_version(b))
		self.installButton.SetLabel(_("&Update") if update else _("&Install"))
		self.installButton.Enable(bool(b) and not pending and (not b.isInstalled or update))
		self.removeButton.Enable(bool(b) and not pending and b.isInstalled)
		self.listenButton.Enable(bool(b) and _sample(b) is not None)

	def onListen(self, evt):
		_i, b = self.selected()
		path = _sample(b) if b else None
		if path:
			try:
				_play_sample(path)
			except Exception:
				log.error("Fasih: could not play the sample", exc_info=True)

	def onInstall(self, evt):
		i, bundle = self.selected()
		if bundle is None:
			return
		base = Path(globalVars.appArgs.configPath, "Fasih-config", "downloads")
		base.mkdir(parents=True, exist_ok=True)
		folder = Path(tempfile.mkdtemp(dir=str(base)))     # one folder per download
		items = [(key, asset["dataUrl"], asset.get("dataMd5")) for key, asset in bundle.assets]
		dlg = DownloadDialog(self, _("Downloading {voice}").format(voice=bundle._voice["name"]), items, folder)
		result = dlg.ShowModal()
		worker = dlg.worker
		error = worker.error
		bundle.files = dict(worker.paths)
		dlg.Destroy()
		if result == wx.ID_CANCEL:
			def cleanup():
				worker.join(60)
				shutil.rmtree(folder, ignore_errors=True)
			threading.Thread(target=cleanup, daemon=True).start()
			self.voiceList.SetFocus()
			return
		if result != wx.ID_OK:
			shutil.rmtree(folder, ignore_errors=True)
			messageBox(_("The voice could not be downloaded: {error}\nCheck your internet connection and try again.").format(error=error),
				_("Error"), style=wx.OK | wx.ICON_ERROR, parent=self)
			return
		try:
			if bundle.isInstalled:
				# an update: NVDA swaps the old version for the new one at restart
				addonHandler.Addon(bundle.installPath).requestRemove()
			addon = addonHandler.installAddonBundle(bundle)
		except Exception:
			log.error("Fasih: voice installation failed", exc_info=True)
			addon = None
		shutil.rmtree(folder, ignore_errors=True)
		if addon is None:
			shutil.rmtree(bundle.pendingInstallPath, ignore_errors=True)
			messageBox(_("The voice could not be installed."), _("Error"), style=wx.OK | wx.ICON_ERROR, parent=self)
			return
		self.restart = True
		msg = (_("{voice} is installed. It will be available after NVDA restarts, among Fasih's voices.") if isinstance(bundle, FasihVoiceBundle)
			else _("{voice} is installed. It will be available after NVDA restarts, as a secondary voice in Fasih."))
		messageBox(msg.format(voice=bundle._voice["name"]),
			_("Voice installed"), style=wx.OK | wx.ICON_INFORMATION, parent=self)
		self.refresh(i)
		self.voiceList.SetFocus()

	def onRemove(self, evt):
		i, bundle = self.selected()
		if bundle is None:
			return
		if messageBox(_("Remove {voice}? It will be removed when NVDA restarts.").format(voice=bundle._voice["name"]),
				_("Remove voice"), style=wx.YES_NO | wx.ICON_QUESTION, parent=self) != wx.YES:
			return
		addonHandler.Addon(bundle.installPath).requestRemove()
		self.restart = True
		self.refresh(i)
		self.voiceList.SetFocus()

	def onClose(self, evt):
		restart = self.restart
		self.Destroy()
		if restart:
			promptUserForRestart()


def _catalogue():
	"""The voice catalogue, cached for the time it says (shared with RHVoice Voice Manager's cache)."""
	folder = Path(globalVars.appArgs.configPath, "RHVoice-config", "packages")
	folder.mkdir(parents=True, exist_ok=True)
	path = folder.joinpath("packages-%s.json" % PKG_VERSION)
	try:
		data = json.loads(path.read_text(encoding="utf-8"))
		expires = datetime.datetime.fromtimestamp(path.stat().st_mtime, datetime.timezone.utc) + datetime.timedelta(seconds=data.get("ttl", 3600))
		if expires > datetime.datetime.now(datetime.timezone.utc):
			return data
	except (OSError, ValueError):
		pass
	resp = requests.get(PKG_URL.format(version=PKG_VERSION), timeout=15)
	resp.raise_for_status()
	data = resp.json()
	tmp = Path(str(path) + ".tmp")
	tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
	tmp.replace(path)
	return data


# --- Pronunciation dictionary (Tools > Fasih > Pronunciation dictionary) ---
# Entries are kept in Fasih-config/user-dic.json, which the driver reads; a word
# matches whatever diacritics it is written with, and only as a whole word.

def _dic_path():
	import os
	d = os.path.join(globalVars.appArgs.configPath, "Fasih-config")
	os.makedirs(d, exist_ok=True)
	return d, os.path.join(d, "user-dic.json")


def _reload_dictionary(folder):
	import sys
	for name, mod in list(sys.modules.items()):
		if name.endswith("fasih.pbt.dictionary_manager") and mod is not None:
			try:
				mod.load_user_dictionary(folder)
			except Exception:
				log.error("Fasih: dictionary reload failed", exc_info=True)


def _speak(text):
	import speech
	speech.cancelSpeech()
	speech.speakText(text)


class EntryDialog(wx.Dialog):

	def __init__(self, parent, title, original="", replacement=""):
		super().__init__(parent, title=title)
		sizer = wx.BoxSizer(wx.VERTICAL)
		sizer.Add(wx.StaticText(self, label=_("&Word or phrase as written:")), flag=wx.ALL, border=5)
		self.original = wx.TextCtrl(self, value=original)
		sizer.Add(self.original, flag=wx.EXPAND | wx.LEFT | wx.RIGHT, border=5)
		sizer.Add(wx.StaticText(self, label=_("&Say it as (add diacritics where needed):")), flag=wx.ALL, border=5)
		self.replacement = wx.TextCtrl(self, value=replacement)
		sizer.Add(self.replacement, flag=wx.EXPAND | wx.LEFT | wx.RIGHT, border=5)
		listen = wx.Button(self, label=_("&Listen"))
		listen.Bind(wx.EVT_BUTTON, lambda e: _speak(self.replacement.GetValue() or self.original.GetValue()))
		sizer.Add(listen, flag=wx.ALL, border=5)
		sizer.Add(self.CreateButtonSizer(wx.OK | wx.CANCEL), flag=wx.ALL | wx.ALIGN_RIGHT, border=5)
		self.SetSizerAndFit(sizer)
		self.SetSize((420, -1))
		self.original.SetFocus()


class DictionaryDialog(wx.Dialog):

	def __init__(self, parent):
		super().__init__(parent, title=_("Fasih pronunciation dictionary"))
		self.folder, self.path = _dic_path()
		try:
			with open(self.path, "r", encoding="utf-8") as f:
				self.entries = [e for e in json.load(f) if e.get("original") and e.get("replacement")]
		except Exception:
			self.entries = []
		sizer = wx.BoxSizer(wx.VERTICAL)
		sizer.Add(wx.StaticText(self, label=_("&Entries:")), flag=wx.ALL, border=5)
		self.list = wx.ListCtrl(self, style=wx.LC_REPORT | wx.LC_SINGLE_SEL, size=(460, 260))
		self.list.AppendColumn(_("Written"), width=220)
		self.list.AppendColumn(_("Said as"), width=220)
		sizer.Add(self.list, proportion=1, flag=wx.EXPAND | wx.LEFT | wx.RIGHT, border=5)
		row = wx.BoxSizer(wx.HORIZONTAL)
		for label, handler in ((_("&Add..."), self.onAdd), (_("&Edit..."), self.onEdit),
		                       (_("&Remove"), self.onRemove), (_("&Listen"), self.onListen)):
			b = wx.Button(self, label=label)
			b.Bind(wx.EVT_BUTTON, handler)
			row.Add(b, flag=wx.ALL, border=5)
		sizer.Add(row)
		sizer.Add(self.CreateButtonSizer(wx.OK | wx.CANCEL), flag=wx.ALL | wx.ALIGN_RIGHT, border=5)
		self.Bind(wx.EVT_BUTTON, self.onOk, id=wx.ID_OK)
		self.list.Bind(wx.EVT_LIST_ITEM_ACTIVATED, self.onEdit)
		self.SetSizerAndFit(sizer)
		self.refresh()
		self.list.SetFocus()

	def refresh(self, select=0):
		self.list.DeleteAllItems()
		for e in self.entries:
			self.list.Append((e["original"], e["replacement"]))
		if self.entries:
			select = max(0, min(select, len(self.entries) - 1))
			self.list.Select(select)
			self.list.Focus(select)

	def _ask(self, title, e=None):
		d = EntryDialog(self, title, *(((e["original"], e["replacement"])) if e else ()))
		try:
			if d.ShowModal() != wx.ID_OK:
				return None
			o, r = d.original.GetValue().strip(), d.replacement.GetValue().strip()
			return {"original": o, "replacement": r} if o and r else None
		finally:
			d.Destroy()

	def onAdd(self, evt):
		e = self._ask(_("Add entry"))
		if e:
			self.entries = [x for x in self.entries if x["original"] != e["original"]] + [e]
			self.refresh(len(self.entries) - 1)

	def onEdit(self, evt):
		i = self.list.GetFirstSelected()
		if i < 0:
			return
		e = self._ask(_("Edit entry"), self.entries[i])
		if e:
			self.entries[i] = e
			self.refresh(i)

	def onRemove(self, evt):
		i = self.list.GetFirstSelected()
		if i >= 0:
			del self.entries[i]
			self.refresh(i)

	def onListen(self, evt):
		i = self.list.GetFirstSelected()
		if i >= 0:
			_speak(self.entries[i]["replacement"])

	def onOk(self, evt):
		try:
			tmp = self.path + ".tmp"
			with open(tmp, "w", encoding="utf-8") as f:
				json.dump(self.entries, f, ensure_ascii=False, indent=1)
			import os
			os.replace(tmp, self.path)
		except Exception:
			log.error("Fasih: could not save the dictionary", exc_info=True)
			messageBox(_("The dictionary could not be saved."), _("Error"), style=wx.OK | wx.ICON_ERROR)
			return
		_reload_dictionary(self.folder)
		self.EndModal(wx.ID_OK)



# --- settings shared by the text window and the update checker (NVDA config, survives updates) ---
def _settings_path():
	import os
	return os.path.join(_dic_path()[0], "settings.json")


def _setting(key, default=None):
	try:
		with open(_settings_path(), encoding="utf-8") as f:
			return json.load(f).get(key, default)
	except Exception:
		return default


def _set_setting(key, value):
	try:
		with open(_settings_path(), encoding="utf-8") as f:
			d = json.load(f)
	except Exception:
		d = {}
	d[key] = value
	try:
		with open(_settings_path(), "w", encoding="utf-8") as f:
			json.dump(d, f, ensure_ascii=False, indent=1)
	except Exception:
		log.error("Fasih: settings not saved", exc_info=True)


# --- Tools > Fasih > Diacritize text -------------------------------------------------------------
# The diacritizers' display names (keep them in step with the Diacritizer setting in the driver).
_DIACRITIZERS = [("rawi", _("Rawi (most accurate)")), ("shams", _("Barq (fastest)"))]
_MARKS_RE = None


def _tashkeel_module():
	"""Fasih's diacritizer module, shared with the speech engine when Fasih is speaking."""
	import importlib
	return importlib.import_module("synthDrivers.fasih.arabic.ar_tashkeel")


def _split_for_progress(text, limit=1500):
	"""Paragraphs, and sentences within long paragraphs: pieces that keep whole sentences
	together (the diacritizer needs the sentence around a word) but are small enough to show
	progress and to stop quickly. Joining the pieces gives back the text exactly."""
	import re
	pieces = []
	for para in re.split(r"(?<=\n)", text):
		if len(para) <= limit:
			pieces.append(para)
			continue
		cur = ""
		for sent in re.split(r"(?<=[.!?؟؛:۔])(?=\s)", para):
			if cur and len(cur) + len(sent) > limit:
				pieces.append(cur)
				cur = ""
			cur += sent
		if cur:
			pieces.append(cur)
	return [x for x in pieces if x]


def _read_text_file(path):
	"""Text from a .txt (UTF-8, UTF-16 or Windows Arabic) or .docx file."""
	import re
	if path.lower().endswith(".docx"):
		with zipfile.ZipFile(path) as z:
			xml = z.read("word/document.xml").decode("utf-8", "replace")
		xml = re.sub(r"</w:p>", "\n", xml)
		xml = re.sub(r"<w:tab/>", "\t", xml)
		xml = re.sub(r"<[^>]+>", "", xml)
		import html
		return html.unescape(xml)
	raw = open(path, "rb").read()
	for enc in ("utf-8-sig", "utf-16", "cp1256"):
		try:
			t = raw.decode(enc)
			if enc != "utf-16" or raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
				return t
		except Exception:
			continue
	return raw.decode("utf-8", "replace")


class DiacritizeDialog(wx.Dialog):
	"""Diacritize Arabic text of any length with the diacritizer chosen here. Works whichever
	synthesizer NVDA is using."""

	def __init__(self, parent):
		super().__init__(parent, title=_("Diacritize Arabic text"), style=wx.DEFAULT_DIALOG_STYLE | wx.RESIZE_BORDER)
		self._cancel = threading.Event()
		self._busy = False
		main = wx.BoxSizer(wx.VERTICAL)
		main.Add(wx.StaticText(self, label=_("&Text to diacritize:")), 0, wx.LEFT | wx.TOP, 8)
		self.input = wx.TextCtrl(self, style=wx.TE_MULTILINE | wx.TE_RICH2, size=(640, 180))
		main.Add(self.input, 1, wx.EXPAND | wx.ALL, 8)
		row = wx.BoxSizer(wx.HORIZONTAL)
		b = wx.Button(self, label=_("&Paste"))
		b.Bind(wx.EVT_BUTTON, self.onPaste)
		row.Add(b, 0, wx.RIGHT, 6)
		b = wx.Button(self, label=_("&Open file..."))
		b.Bind(wx.EVT_BUTTON, self.onOpen)
		row.Add(b, 0, wx.RIGHT, 6)
		main.Add(row, 0, wx.LEFT | wx.RIGHT, 8)
		row = wx.BoxSizer(wx.HORIZONTAL)
		row.Add(wx.StaticText(self, label=_("Dia&critizer:")), 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 6)
		self.choices = self._available()
		self.lib = wx.Choice(self, choices=[n for _k, n in self.choices])
		keys = [k for k, _n in self.choices]
		want = _setting("windowDiacritizer", "rawi")
		self.lib.SetSelection(keys.index(want) if want in keys else 0)
		row.Add(self.lib, 1)
		main.Add(row, 0, wx.EXPAND | wx.ALL, 8)
		self.keep = wx.CheckBox(self, label=_("&Keep the harakat already in the text"))
		self.keep.SetValue(bool(_setting("windowKeepMarks", True)))
		main.Add(self.keep, 0, wx.LEFT | wx.RIGHT, 8)
		row = wx.BoxSizer(wx.HORIZONTAL)
		self.go = wx.Button(self, label=_("&Diacritize"))
		self.go.Bind(wx.EVT_BUTTON, self.onGo)
		row.Add(self.go, 0, wx.RIGHT, 6)
		self.stop = wx.Button(self, label=_("&Stop"))
		self.stop.Bind(wx.EVT_BUTTON, lambda e: self._cancel.set())
		self.stop.Disable()
		row.Add(self.stop, 0)
		main.Add(row, 0, wx.ALL, 8)
		self.gauge = wx.Gauge(self, range=100)
		main.Add(self.gauge, 0, wx.EXPAND | wx.LEFT | wx.RIGHT, 8)
		self.status = wx.StaticText(self, label="")
		main.Add(self.status, 0, wx.ALL, 8)
		main.Add(wx.StaticText(self, label=_("&Result:")), 0, wx.LEFT, 8)
		self.output = wx.TextCtrl(self, style=wx.TE_MULTILINE | wx.TE_READONLY | wx.TE_RICH2, size=(640, 180))
		main.Add(self.output, 1, wx.EXPAND | wx.ALL, 8)
		row = wx.BoxSizer(wx.HORIZONTAL)
		b = wx.Button(self, label=_("C&opy result"))
		b.Bind(wx.EVT_BUTTON, self.onCopy)
		row.Add(b, 0, wx.RIGHT, 6)
		b = wx.Button(self, label=_("Sa&ve result..."))
		b.Bind(wx.EVT_BUTTON, self.onSave)
		row.Add(b, 0, wx.RIGHT, 6)
		b = wx.Button(self, wx.ID_CLOSE, label=_("Close"))
		b.Bind(wx.EVT_BUTTON, lambda e: self.Close())
		row.Add(b, 0)
		main.Add(row, 0, wx.ALL, 8)
		self.SetEscapeId(wx.ID_CLOSE)
		self.Bind(wx.EVT_CLOSE, self.onClose)
		self.SetSizerAndFit(main)
		self.input.SetFocus()

	def _available(self):
		try:
			have = _tashkeel_module().available()
		except Exception:
			have = ["rawi"]
		out = [(k, n) for k, n in _DIACRITIZERS if k in have]
		return out or _DIACRITIZERS[:1]

	def onPaste(self, evt):
		data = wx.TextDataObject()
		if wx.TheClipboard.Open():
			try:
				if wx.TheClipboard.GetData(data):
					self.input.SetValue(data.GetText())
			finally:
				wx.TheClipboard.Close()
		self.input.SetFocus()

	def onOpen(self, evt):
		with wx.FileDialog(self, _("Open a text file"), wildcard=_("Text and Word files") + " (*.txt;*.docx)|*.txt;*.docx", style=wx.FD_OPEN | wx.FD_FILE_MUST_EXIST) as fd:
			if fd.ShowModal() != wx.ID_OK:
				return
			try:
				self.input.SetValue(_read_text_file(fd.GetPath()))
			except Exception:
				log.error("Fasih: could not read file", exc_info=True)
				messageBox(_("The file could not be read."), _("Error"), style=wx.OK | wx.ICON_ERROR, parent=self)
		self.input.SetFocus()

	def onGo(self, evt):
		import re
		text = self.input.GetValue()
		if not text.strip() or self._busy:
			return
		key = self.choices[self.lib.GetSelection()][0]
		_set_setting("windowDiacritizer", key)
		_set_setting("windowKeepMarks", self.keep.GetValue())
		if not self.keep.GetValue():
			text = re.sub("[ً-ْٰ]", "", text)
		pieces = _split_for_progress(text)
		self._busy = True
		self._cancel.clear()
		self.go.Disable()
		self.stop.Enable()
		self.output.SetValue("")
		self.gauge.SetValue(0)
		self.status.SetLabel(_("Diacritizing..."))
		total = sum(len(x) for x in pieces) or 1

		def work():
			out, done, last = [], 0, 0.0
			t0 = time.time()
			try:
				T = _tashkeel_module()
				for piece in pieces:
					if self._cancel.is_set():
						break
					if re.search("[ء-ي]", piece):
						try:
							d = T.diacritize_strict(piece, backend=key, is_cancelled=self._cancel.is_set)
						except TypeError:
							d = T.diacritize_strict(piece, backend=key)
						out.append(d if d and d.strip() else piece)
					else:
						out.append(piece)
					done += len(piece)
					if time.time() - last > 0.3:
						last = time.time()
						wx.CallAfter(self._progress, done * 100 // total)
				err = None
			except Exception as e:
				log.error("Fasih: diacritization failed", exc_info=True)
				err = str(e)
			wx.CallAfter(self._finished, "".join(out), self._cancel.is_set(), err, time.time() - t0, len(pieces))

		threading.Thread(target=work, daemon=True).start()

	def _progress(self, pct):
		if not self:
			return
		self.gauge.SetValue(min(pct, 100))
		self.status.SetLabel(_("{pct}% done").format(pct=pct))

	def _finished(self, result, cancelled, err, secs, n):
		if not self:
			return
		self._busy = False
		self.go.Enable()
		self.stop.Disable()
		self.output.SetValue(result)
		if err:
			msg = _("Diacritization failed: {err}").format(err=err)
		elif cancelled:
			msg = _("Stopped. The part done so far is in the result.")
		else:
			self.gauge.SetValue(100)
			msg = _("Done in {secs:.1f} seconds.").format(secs=secs)
		self.status.SetLabel(msg)
		import ui
		ui.message(msg)
		self.output.SetFocus()
		self.output.SetInsertionPoint(0)

	def onCopy(self, evt):
		text = self.output.GetValue()
		if text and wx.TheClipboard.Open():
			try:
				wx.TheClipboard.SetData(wx.TextDataObject(text))
			finally:
				wx.TheClipboard.Close()
			import ui
			ui.message(_("Copied"))

	def onSave(self, evt):
		text = self.output.GetValue()
		if not text:
			return
		with wx.FileDialog(self, _("Save the result"), wildcard=_("Text files") + " (*.txt)|*.txt", style=wx.FD_SAVE | wx.FD_OVERWRITE_PROMPT) as fd:
			if fd.ShowModal() != wx.ID_OK:
				return
			path = fd.GetPath()
			if not path.lower().endswith(".txt"):
				path += ".txt"
			try:
				with open(path, "w", encoding="utf-8-sig") as f:
					f.write(text)
			except Exception:
				log.error("Fasih: could not save", exc_info=True)
				messageBox(_("The file could not be saved."), _("Error"), style=wx.OK | wx.ICON_ERROR, parent=self)

	def onClose(self, evt):
		self._cancel.set()
		self.Destroy()


# --- Tools > Fasih > Check for updates -----------------------------------------------------------
RELEASES_API = "https://api.github.com/repos/riadassoum/Fasih/releases?per_page=30"
RELEASES_PAGE = "https://github.com/riadassoum/Fasih/releases"


def _vkey(v):
	"""Version sort key: 1.3 > 1.3-rc1 > 1.3-beta9 > 1.3-beta2 > 1.2.1; a "v" prefix is ignored."""
	import re
	m = re.match(r"\s*v?(\d+(?:\.\d+)*)(?:[-. _]?(alpha|beta|rc|a|b)[-. ]?(\d*))?", str(v).lower())
	if not m:
		return None
	nums = [int(x) for x in m.group(1).split(".")] + [0, 0]
	stage = {"alpha": 0, "a": 0, "beta": 1, "b": 1, "rc": 2}.get(m.group(2), 3)
	return tuple(nums[:3]) + (stage, int(m.group(3) or 0))


def _is_prerelease_version(v):
	k = _vkey(v)
	return bool(k) and k[3] < 3


def _current_version():
	try:
		return addonHandler.getCodeAddon().version
	except Exception:
		return "0"


def _latest_release(include_betas):
	"""The newest Fasih release (or None): version, changelog, add-on asset (url, size, sha256)."""
	r = requests.get(RELEASES_API, timeout=TIMEOUT, headers={"Accept": "application/vnd.github+json"})
	r.raise_for_status()
	best = None
	for rel in r.json():
		if rel.get("draft"):
			continue
		ver = (rel.get("tag_name") or "").strip()
		key = _vkey(ver)
		if key is None:          # the "voices" release and anything else that is not a version
			continue
		if (rel.get("prerelease") or key[3] < 3) and not include_betas:
			continue
		asset = next((a for a in rel.get("assets", []) if a.get("name", "").endswith(".nvda-addon")), None)
		if not asset:
			continue
		if best is None or key > best["key"]:
			digest = asset.get("digest") or ""
			best = {"key": key, "version": ver.lstrip("vV"), "notes": rel.get("body") or "",
			        "url": asset.get("browser_download_url"), "size": asset.get("size") or 0,
			        "sha256": digest.split(":", 1)[1].lower() if digest.startswith("sha256:") else "",
			        "page": rel.get("html_url") or RELEASES_PAGE, "name": rel.get("name") or ver}
	return best


def _pack_updates():
	"""Installed Fasih voice packs that have a newer version in voices.json: [(name, old, new)]."""
	import os
	out = []
	try:
		fv = requests.get(FASIH_URL, timeout=TIMEOUT).json()
	except Exception:
		return out
	base = os.path.join(globalVars.appArgs.configPath, "addons")
	for v in fv.get("voices", []):
		path = os.path.join(base, "RHVoice-voice-fasih-%s" % v.get("id", v.get("name")))
		if not os.path.isdir(path):
			continue
		try:
			old = addonHandler.Addon(path).version
		except Exception:
			continue
		if _newer(v.get("version"), old):
			out.append((v.get("name"), old, v.get("version")))
	return out


class UpdateDialog(wx.Dialog):
	"""A new Fasih version: what changed, and what to do about it."""

	def __init__(self, parent, rel, current, packs):
		super().__init__(parent, title=_("Fasih update available"), style=wx.DEFAULT_DIALOG_STYLE | wx.RESIZE_BORDER)
		self.choice = None
		main = wx.BoxSizer(wx.VERTICAL)
		kind = _("beta version") if _is_prerelease_version(rel["version"]) else _("version")
		head = _("A new Fasih {kind} is available: {new}. You have {cur}.").format(kind=kind, new=rel["version"], cur=current)
		if packs:
			head += " " + _("Updates are also available for these voices: {names}.").format(names=", ".join(n for n, _o, _v in packs))
		main.Add(wx.StaticText(self, label=head), 0, wx.ALL, 8)
		main.Add(wx.StaticText(self, label=_("&What's new:")), 0, wx.LEFT, 8)
		notes = wx.TextCtrl(self, value=rel["notes"] or _("(no notes for this release)"), style=wx.TE_MULTILINE | wx.TE_READONLY | wx.TE_RICH2, size=(600, 260))
		main.Add(notes, 1, wx.EXPAND | wx.ALL, 8)
		row = wx.BoxSizer(wx.HORIZONTAL)
		for label, code in ((_("&Download and install"), "install"), (_("&Skip this version"), "skip"), (_("Remind me &later"), "later")):
			b = wx.Button(self, label=label)
			b.Bind(wx.EVT_BUTTON, lambda e, c=code: self._done(c))
			row.Add(b, 0, wx.RIGHT, 6)
		main.Add(row, 0, wx.ALL, 8)
		self.Bind(wx.EVT_CHAR_HOOK, lambda e: self._done("later") if e.GetKeyCode() == wx.WXK_ESCAPE else e.Skip())
		self.SetSizerAndFit(main)
		notes.SetFocus()

	def _done(self, code):
		self.choice = code
		self.EndModal(wx.ID_OK)


class _UpdateDownloader(threading.Thread):
	"""Download the add-on, check its size and SHA-256 against GitHub's, then install it."""

	def __init__(self, rel):
		super().__init__(daemon=True)
		self.rel = rel
		self.cancelled = False
		self.dlg = wx.ProgressDialog(_("Updating Fasih"), _("Downloading Fasih {v}...").format(v=rel["version"]), maximum=100, parent=gui.mainFrame, style=wx.PD_APP_MODAL | wx.PD_AUTO_HIDE | wx.PD_CAN_ABORT | wx.PD_ELAPSED_TIME | wx.PD_REMAINING_TIME)

	def _tick(self, pct, got, total):
		try:
			cont, _skip = self.dlg.Update(min(pct, 99), _("Downloaded {got} of {total}").format(got=_size(got), total=_size(total)))
			if not cont:
				self.cancelled = True
		except Exception:
			pass

	def run(self):
		import os
		path = os.path.join(tempfile.gettempdir(), "Fasih-%s.nvda-addon" % self.rel["version"])
		err = None
		try:
			h = hashlib.sha256()
			with requests.get(self.rel["url"], stream=True, timeout=TIMEOUT) as r:
				r.raise_for_status()
				total = int(r.headers.get("Content-Length") or self.rel["size"] or 0)
				got, last = 0, 0
				with open(path, "wb") as f:
					for chunk in r.iter_content(1 << 16):
						if self.cancelled:
							break
						f.write(chunk)
						h.update(chunk)
						got += len(chunk)
						if total and time.time() - last > 0.25:
							last = time.time()
							wx.CallAfter(self._tick, got * 100 // total, got, total)
			if self.cancelled:
				err = "cancelled"
			elif self.rel["size"] and got != self.rel["size"]:
				err = _("the download is incomplete")
			elif self.rel["sha256"] and h.hexdigest() != self.rel["sha256"]:
				err = _("the downloaded file does not match the release (checksum mismatch)")
		except Exception as e:
			log.error("Fasih: update download failed", exc_info=True)
			err = str(e)
		wx.CallAfter(self._finish, path, err)

	def _finish(self, path, err):
		import os
		try:
			self.dlg.Destroy()
		except Exception:
			pass
		if err:
			try:
				os.remove(path)
			except Exception:
				pass
			if err != "cancelled":
				if messageBox(_("Fasih could not be updated: {err}. Open the download page instead?").format(err=err), _("Fasih update"), style=wx.YES_NO | wx.ICON_ERROR) == wx.YES:
					import webbrowser
					webbrowser.open(self.rel["page"])
			return
		_install_addon_file(path)


def _install_addon_file(path):
	"""Hand the add-on to NVDA's installer (with its usual restart prompt); fall back to a
	direct install with our own prompt, then to opening the file."""
	import os
	try:
		from gui import addonGui
		if hasattr(addonGui, "installAddon"):
			addonGui.installAddon(gui.mainFrame, path)
			return
	except Exception:
		log.debugWarning("Fasih: addonGui.installAddon failed", exc_info=True)
	try:
		addonHandler.installAddonBundle(addonHandler.AddonBundle(path))
		promptUserForRestart()
		return
	except Exception:
		log.debugWarning("Fasih: direct install failed", exc_info=True)
	try:
		os.startfile(path)
	except Exception:
		log.error("Fasih: could not open the update", exc_info=True)


class _UpdateChecker(object):
	"""Checks GitHub for a newer Fasih (and newer voice packs) on request, and once a day at
	start-up when automatic checking is on."""

	def __init__(self, plugin):
		self.plugin = plugin
		self.running = False

	def check(self, manual):
		if self.running:
			return
		self.running = True
		include_betas = bool(_setting("updateBetas", False)) or _is_prerelease_version(_current_version())

		def work():
			res, err = None, None
			try:
				res = (_latest_release(include_betas), _pack_updates())
			except Exception as e:
				log.debugWarning("Fasih: update check failed", exc_info=True)
				err = e
			wx.CallAfter(self._result, res, err, manual)

		threading.Thread(target=work, daemon=True).start()

	def _result(self, res, err, manual):
		self.running = False
		_set_setting("lastUpdateCheck", time.time())
		current = _current_version()
		if err or res is None:
			if manual:
				messageBox(_("Could not check for updates. Check your internet connection."), _("Fasih update"), style=wx.OK | wx.ICON_WARNING)
			return
		rel, packs = res
		newer = rel is not None and (_vkey(current) is None or rel["key"] > _vkey(current))
		if newer and (manual or _setting("skippedVersion") != rel["version"]):
			gui.mainFrame.prePopup()
			try:
				d = UpdateDialog(gui.mainFrame, rel, current, packs)
				d.ShowModal()
				choice = d.choice
				d.Destroy()
			finally:
				gui.mainFrame.postPopup()
			if choice == "install":
				_UpdateDownloader(rel).start()
			elif choice == "skip":
				_set_setting("skippedVersion", rel["version"])
			return
		if packs:
			names = ", ".join("%s %s" % (n, v) for n, _o, v in packs)
			if messageBox(_("Fasih is up to date, but newer versions of these voices are available: {names}. Open Download voices to update them?").format(names=names), _("Fasih update"), style=wx.YES_NO | wx.ICON_INFORMATION) == wx.YES:
				self.plugin.onVoices()
			return
		if manual:
			if rel is not None and _vkey(current) and rel["key"] < _vkey(current):
				msg = _("You have Fasih {cur}, newer than the latest published release ({new}).").format(cur=current, new=rel["version"])
			else:
				msg = _("Fasih {cur} is up to date, and so are your voices.").format(cur=current)
			messageBox(msg, _("Fasih update"), style=wx.OK | wx.ICON_INFORMATION)

	def maybe_auto(self):
		if not _setting("autoUpdateCheck", True):
			return
		if time.time() - float(_setting("lastUpdateCheck", 0) or 0) < 20 * 3600:
			return
		self.check(manual=False)


class GlobalPlugin(globalPluginHandler.GlobalPlugin):

	def __init__(self):
		super().__init__()
		self.menu = None
		if globalVars.appArgs.secure:
			return
		tools = gui.mainFrame.sysTrayIcon.toolsMenu
		self.menu = wx.Menu()
		item = self.menu.Append(wx.ID_ANY, _("&Download voices..."), _("Download more Fasih voices and voices in other languages"))
		gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self.onVoices, item)
		item = self.menu.Append(wx.ID_ANY, _("&Pronunciation dictionary..."), _("Words Fasih should say differently"))
		gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self.onDictionary, item)
		item = self.menu.Append(wx.ID_ANY, _("Dia&critize text..."), _("Add harakat to Arabic text of any length, with the diacritizer you choose"))
		gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self.onDiacritize, item)
		self.menu.AppendSeparator()
		item = self.menu.Append(wx.ID_ANY, _("Check for &updates..."), _("See whether a newer Fasih or newer voices are available"))
		gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, lambda e: self.updater.check(manual=True), item)
		self.autoItem = self.menu.AppendCheckItem(wx.ID_ANY, _("Check for updates &automatically"), _("Once a day, when NVDA starts"))
		self.autoItem.Check(bool(_setting("autoUpdateCheck", True)))
		gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, lambda e: _set_setting("autoUpdateCheck", self.autoItem.IsChecked()), self.autoItem)
		self.betaItem = self.menu.AppendCheckItem(wx.ID_ANY, _("Include &beta versions"), _("Also offer test versions of Fasih"))
		self.betaItem.Check(bool(_setting("updateBetas", False)))
		gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, lambda e: _set_setting("updateBetas", self.betaItem.IsChecked()), self.betaItem)
		self.menuItem = tools.AppendSubMenu(self.menu, _("&Fasih"))
		self.updater = _UpdateChecker(self)
		wx.CallLater(6000, self._movedVoiceNotice)
		wx.CallLater(20000, self.updater.maybe_auto)

	def _movedVoiceNotice(self):
		"""Once after updating from 1.2: the chosen voice is now a download."""
		import os
		path = os.path.join(globalVars.appArgs.configPath, "Fasih-config", "missing_voice.txt")
		try:
			with open(path, encoding="utf-8") as f:
				voice = f.read().strip()
			os.remove(path)
		except Exception:
			return
		if not voice:
			return
		msg = _("{voice} is no longer part of the Fasih add-on, so Fasih is speaking with Aseel for now. "
		        "{voice} and the other Arabic voices can be downloaded from Tools > Fasih > Download voices. "
		        "Open it now?").format(voice=voice)
		gui.mainFrame.prePopup()
		try:
			answer = messageBox(msg, _("Fasih"), style=wx.YES_NO | wx.ICON_INFORMATION)
		finally:
			gui.mainFrame.postPopup()
		if answer == wx.YES:
			self.onVoices()

	def onDiacritize(self, evt=None):
		gui.mainFrame.prePopup()
		d = DiacritizeDialog(gui.mainFrame)
		d.Show()
		gui.mainFrame.postPopup()

	def onDictionary(self, evt=None):
		gui.mainFrame.prePopup()
		d = DictionaryDialog(gui.mainFrame)
		d.ShowModal()
		d.Destroy()
		gui.mainFrame.postPopup()

	def onVoices(self, evt=None):
		import systemUtils
		def fetch():
			langs = []
			try:
				fv = requests.get(FASIH_URL, timeout=TIMEOUT).json()
				if fv.get("voices"):
					langs.append({"lang2code": "ar", "name": "Arabic (Fasih)", "fasih": True, "voices": fv["voices"]})
			except Exception:
				log.error("Fasih: Fasih voice list unavailable", exc_info=True)
			try:
				langs += _catalogue().get("languages", [])
			except Exception:
				log.error("Fasih: voice catalogue unavailable", exc_info=True)
			return {"languages": langs} if langs else None
		progress = gui.IndeterminateProgressDialog(gui.mainFrame, _("Fasih"), _("Getting the list of voices..."))
		try:
			data = systemUtils.ExecAndPump(fetch).funcRes
		finally:
			progress.done()
			progress.Destroy()
		if not data or not data.get("languages"):
			messageBox(_("The list of voices could not be retrieved. Check your internet connection."), _("Error"), style=wx.OK | wx.ICON_ERROR)
			return
		gui.mainFrame.prePopup()
		dlg = VoicesDialog(gui.mainFrame, data)
		dlg.Show()
		gui.mainFrame.postPopup()

	def terminate(self):
		if self.menu is not None:
			try:
				gui.mainFrame.sysTrayIcon.toolsMenu.Remove(self.menuItem)
			except Exception:
				pass
