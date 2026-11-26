"""Inputs that make the engine drop a whole sentence must never leave Fasih's text pipeline.

Found by trying every letter and mark pair against the engine: alef wasla straight after a letter
without a vowel, tatweel, the rare letters U+063B-U+063F, and ژ or ڨ after an alef or under a
shadda. This test needs no engine: it checks the pipeline's output text only.
"""
import os, re, sys, types
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
for m in ["logHandler", "globalVars", "addonHandler", "config", "synthDriverHandler", "speech", "speech.commands",
          "nvwave", "languageHandler", "driverHandler", "autoSettingsUtils", "autoSettingsUtils.driverSetting"]:
    sys.modules.setdefault(m, mock.MagicMock())

BAD = re.compile("(?<=[ء-يٱ-ۓ])ٱ|[ـػ-ؿ]"
                 "|(?<=[اٱ])[ژڨ]|[ژڨ](?=ّ)")


def load(addon_dir):
    pkg = types.ModuleType("fasih")
    pkg.__path__ = [addon_dir]
    sys.modules["fasih"] = pkg
    from fasih import arabic_pipeline
    return arabic_pipeline


def main(addon_dir):
    P = load(addon_dir)
    letters = [chr(c) for c in range(0x621, 0x64B) if chr(c).isalpha()] + list("ٱپچژڤڨگکیىـ")
    marks = list("ًٌٍَُِّْٰ")
    cases = [a + b for a in letters for b in letters] + [a + m for a in letters for m in marks]
    cases += [a + m + "ال" for a in letters for m in ("", "ِ", "َ")]
    cases += ["وَعُتبَةَ قَد تَرَكنا پِالجَبوبِ"]
    fails = [c for c in cases if BAD.search(P.process("كَتَبَ " + c + " الدَّرْسَ", diacritize=False) or "")]
    print("%d cases, %d unsafe for the engine" % (len(cases), len(fails)), fails[:10])
    return len(fails)


if __name__ == "__main__":
    sys.exit(1 if main(sys.argv[1]) else 0)
