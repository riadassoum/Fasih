import argparse
import os
import pickle
import sys
import time
from collections import Counter, defaultdict
from typing import Dict, List, Optional, Tuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ENGINE_DIR = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.join(ENGINE_DIR, "python"))
sys.path.insert(0, SCRIPT_DIR)

from arabic_utils import DIAC_ID_TO_STR, extract_arabic_words, is_arabic_diacritic, is_arabic_letter
from eval_fast import FastEngine

SHADDA = "\u0651"
SUN_LETTERS = frozenset("تثدذرزسشصضطظلن")
REL_SKIP = frozenset({"الذي", "التي", "الذين", "اللواتي", "اللاتي", "اللذان", "اللتان", "اللذين", "اللتين"})


def diac_id_to_base_and_sh(did: int) -> Tuple[int, bool]:
    if did == 8:
        return 0, True
    if 9 <= did <= 14:
        return did - 8, True
    return did, False


def build_shadda_table(corpus_path: str, cache_path: Optional[str] = None, max_lines: int = 0) -> Tuple[Dict[Tuple[str, bytes], int], set]:
    """Builds exact lookup table (undiac, base_vowels_bytes) -> deterministic shadda_mask,
    plus a set of (undiac, base_vowels_bytes) keys known to be context-ambiguous in shadda.
    """
    if cache_path and os.path.isfile(cache_path):
        with open(cache_path, "rb") as f:
            return pickle.load(f)

    counts: Dict[Tuple[str, bytes], Counter] = defaultdict(Counter)
    t0 = time.time()
    with open(corpus_path, "r", encoding="utf-8", errors="ignore") as f:
        for idx, line in enumerate(f):
            if max_lines and idx >= max_lines:
                break
            for undiac, syms, diacs in extract_arabic_words(line):
                n = len(diacs)
                if n == 0 or n > 30:
                    continue
                bases = bytearray(n)
                sh_mask = 0
                for i, d in enumerate(diacs):
                    b, has_sh = diac_id_to_base_and_sh(d)
                    bases[i] = b
                    if has_sh:
                        sh_mask |= 1 << i
                counts[(undiac, bytes(bases))][sh_mask] += 1

    det_map: Dict[Tuple[str, bytes], int] = {}
    ambig_set = set()
    for key, cnts in counts.items():
        tot = sum(cnts.values())
        if tot < 3:
            continue
        top_mask, top_cnt = cnts.most_common(1)[0]
        ratio = top_cnt / tot
        if ratio >= 0.96:
            if top_mask != 0:
                det_map[key] = top_mask
        else:
            # Genuinely ambiguous in this (undiac, base_vowels) configuration
            ambig_set.add(key)

    print(f"Built shadda table from {corpus_path} in {time.time()-t0:.1f}s: det={len(det_map):,}, ambig={len(ambig_set):,}")
    if cache_path:
        os.makedirs(os.path.dirname(os.path.abspath(cache_path)), exist_ok=True)
        with open(cache_path, "wb") as f:
            pickle.dump((det_map, ambig_set), f, protocol=pickle.HIGHEST_PROTOCOL)
    return det_map, ambig_set


BASE_CHAR_TO_ID = {
    "\u064e": 1,
    "\u064f": 2,
    "\u0650": 3,
    "\u0652": 4,
    "\u064b": 5,
    "\u064c": 6,
    "\u064d": 7,
}


def parse_tokens_with_spans(line: str) -> List[Tuple[int, int, str, List[str], List[str], bytes, int]]:
    """Extracts exact character spans of Arabic words in line along with base vowels and existing shadda mask."""
    tokens = []
    n = len(line)
    i = 0
    while i < n:
        if not is_arabic_letter(line[i]):
            i += 1
            continue
        start = i
        letters: List[str] = []
        base_marks: List[str] = []
        base_ids = bytearray()
        sh_mask = 0
        while i < n and (is_arabic_letter(line[i]) or is_arabic_diacritic(line[i])):
            if is_arabic_letter(line[i]):
                ch = line[i]
                j = i + 1
                has_sh = False
                bid = 0
                bm_chars = []
                while j < n and is_arabic_diacritic(line[j]):
                    mc = line[j]
                    if mc == SHADDA:
                        has_sh = True
                    else:
                        bm_chars.append(mc)
                        v = BASE_CHAR_TO_ID.get(mc)
                        if v is not None:
                            bid = v
                    j += 1
                li = len(letters)
                letters.append(ch)
                if has_sh:
                    sh_mask |= 1 << li
                base_marks.append("".join(bm_chars))
                base_ids.append(bid)
                i = j
            else:
                i += 1
        end = i
        if letters:
            tokens.append((start, end, "".join(letters), letters, base_marks, bytes(base_ids), sh_mask))
    return tokens


