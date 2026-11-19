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


class VoicesDialog(wx.Dialog):
	"""Languages and voices of the RHVoice catalogue, with Install and Remove."""

	def __init__(self, parent, pkgData):
		super().__init__(parent, title=_("Fasih: download RHVoice voices"))
		self.pkgData = pkgData
		self.restart = False
		langs = pkgData.get("languages", [])
		ui = (languageHandler.getLanguage() or "").split("_")[0]
		defIndex = next((i for i, l in enumerate(langs) if l.get("lang2code") == ui), 0)
		if not any(l.get("lang2code") == ui for l in langs):
			defIndex = next((i for i, l in enumerate(langs) if l.get("lang2code") == "en"), 0)
		sizer = wx.BoxSizer(wx.VERTICAL)
		sizer.Add(wx.StaticText(self, label=_("&Language:")), flag=wx.LEFT | wx.TOP, border=8)
		self.langChoice = wx.Choice(self, choices=[LANG_LABELS.get(l.get("lang2code"), l.get("name", "?")) for l in langs])
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
		closeButton = wx.Button(self, wx.ID_CLOSE, _("Cl&ose"))
		for b in (self.installButton, self.removeButton, closeButton):
			buttons.Add(b, flag=wx.ALL, border=4)
		sizer.Add(buttons, flag=wx.ALIGN_CENTER | wx.ALL, border=4)
		self.installButton.Bind(wx.EVT_BUTTON, self.onInstall)
		self.removeButton.Bind(wx.EVT_BUTTON, self.onRemove)
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
			b = VoiceBundle(lang, voice)
			self.bundles.append(b)
			if b.isInstalled:
				status = _("Will be removed after restart") if b.isPendingRemove else _("Installed")
			else:
				status = _("Will be installed after restart") if b.isPendingInstall else _("Not installed")
			self.voiceList.Append((voice["name"], status, b.version))
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
		self.installButton.Enable(bool(b) and not pending and not b.isInstalled)
		self.removeButton.Enable(bool(b) and not pending and b.isInstalled)

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
		messageBox(_("{voice} is installed. It will be available after NVDA restarts, as a secondary voice in Fasih.").format(voice=bundle._voice["name"]),
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


class GlobalPlugin(globalPluginHandler.GlobalPlugin):

	def __init__(self):
		super().__init__()
		self.menu = None
		if globalVars.appArgs.secure:
			return
		tools = gui.mainFrame.sysTrayIcon.toolsMenu
		self.menu = wx.Menu()
		item = self.menu.Append(wx.ID_ANY, _("&Download voices..."), _("Download more RHVoice voices for Fasih's secondary voice"))
		gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self.onVoices, item)
		self.menuItem = tools.AppendSubMenu(self.menu, _("&Fasih"))

	def onVoices(self, evt=None):
		import systemUtils
		def fetch():
			try:
				return _catalogue()
			except Exception:
				log.error("Fasih: voice catalogue unavailable", exc_info=True)
				return None
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
