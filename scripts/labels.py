"""Parse RHVoice full-context labels into integer feature matrices, and build
HTS-style questions (each question = one field taking a value in a set).

Field layout comes from the language's labelling.xml, so the questions always
match what the engine produces at synthesis time.
"""
import os, re, json
import xml.etree.ElementTree as ET
import numpy as np


def field_layout(labelling_xml):
    """-> list of (feature_name, left_delim, right_delim)"""
    root = ET.parse(labelling_xml).getroot()
    fmt = root.find("format")
    items = []  # alternate text / feature
    if fmt.text and fmt.text.strip():
        items.append(("text", fmt.text.strip()))
    for el in fmt:
        items.append(("feat", el.get("name")))
        if el.tail and el.tail.strip():
            items.append(("text", el.tail.strip()))
    fields = []
    for i, (k, v) in enumerate(items):
        if k != "feat":
            continue
        left = items[i - 1][1] if i > 0 and items[i - 1][0] == "text" else ""
        right = items[i + 1][1] if i + 1 < len(items) and items[i + 1][0] == "text" else ""
        fields.append((v, left, right))
    return fields


def make_parser(fields):
    parts = []
    for name, left, right in fields:
        if not parts and left:
            parts.append(re.escape(left))
        parts.append(r"(.*?)")
        if right:
            parts.append(re.escape(right))
    return re.compile("^" + "".join(parts) + "$")


def read_lab(path):
    """-> list of (start, end, label) ; start/end None if absent"""
    out = []
    for line in open(path, encoding="utf-8"):
        p = line.split()
        if not p:
            continue
        if len(p) >= 3:
            out.append((int(p[0]), int(p[1]), p[-1]))
        else:
            out.append((None, None, p[-1]))
    return out


def phoneme_features(phonemes_xml):
    root = ET.parse(phonemes_xml).getroot()
    feats = {}
    for ph in root.findall("phoneme"):
        a = dict(ph.attrib)
        feats[a.pop("name")] = a
    return feats


PHONE_FIELDS = ["prev_prev_name", "prev_name", "name", "next_name", "next_next_name"]


def build_questions(fields, values, phfeats, numeric_le=True):
    """values: dict field -> sorted list of observed string values.
    Returns list of dict(name, field, set(values), patterns)."""
    qs = []
    pos = {f[0]: i for i, f in enumerate(fields)}

    def pats(fi, vals):
        name, left, right = fields[fi]
        out = []
        for v in vals:
            if fi == 0:
                out.append(f"{v}{right}*")
            elif fi == len(fields) - 1:
                out.append(f"*{left}{v}")
            else:
                out.append(f"*{left}{v}{right}*")
        return out

    seen = set()

    def add(qname, fname, vals):
        vals = sorted(set(vals) & set(values[fname]))
        if not vals or len(vals) == len(values[fname]):
            return
        key = (fname, tuple(vals))
        if key in seen:
            return
        seen.add(key)
        qs.append({"name": qname, "field": fname, "values": vals, "patterns": pats(pos[fname], vals)})

    for fname, _l, _r in fields:
        if fname == "none" or fname not in values:
            continue
        vals = values[fname]
        if fname in PHONE_FIELDS:
            for v in vals:
                add(f"{fname}=={v}", fname, [v])
            # phonetic classes
            classes = {}
            for ph, fe in phfeats.items():
                for k, v in fe.items():
                    classes.setdefault((k, v), []).append(ph)
            for (k, v), phs in sorted(classes.items()):
                add(f"{fname}_{k}=={v}", fname, phs)
            # useful combos
            for k1, v1, k2, v2 in [("vc", "+", "vlng", "l"), ("vc", "-", "emph", "+"), ("cvox", "+", "ctype", "f"),
                                   ("cvox", "-", "ctype", "s"), ("cplace", "phar", "cvox", "+")]:
                phs = [p for p, fe in phfeats.items() if fe.get(k1) == v1 and fe.get(k2) == v2]
                add(f"{fname}_{k1}=={v1}&&{k2}=={v2}", fname, phs)
        else:
            for v in vals:
                add(f"{fname}=={v}", fname, [v])
            nums = sorted(int(v) for v in vals if v.isdigit())
            if numeric_le and len(nums) > 2:
                for t in nums[:-1]:
                    if t > 20:
                        break
                    add(f"{fname}<={t}", fname, [str(n) for n in nums if n <= t])
    return qs


class LabelSet:
    """All labels of the corpus, as integer codes per field."""

    def __init__(self, lang_dir):
        self.fields = field_layout(os.path.join(lang_dir, "labelling.xml"))
        self.rx = make_parser(self.fields)
        self.phfeats = phoneme_features(os.path.join(lang_dir, "phonemes.xml"))

    def parse(self, label):
        m = self.rx.match(label)
        if not m:
            raise ValueError("cannot parse label: " + label)
        return m.groups()