def solar_lam_mask(undiac: str, base_marks: List[str]) -> int:
    """Deterministic solar lam shadda candidate bit when definite article ال precedes a sun letter without sukun."""
    n = len(undiac)
    if undiac in REL_SKIP or n < 4:
        return 0
    st = 0
    if undiac[0] in ("و", "ف") and n >= 5 and undiac[1] in ("ب", "ك", "ل") and undiac[2] == "ا" and undiac[3] == "ل":
        st = 2
    elif undiac[0] in ("و", "ف", "ب", "ك") and n >= 5 and undiac[1] == "ا" and undiac[2] == "ل":
        st = 1
    elif undiac[0] == "ا" and undiac[1] == "ل":
        st = 0
    else:
        return 0
    lam_pos = st + 1
    sun_pos = st + 2
    if sun_pos >= n or undiac[sun_pos] not in SUN_LETTERS:
        return 0
    # Only definite article when lam has NO sukun; if sun letter carries sukun it cannot take shadda
    if "\u0652" in base_marks[lam_pos] or "\u0652" in base_marks[sun_pos]:
        return 0
    # Guard Form VIII verbs like التَقَى / التَزَمَ where taa is infixed with bare lam
    if undiac[sun_pos] == "ت" and base_marks[lam_pos] == "" and n >= sun_pos + 3:
        return 0
    return 1 << sun_pos


def restore_batch(
    lines: List[str],
    det_map: Dict[Tuple[str, bytes], int],
    ambig_set: set,
    engine: Optional[FastEngine] = None,
) -> Tuple[List[str], Dict[str, int]]:
    stats = {"lines": len(lines), "tier1_solar": 0, "tier2_lex": 0, "tier3_ctx": 0}
    parsed = [parse_tokens_with_spans(l) for l in lines]

    need_ctx_indices: List[int] = []
    need_ctx_plain: List[str] = []
    for li, toks in enumerate(parsed):
        need = False
        for _, _, undiac, _, base_marks, base_bytes, sh_mask in toks:
            key = (undiac, base_bytes)
            if key in ambig_set:
                need = True
                break
            # Also ask context when line has an unrecognized word with solar-lam candidate (e.g. التَقَى vs السَّلام)
            if key not in det_map and solar_lam_mask(undiac, base_marks) != 0 and (sh_mask & solar_lam_mask(undiac, base_marks)) == 0:
                need = True
                break
        if need and engine is not None:
            need_ctx_indices.append(li)
            need_ctx_plain.append(" ".join(t[2] for t in toks))

    ctx_preds: Dict[int, List[Tuple[str, List[int]]]] = {}
    if need_ctx_plain and engine is not None:
        pred_lines = engine.diacritize_lines(need_ctx_plain)
        for li, pl in zip(need_ctx_indices, pred_lines):
            ctx_preds[li] = [(u, d) for u, _, d in extract_arabic_words(pl)]

    out_lines: List[str] = []
    for li, (line, toks) in enumerate(zip(lines, parsed)):
        if not toks:
            out_lines.append(line)
            continue
        p_words = ctx_preds.get(li)
        rebuilt = []
        cursor = 0
        for wi, (start, end, undiac, letters, base_marks, base_bytes, sh_mask) in enumerate(toks):
            rebuilt.append(line[cursor:start])
            new_mask = sh_mask
            key = (undiac, base_bytes)

            if key in det_map:
                add = det_map[key] & ~sh_mask
                if add:
                    new_mask |= add
                    stats["tier2_lex"] += 1
            else:
                sm = solar_lam_mask(undiac, base_marks)
                if sm and (sh_mask & sm) == 0:
                    # Prefer model verification on ambiguous solar stems (like التَقَى)
                    if p_words and wi < len(p_words) and p_words[wi][0] == undiac and len(p_words[wi][1]) == len(letters):
                        pd = p_words[wi][1]
                        pos = (sm.bit_length() - 1)
                        if pd[pos] >= 8:
                            new_mask |= sm
                            stats["tier1_solar"] += 1
                    elif key not in ambig_set and undiac[sm.bit_length() - 1] != "ت":
                        new_mask |= sm
                        stats["tier1_solar"] += 1

            if key in ambig_set and p_words and wi < len(p_words):
                pu, pd = p_words[wi]
                if pu == undiac and len(pd) == len(letters):
                    for k, d_pred in enumerate(pd):
                        b_pred, sh_pred = diac_id_to_base_and_sh(d_pred)
                        if sh_pred and not ((new_mask >> k) & 1):
                            # Accept contextual shadda when base vowel agrees or at case-ending when compatible
                            if b_pred == base_bytes[k] or base_bytes[k] == 0:
                                new_mask |= 1 << k
                                stats["tier3_ctx"] += 1

            # Reconstruct token preserving all original base vowels verbatim, inserting canonical SHADDA
            word_chars = []
            for k, (ch, bm) in enumerate(zip(letters, base_marks)):
                word_chars.append(ch)
                if (new_mask >> k) & 1:
                    if "\u0652" not in bm:
                        word_chars.append(SHADDA)
                word_chars.append(bm)
            rebuilt.append("".join(word_chars))
            cursor = end
        rebuilt.append(line[cursor:])
        out_lines.append("".join(rebuilt))
    return out_lines, stats


