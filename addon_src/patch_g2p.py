"""Fasih fix for ClaritySynth's _merge_user_marks: merge marks by component.
A user's shadda alone must not wipe out the model's vowel (شدّ -> شَدَّ, محمّد -> مُحَمَّد),
and a user's vowel alone must not drop the model's shadda.
usage: patch_g2p.py ar_g2p.py"""
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
old = "        pick = um[i] if um[i] else mm[i]\n"
assert old in s, "merge line not found"
new = '''        pick = um[i] if um[i] else mm[i]
        if um[i] and mm[i]:
            _SH = "\\u0651"
            u_v = um[i].replace(_SH, "")
            m_v = mm[i].replace(_SH, "")
            sh = _SH if (_SH in um[i] or _SH in mm[i]) else ""
            if not u_v:
                # the writer typed only a shadda: keep the model's vowel with it,
                # unless the model put sukun there (a shadda is never silent)
                u_v = "" if m_v == "\\u0652" else m_v
            if sh and u_v == "\\u0652":
                u_v = ""
            pick = sh + u_v
'''
s = s.replace(old, new, 1)
compile(s, p, "exec")
open(p, "w", encoding="utf-8").write(s)
print("ar_g2p patched")