def evaluate_on_gold(gold_lines: List[str], det_map, ambig_set, engine: FastEngine) -> None:
    stripped = [l.replace(SHADDA, "") for l in gold_lines]
    restored, st = restore_batch(stripped, det_map, ambig_set, engine)
    tp = fp = fn = tn = 0
    for g, r in zip(gold_lines, restored):
        gw = extract_arabic_words(g)
        rw = extract_arabic_words(r)
        for (gu, _, gd), (ru, _, rd) in zip(gw, rw):
            if gu != ru or len(gd) != len(rd):
                continue
            for a, b in zip(gd, rd):
                ga = diac_id_to_base_and_sh(a)[1]
                rb = diac_id_to_base_and_sh(b)[1]
                if ga and rb: tp += 1
                elif not ga and rb: fp += 1
                elif ga and not rb: fn += 1
                else: tn += 1
    prec = 100.0 * tp / max(1, tp + fp)
    rec = 100.0 * tp / max(1, tp + fn)
    f1 = 2 * prec * rec / max(1e-9, prec + rec)
    print(f"Shadda eval ({len(gold_lines)} lines): Precision={prec:.2f}% Recall={rec:.2f}% F1={f1:.2f}% | TP={tp:,} FP={fp:,} FN={fn:,} | stats={st}")


def main():
    parser = argparse.ArgumentParser(description="High-accuracy 3-tier Arabic Shadda Restorer")
    parser.add_argument("--ref-corpus", required=True, help="Clean reference corpus to extract shadda table")
    parser.add_argument("--cache", default="", help="Optional pickle cache path for shadda table")
    parser.add_argument("--model", required=True, help="Path to .tash model for Tier-3 contextual shadda resolution")
    parser.add_argument("--in-file", default="", help="Input corpus file to restore shaddas on")
    parser.add_argument("--out-file", default="", help="Output restored file")
    parser.add_argument("--eval-heldout", default="", help="Optional clean gold file to benchmark shadda P/R/F1")
    parser.add_argument("--eval-lines", type=int, default=3000)
    parser.add_argument("--batch-size", type=int, default=50000)
    args = parser.parse_args()

    det_map, ambig_set = build_shadda_table(args.ref_corpus, args.cache or None)
    eng = FastEngine(args.model, workers=4)

    if args.eval_heldout:
        with open(args.eval_heldout, "r", encoding="utf-8", errors="ignore") as f:
            gl = [l.strip() for _, l in zip(range(args.eval_lines), f) if l.strip()]
        evaluate_on_gold(gl, det_map, ambig_set, eng)

    if args.in_file and args.out_file:
        t0 = time.time()
        os.makedirs(os.path.dirname(os.path.abspath(args.out_file)), exist_ok=True)
        tot_stats = Counter()
        batch: List[str] = []
        with open(args.in_file, "r", encoding="utf-8", errors="ignore") as fin, \
             open(args.out_file, "w", encoding="utf-8") as fout:
            for line in fin:
                s = line.strip()
                if not s:
                    continue
                batch.append(s)
                if len(batch) >= args.batch_size:
                    res, st = restore_batch(batch, det_map, ambig_set, eng)
                    for r in res:
                        fout.write(r + "\n")
                    tot_stats.update(st)
                    batch.clear()
                    if tot_stats["lines"] % 200000 == 0:
                        print(f"  Restored {tot_stats['lines']:,} lines ({time.time()-t0:.1f}s) | {dict(tot_stats)}")
            if batch:
                res, st = restore_batch(batch, det_map, ambig_set, eng)
                for r in res:
                    fout.write(r + "\n")
                tot_stats.update(st)
        print(f"Done restoring {tot_stats['lines']:,} lines in {time.time()-t0:.1f}s | {dict(tot_stats)}")
    eng.close()


if __name__ == "__main__":
    main()
