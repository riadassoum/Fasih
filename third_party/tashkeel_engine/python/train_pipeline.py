import argparse
import math
import multiprocessing as mp
import os
import random
import time
from collections import Counter, defaultdict
from typing import Dict, List, Set, Tuple

import numpy as np

from arabic_utils import (
    DIAC_DAMMA,
    DIAC_FATHA,
    DIAC_KASRA,
    DIAC_NONE,
    DIAC_SHADDA,
    DIAC_SHADDA_FATHA,
    DIAC_SUKUN,
    NUM_DIACRITICS,
    extract_arabic_words,
    infer_word_flags,
)
from compress_model import convert_v2_to_v3, write_v5_model
from fst_builder import (
    DEFAULT_WEIGHTS,
    WORD_CAND_STRUCT,
    WORD_META_STRUCT,
    build_bigram_table,
    build_bitmapped_fst,
    build_bitmapped_fst_v5,
    export_lightgbm_ensemble,
    export_sklearn_rf,
    write_tash_model,
)
from grammar import (
    PART_AMMA,
    PART_AN,
    PART_AY,
    PART_COND,
    PART_CONJ,
    PART_DEM,
    PART_GEN,
    PART_HATTA,
    PART_IDAFA_NOUN,
    PART_ILLA,
    PART_IN,
    PART_INNA,
    PART_JAZM,
    PART_KANA,
    PART_LA,
    PART_LAKIN,
    PART_LAMA,
    PART_LI_AN,
    PART_NASB,
    PART_NONE,
    PART_NUM_IDAFA,
    PART_PREP_PRON,
    PART_PRON,
    PART_QAD,
    PART_QUEST,
    PART_REL,
    PART_SAWFA,
    PART_YA,
    S_ALEF,
    S_ALEF_HAMZA_A,
    S_ALEF_HAMZA_B,
    S_ALEF_MAKSURA,
    S_ALEF_WASLA,
    S_BEH,
    S_FEH,
    S_HEH,
    S_KAF,
    S_LAM,
    S_MEEM,
    S_NOON,
    S_SEEN,
    S_TEH,
    S_TEH_MARBUTA,
    S_WAW,
    S_YEH,
    SUN_LETTERS,
    analyze_surface,
)


def _has_stem_in_vocab(undiac_stem: str, vocab_set: Set[str]) -> bool:
    if len(undiac_stem) < 2:
        return False
    if undiac_stem in vocab_set:
        return True
    if undiac_stem.endswith("ت") and (undiac_stem[:-1] + "ة") in vocab_set:
        return True
    if undiac_stem.endswith("ا") and (undiac_stem[:-1] + "ى") in vocab_set:
        return True
    if len(undiac_stem) >= 4 and undiac_stem[0] in ("و", "ف", "ب", "ل", "ك"):
        if undiac_stem[1:] in vocab_set:
            return True
    return False


def resolve_case_pos(undiac: str, syms: bytes, diacs: List[int], vocab_set: Set[str], si: dict) -> Tuple[int, bool]:
    """Matches C++ finalize_cand case_pos & suffix detection logic. Returns (cpos, has_suffix)."""
    n = len(syms)
    if n <= 1:
        return 0, False
    cpos = n - 2 if (n >= 3 and syms[n - 1] == S_ALEF) else n - 1
    if si["particle"] == PART_NONE and si["al_len"] == 0 and n == 3 and syms[0] in (S_ALEF_HAMZA_A, S_ALEF_HAMZA_B) and syms[1] == S_NOON and syms[2] == S_HEH and diacs[2] in (DIAC_DAMMA, DIAC_KASRA):
        return 1, True
    if si["particle"] == PART_NONE and si["al_len"] == 0 and n >= 4 and diacs[n - 1] not in (5, 6, 7, 12, 13, 14):
        sl = 0
        if n >= 6 and syms[n - 3] in (S_HEH, S_KAF) and syms[n - 2] == S_MEEM and syms[n - 1] == S_ALEF and diacs[n - 1] == DIAC_NONE:
            sl = 3
        elif n >= 5 and (
            (syms[n - 2] == S_HEH and (
                (syms[n - 1] == S_ALEF and diacs[n - 1] == DIAC_NONE) or
                (syms[n - 1] == S_MEEM and diacs[n - 1] in (DIAC_SUKUN, DIAC_DAMMA)) or
                (syms[n - 1] == S_NOON and diacs[n - 1] == DIAC_SHADDA_FATHA)
            )) or
            (syms[n - 2] == S_KAF and (
                (syms[n - 1] == S_MEEM and diacs[n - 1] in (DIAC_SUKUN, DIAC_DAMMA)) or
                (syms[n - 1] == S_NOON and diacs[n - 1] == DIAC_SHADDA_FATHA)
            )) or
            (syms[n - 2] == S_NOON and syms[n - 1] in (S_ALEF, S_YEH) and diacs[n - 1] == DIAC_NONE)
        ):
            sl = 2
        elif (syms[n - 1] == S_HEH and diacs[n - 1] in (DIAC_DAMMA, DIAC_KASRA)) or (
            syms[n - 1] == S_KAF and diacs[n - 1] in (DIAC_FATHA, DIAC_KASRA)
        ):
            sl = 1

        if sl > 0 and n - sl >= 2:
            stem_len = n - sl
            if diacs[stem_len - 1] not in (5, 6, 7, 12, 13, 14) and _has_stem_in_vocab(undiac[:stem_len], vocab_set):
                return stem_len - 1, True
    return cpos, False


def compute_trans_matrix(trans_counts: np.ndarray) -> bytes:
    alpha = 0.5
    cond_counts = trans_counts.astype(np.float64) + alpha
    row_sums = cond_counts.sum(axis=3, keepdims=True)
    p_cond = cond_counts / np.maximum(row_sums, 1.0)

    role_counts = trans_counts.sum(axis=(0, 1), keepdims=True).astype(np.float64) + alpha * 16.0
    p_base = role_counts / np.maximum(role_counts.sum(axis=3, keepdims=True), 1.0)

    score = np.log(p_cond / np.maximum(p_base, 1e-6)) * 400.0
    score = np.clip(np.round(score), -4000, 4000).astype(np.int16)
    return score.tobytes()


def build_oov_dataset(sampled_lines: List[str], max_samples: int = 450000):
    X_list = []
    y_list = []
    for line in sampled_lines:
        words = extract_arabic_words(line)
        for _, syms, diacs in words:
            n = len(syms)
            if n == 0:
                continue
            si = analyze_surface(syms)
            al_len = si["al_len"]
            stem_len = n - al_len
            last_sym = syms[n - 1]
            is_sun_al = 1 if (al_len >= 2 and al_len < n and syms[al_len] in SUN_LETTERS) else 0
            starts_meem = 1 if (al_len < n and syms[al_len] == S_MEEM) else 0
            starts_imp = si["verb_like"]
            ends_tm = 1 if last_sym == S_TEH_MARBUTA else 0
            ends_at = 1 if (n >= 4 and syms[n - 2] == S_ALEF and last_sym == S_TEH) else 0
            ends_wa = 1 if (n >= 4 and syms[n - 2] == S_WAW and last_sym == S_ALEF) else 0
            for i in range(n):
                feats = [
                    syms[i - 3] if i >= 3 else -1,
                    syms[i - 2] if i >= 2 else -1,
                    syms[i - 1] if i >= 1 else -1,
                    syms[i],
                    syms[i + 1] if i + 1 < n else -1,
                    syms[i + 2] if i + 2 < n else -1,
                    syms[i + 3] if i + 3 < n else -1,
                    i,
                    n - 1 - i,
                    n,
                    al_len,
                    i - al_len,
                    diacs[i - 1] if i >= 1 else -1,
                    diacs[i - 2] if i >= 2 else -1,
                    stem_len,
                    last_sym,
                    is_sun_al,
                    starts_meem,
                    starts_imp,
                    ends_tm,
                    ends_at,
                    ends_wa,
                ]
                X_list.append(feats)
                y_list.append(diacs[i])
                if len(X_list) >= max_samples:
                    return np.array(X_list, dtype=np.int32), np.array(y_list, dtype=np.int32)
    return np.array(X_list, dtype=np.int32), np.array(y_list, dtype=np.int32)


def _diac_fam_py(d: int) -> int:
    if d in (2, 6, 10, 13): return 1
    if d in (1, 5, 9, 12): return 2
    if d in (3, 7, 11, 14): return 3
    if d == 4: return 4
    return 0


def _is_fem_at_py(syms: bytes, al_len: int) -> int:
    n = len(syms)
    if n >= al_len + 5 and syms[n - 1] == S_TEH and syms[n - 2] == S_ALEF and syms[al_len] not in (S_ALEF_HAMZA_A, S_ALEF):
        return 1
    return 0


def _is_ibn_py(syms: bytes) -> int:
    n = len(syms)
    st = 1 if (n >= 4 and syms[0] in (S_WAW, S_FEH)) else 0
    w = syms[st:]
    ln = len(w)
    if ln == 2 and w[0] == S_BEH and w[1] == S_NOON:
        return 1
    if ln == 3 and w[0] in (S_ALEF, S_ALEF_WASLA) and w[1] == S_BEH and w[2] == S_NOON:
        return 1
    if ln == 3 and w[0] == S_BEH and w[1] == S_NOON and w[2] == S_TEH:
        return 1
    return 0


def _is_adj_surface_py(syms: bytes, al_len: int) -> int:
    n = len(syms)
    if n <= al_len + 2:
        return 0
    w = syms[al_len:]
    sn = len(w)
    if sn >= 4 and w[sn - 1] == S_YEH and w[0] not in (S_YEH, S_TEH, S_NOON):
        return 1
    if sn >= 5 and w[sn - 2] == S_YEH and w[sn - 1] == S_TEH_MARBUTA:
        return 1
    if sn >= 5 and w[0] == S_MEEM and (
        w[1] in (S_TEH, S_NOON) or (w[1] == S_SEEN and w[2] == S_TEH) or
        w[sn - 2] == S_WAW or (sn >= 6 and w[sn - 1] == S_TEH_MARBUTA and w[sn - 3] == S_WAW)
    ):
        return 1
    if sn == 4 and w[0] == S_ALEF_HAMZA_A and w[3] not in (S_ALEF, S_ALEF_MAKSURA):
        return 1
    if (sn == 4 and w[2] == S_YEH and w[0] not in (S_YEH, S_TEH, S_MEEM)) or (
        sn == 5 and w[2] == S_YEH and w[4] == S_TEH_MARBUTA and w[0] != S_MEEM
    ):
        return 1
    return 0


def _is_diptote_surface_py(syms: bytes, st: int) -> int:
    n = len(syms)
    if n <= st + 2:
        return 0
    w = syms[st:]
    sn = len(w)
    if w[sn - 1] == S_TEH_MARBUTA:
        return 0
    if sn == 5 and w[2] == S_ALEF and w[4] not in (S_ALEF, S_ALEF_MAKSURA) and (w[3] == 5 or w[0] == S_MEEM or w[1] == S_WAW):
        return 1
    if sn == 6 and w[2] == S_ALEF and w[4] == S_YEH and w[5] != S_YEH and w[0] in (S_MEEM, S_TEH, S_ALEF_HAMZA_A):
        return 1
    if sn >= 5 and w[sn - 2] == S_ALEF and w[sn - 1] == 0 and w[0] not in (S_ALEF, S_ALEF_HAMZA_B) and w[sn - 3] != S_ALEF:
        return 1
    if sn >= 6 and w[sn - 2] == S_BEH and w[sn - 1] == 16 and w[0] in (S_SEEN, S_ALEF_HAMZA_A, S_NOON, 14):
        return 1
    return 0


def _is_verb_cand_py(undiac: str, syms: bytes, diacs: List[int], cpos: int, si: dict) -> int:
    if si["al_len"] > 0 or si["particle"] != PART_NONE:
        return 0
    if diacs[cpos] in (5, 6, 7, 12, 13, 14):
        return 0
    if si["verb_like"] == 2:
        return 1
    if len(syms) >= 3 and undiac[0] in ("ي", "ت", "ن") and diacs[cpos] in (DIAC_DAMMA, DIAC_FATHA, DIAC_SUKUN):
        return 1
    return 0


def _analyze_pos_mc_py(undiac: str, syms: bytes, diacs: List[int], cpos: int, si: dict) -> Tuple[int, int, int]:
    """Returns (top_verb, pos_tag, morph_class) aligned with C++ finalize_cand."""
    n = len(syms)
    pid = si["particle"]
    al_len = si["al_len"]
    if pid not in (PART_NONE, PART_KANA, PART_IDAFA_NOUN, PART_NUM_IDAFA):
        sh_any = any(d >= DIAC_SHADDA for d in diacs)
        return 0, 10, (9 if sh_any else 10)

    if al_len > 0:
        sh_core = any(d >= DIAC_SHADDA for d in diacs[al_len + 1 : cpos])
        if sh_core:
            mc = 7
        else:
            f0 = _diac_fam_py(diacs[al_len]) if al_len < n else 0
            mc = 4 if f0 == 2 else (5 if f0 == 3 else (6 if f0 == 1 else 11))
        return 0, 7, mc

    sn = cpos + 1 if cpos < n - 1 and not (n >= 3 and syms[n - 1] == S_ALEF and cpos == n - 2) else n
    st = 0
    if sn >= 4 and syms[0] in (S_WAW, S_FEH) and (syms[1] in (S_YEH, S_TEH, S_NOON, S_ALEF_HAMZA_A, S_SEEN) or diacs[0] == DIAC_FATHA):
        st = 1
    if st < sn and syms[st] == S_SEEN and sn - st >= 4 and syms[st + 1] in (S_YEH, S_TEH, S_NOON, S_ALEF_HAMZA_A) and diacs[st] == DIAC_FATHA:
        st += 1

    is_v = _is_verb_cand_py(undiac, syms, diacs, cpos, si)
    slen = sn - st
    is_past_v = False
    is_pass = False
    is_past_fem = False
    is_past_subj = False
    if not is_v and slen >= 3 and diacs[cpos] not in (5, 6, 7, 12, 13, 14) and syms[sn - 1] != S_TEH_MARBUTA:
        if slen == 3 and diacs[st + 2] in (DIAC_FATHA, DIAC_SHADDA_FATHA) and diacs[st] in (DIAC_FATHA, DIAC_DAMMA):
            is_past_v = True
            if diacs[st] == DIAC_DAMMA and diacs[st + 1] in (DIAC_KASRA, 11):
                is_pass = True
        elif slen >= 4 and syms[sn - 1] == S_TEH and diacs[sn - 1] in (DIAC_SUKUN, DIAC_KASRA, DIAC_DAMMA, DIAC_FATHA) and diacs[st] in (DIAC_FATHA, DIAC_DAMMA):
            is_past_v = True
            if diacs[sn - 1] == DIAC_SUKUN or (diacs[sn - 1] == DIAC_KASRA and diacs[sn - 2] == DIAC_FATHA):
                is_past_fem = True
            elif diacs[sn - 2] == DIAC_SUKUN:
                is_past_subj = True
            if diacs[st] == DIAC_DAMMA and diacs[sn - 2] == DIAC_FATHA and (slen > 4 or diacs[st + 1] == DIAC_KASRA):
                is_pass = True
        elif slen >= 4 and syms[sn - 1] == S_ALEF and syms[sn - 2] == S_WAW and diacs[sn - 3] == DIAC_DAMMA and diacs[st] in (DIAC_FATHA, DIAC_DAMMA):
            is_past_v = True
            is_past_subj = True
            if diacs[st] == DIAC_DAMMA and slen >= 5:
                is_pass = True
        elif slen == 4 and syms[st] != S_MEEM and syms[st + 3] in (S_ALEF_MAKSURA, S_ALEF) and diacs[st + 3] == DIAC_NONE and diacs[st] == DIAC_FATHA and diacs[st + 2] in (DIAC_FATHA, DIAC_SHADDA_FATHA):
            is_past_v = True
        elif slen >= 4 and diacs[sn - 1] in (DIAC_FATHA, DIAC_SHADDA_FATHA) and diacs[st] in (DIAC_FATHA, DIAC_DAMMA) and syms[st] not in (S_MEEM,):
            if slen == 4 and diacs[st + 2] in (DIAC_FATHA, DIAC_SHADDA_FATHA, DIAC_KASRA):
                is_past_v = True
                if diacs[st] == DIAC_DAMMA and diacs[st + 2] == DIAC_KASRA:
                    is_pass = True
            elif slen >= 5 and syms[st] in (S_ALEF, S_TEH, S_ALEF_HAMZA_A):
                is_past_v = True
                if diacs[st] == DIAC_DAMMA and diacs[sn - 2] == DIAC_KASRA:
                    is_pass = True

    if is_v:
        if diacs[st] == DIAC_DAMMA and slen >= 4 and diacs[sn - 2] in (DIAC_FATHA, DIAC_SHADDA_FATHA):
            return 1, 4, 3
        pos = 5 if syms[st] in (S_ALEF_HAMZA_A, S_NOON) else 6
        return 1, pos, 2
    if is_past_v:
        if is_pass:
            return 1, 4, 1
        pos = 3 if is_past_fem else (2 if is_past_subj else 1)
        sh_core = any(d >= DIAC_SHADDA for d in diacs[st:cpos])
        return 1, pos, (8 if sh_core else 0)

    pos = 9 if sn < n else 8
    sh_core = any(d >= DIAC_SHADDA for d in diacs[st + 1 : cpos])
    if sh_core:
        mc = 7
    else:
        f0 = _diac_fam_py(diacs[st]) if st < n else 0
        mc = 4 if f0 == 2 else (5 if f0 == 3 else (6 if f0 == 1 else 11))
    return 0, pos, mc


def _cand_role_v5_py(pid: int, pos_tag: int, mc: int) -> int:
    """Matches C++ Diacritizer::cand_role_v5 exactly (24 roles: 0..23)."""
    if pid == PART_GEN: return 1
    if pid in (PART_NASB, PART_HATTA): return 2
    if pid == PART_JAZM: return 3
    if pid == PART_LAMA: return 13 if mc == 9 else 3
    if pid in (PART_AN, PART_LI_AN): return 4 if mc == 9 else 2
    if pid == PART_IN: return 4 if mc == 9 else 13
    if pid == PART_LAKIN: return 4 if mc == 9 else 6
    if pid == PART_INNA: return 4
    if pid == PART_KANA: return 5
    if pid == PART_CONJ: return 6
    if pid == PART_LA: return 7
    if pid in (PART_QAD, PART_SAWFA): return 8
    if pid == PART_DEM: return 9
    if pid == PART_REL: return 10
    if pid == PART_PRON: return 11
    if pid in (PART_IDAFA_NOUN, PART_NUM_IDAFA): return 12
    if pid in (PART_COND, PART_QUEST, PART_AMMA, PART_YA, PART_AY, PART_ILLA): return 13
    if pid == PART_PREP_PRON: return 14
    if pos_tag == 1: return 15
    if pos_tag == 2: return 16
    if pos_tag == 3: return 17
    if pos_tag == 4: return 18
    if pos_tag == 5: return 19
    if pos_tag == 6: return 20
    if pos_tag == 7: return 21
    if pos_tag == 9: return 22
    return 23


def build_irab_dataset(
    sampled_lines: List[str],
    vocab_set: Set[str],
    word_top_case: Dict[str, int],
    word_top_verb: Dict[str, int],
    word_top_pos: Dict[str, int],
    max_samples: int = 650000,
    word_top_mc: Dict[str, int] = None,
):
    """Builds 56-feature syntactic I'rab dataset 100% aligned with C++ diacritizer.cpp."""
    if word_top_mc is None:
        word_top_mc = {}
    X_list = []
    y_list = []
    for line in sampled_lines:
        words = extract_arabic_words(line)
        W = len(words)
        if W == 0:
            continue
        sis = []
        for undiac, syms, diacs in words:
            si_w = analyze_surface(syms)
            if si_w["particle"] == PART_NONE and si_w["al_len"] == 0 and len(syms) >= 3 and undiac[1:] in vocab_set:
                if (syms[0] in (S_BEH, S_LAM) and diacs[0] == DIAC_KASRA) or (syms[0] == S_KAF and diacs[0] == DIAC_FATHA):
                    if diacs[-1] != DIAC_NONE:
                        si_w["has_prep"] = 1
            sis.append(si_w)
        cpos_list = []
        suf_list = []
        adj_list = []
        t_end_list = []
        t_v_list = []
        t_p_list = []
        t_mc_list = []
        for idx_w, (undiac, syms, diacs) in enumerate(words):
            si_w = sis[idx_w]
            cp, sf = resolve_case_pos(undiac, syms, diacs, vocab_set, si_w)
            cpos_list.append(cp)
            suf_list.append(1 if sf else 0)
            adj_list.append(_is_adj_surface_py(syms, si_w["al_len"]))
            t_end_list.append(word_top_case.get(undiac, -1))
            t_v_list.append(word_top_verb.get(undiac, 0))
            t_p_list.append(word_top_pos.get(undiac, 0))
            t_mc_list.append(word_top_mc.get(undiac, 11))

        # Precompute clause government & positional features identically to C++
        cg_arr = [0] * W
        dg_arr = [-1] * W
        pic_arr = [0] * W
        ric_arr = [0] * W
        gs = 0
        lg = -1
        for i in range(W):
            si_w = sis[i]
            if i == 0 or si_w["particle"] in (PART_REL, PART_COND):
                gs = 0
                lg = -1
            cg_arr[i] = gs
            dg_arr[i] = min(7, i - lg) if lg >= 0 else -1
            pic_arr[i] = min(15, i)
            ric_arr[i] = min(15, W - 1 - i)
            pid_w = si_w["particle"]
            top_mc = t_mc_list[i]
            tv = t_v_list[i]
            tp = t_p_list[i]
            tend = t_end_list[i]
            if pid_w == PART_INNA or (pid_w in (PART_AN, PART_LI_AN, PART_LAKIN) and top_mc == 9):
                gs = 4; lg = i
            elif pid_w == PART_KANA:
                gs = 5; lg = i
            elif pid_w == PART_GEN:
                gs = 6; lg = i
            elif tv == 1 or (si_w["verb_like"] == 2 and pid_w == PART_NONE):
                if tp == 4: gs = 3
                elif tp in (2, 5): gs = 2
                else: gs = 1
                lg = i
            elif gs == 6 and pid_w == PART_NONE and si_w["al_len"] == 0 and tend in (5, 6, 7, 12, 13, 14):
                gs = 0

        for i in range(W):
            undiac, syms, diacs = words[i]
            n = len(syms)
            if n == 0:
                continue
            cpos = cpos_list[i]
            p2_si = sis[i - 2] if i >= 2 else None
            p_si = sis[i - 1] if i >= 1 else None
            si = sis[i]
            nx_si = sis[i + 1] if i + 1 < W else None
            nx2_si = sis[i + 2] if i + 2 < W else None
            p_top_end = t_end_list[i - 1] if i >= 1 else -1
            t_top_end = t_end_list[i]
            nx_top_end = t_end_list[i + 1] if i + 1 < W else -1
            p_top_v = t_v_list[i - 1] if i >= 1 else -1
            t_top_v = t_v_list[i]
            nx_top_v = t_v_list[i + 1] if i + 1 < W else -1
            p_top_p = t_p_list[i - 1] if i >= 1 else -1
            t_top_p = t_p_list[i]
            nx_top_p = t_p_list[i + 1] if i + 1 < W else -1
            nx_syms = words[i + 1][1] if i + 1 < W else None
            feats = [
                p2_si["particle"] if p2_si else -1,
                p_si["particle"] if p_si else -1,
                si["particle"],
                nx_si["particle"] if nx_si else -1,
                nx2_si["particle"] if nx2_si else -1,
                p2_si["al_len"] if p2_si else -1,
                p_si["al_len"] if p_si else -1,
                si["al_len"],
                nx_si["al_len"] if nx_si else -1,
                nx2_si["al_len"] if nx2_si else -1,
                p2_si["has_prep"] if p2_si else -1,
                p_si["has_prep"] if p_si else -1,
                si["has_prep"],
                nx_si["has_prep"] if nx_si else -1,
                p_si["verb_like"] if p_si else -1,
                si["verb_like"],
                nx_si["verb_like"] if nx_si else -1,
                nx2_si["verb_like"] if nx2_si else -1,
                p_top_end,
                t_top_end,
                nx_top_end,
                p_top_v,
                t_top_v,
                nx_top_v,
                p_top_p,
                t_top_p,
                nx_top_p,
                n,
                syms[n - 1],
                syms[n - 2] if n >= 2 else -1,
                syms[min(si["al_len"], n - 1)],
                words[i - 1][1][-1] if i >= 1 else -1,
                nx_syms[-1] if nx_syms else -1,
                nx_syms[min(nx_si["al_len"], len(nx_syms) - 1)] if nx_syms else -1,
                1 if i == 0 else 0,
                _is_fem_at_py(syms, si["al_len"]),
                _is_diptote_surface_py(syms, si["al_len"]),
                _is_ibn_py(syms),
                _is_ibn_py(nx_syms) if nx_syms else -1,
                _is_fem_at_py(nx_syms, nx_si["al_len"]) if nx_syms else -1,
                _is_fem_at_py(words[i - 1][1], p_si["al_len"]) if i >= 1 else -1,
                1 if (i >= 1 and p_top_p == 3) else 0,
                suf_list[i - 1] if i >= 1 else -1,
                suf_list[i],
                cg_arr[i],
                dg_arr[i],
                pic_arr[i],
                ric_arr[i],
                adj_list[i],
                adj_list[i + 1] if i + 1 < W else -1,
                adj_list[i - 1] if i >= 1 else -1,
                1 if (nx_si and nx_si["al_len"] > 0 and nx_si["has_prep"] == 0) else 0,
                1 if (nx_si and (nx_si["verb_like"] == 2 or nx_top_v == 1)) else 0,
                1 if (p_si and p_si["al_len"] > 0 and p_si["has_prep"] == 0) else 0,
                1 if (not nx_si or nx_si["particle"] != PART_NONE or nx_si["has_prep"] != 0) else 0,
                t_p_list[i - 2] if i >= 2 else -1,
            ]
            X_list.append(feats)
            y_list.append(diacs[cpos])
            if len(X_list) >= max_samples:
                return np.array(X_list, dtype=np.int32), np.array(y_list, dtype=np.int32)
    return np.array(X_list, dtype=np.int32), np.array(y_list, dtype=np.int32)


def build_morph_dataset(
    sampled_lines: List[str],
    vocab_set: Set[str],
    word_top_case: Dict[str, int],
    word_top_verb: Dict[str, int],
    word_top_pos: Dict[str, int],
    ambig_words: Set[str],
    max_samples: int = 550000,
):
    """Builds 32-feature morphological/POS/particle disambiguation dataset aligned with C++ morph_trees_."""
    X_list = []
    y_list = []
    for line in sampled_lines:
        words = extract_arabic_words(line)
        W = len(words)
        if W == 0:
            continue
        sis = []
        for undiac, syms, diacs in words:
            si_w = analyze_surface(syms)
            if si_w["particle"] == PART_NONE and si_w["al_len"] == 0 and len(syms) >= 3 and undiac[1:] in vocab_set:
                if (syms[0] in (S_BEH, S_LAM) and diacs[0] == DIAC_KASRA) or (syms[0] == S_KAF and diacs[0] == DIAC_FATHA):
                    if diacs[-1] != DIAC_NONE:
                        si_w["has_prep"] = 1
            sis.append(si_w)
        for i in range(W):
            undiac, syms, diacs = words[i]
            n = len(syms)
            if n == 0:
                continue
            si = sis[i]
            if undiac not in ambig_words and si["particle"] not in (PART_AN, PART_IN, PART_LAKIN, PART_LAMA, PART_LI_AN, PART_AY, PART_GEN):
                continue
            cpos, _ = resolve_case_pos(undiac, syms, diacs, vocab_set, si)
            _, _, mc = _analyze_pos_mc_py(undiac, syms, diacs, cpos, si)
            p2_si = sis[i - 2] if i >= 2 else None
            p_si = sis[i - 1] if i >= 1 else None
            nx_si = sis[i + 1] if i + 1 < W else None
            nx2_si = sis[i + 2] if i + 2 < W else None
            nx_syms = words[i + 1][1] if i + 1 < W else None
            feats = [
                p2_si["particle"] if p2_si else -1,
                p_si["particle"] if p_si else -1,
                si["particle"],
                nx_si["particle"] if nx_si else -1,
                nx2_si["particle"] if nx2_si else -1,
                p_si["al_len"] if p_si else -1,
                si["al_len"],
                nx_si["al_len"] if nx_si else -1,
                nx2_si["al_len"] if nx2_si else -1,
                p_si["has_prep"] if p_si else -1,
                si["has_prep"],
                nx_si["has_prep"] if nx_si else -1,
                p_si["verb_like"] if p_si else -1,
                si["verb_like"],
                nx_si["verb_like"] if nx_si else -1,
                word_top_case.get(words[i - 1][0], -1) if i >= 1 else -1,
                word_top_case.get(undiac, -1),
                word_top_case.get(words[i + 1][0], -1) if i + 1 < W else -1,
                word_top_verb.get(words[i - 1][0], -1) if i >= 1 else -1,
                word_top_verb.get(undiac, 0),
                word_top_verb.get(words[i + 1][0], -1) if i + 1 < W else -1,
                word_top_pos.get(words[i - 1][0], -1) if i >= 1 else -1,
                word_top_pos.get(undiac, 0),
                word_top_pos.get(words[i + 1][0], -1) if i + 1 < W else -1,
                n,
                syms[0],
                syms[min(si["al_len"], n - 1)],
                syms[n - 2] if n >= 2 else -1,
                syms[n - 1],
                1 if i == 0 else 0,
                nx_syms[0] if nx_syms else -1,
                nx_syms[-1] if nx_syms else -1,
            ]
            X_list.append(feats)
            y_list.append(mc)
            if len(X_list) >= max_samples:
                return np.array(X_list, dtype=np.int32), np.array(y_list, dtype=np.int32)
    return np.array(X_list, dtype=np.int32), np.array(y_list, dtype=np.int32)


def train_tree_model(
    X: np.ndarray,
    y: np.ndarray,
    backend: str,
    num_trees: int,
    max_depth: int,
    lr: float,
    scale: float = 500.0,
    num_classes: int = NUM_DIACRITICS,
    max_leaves: int = 31,
) -> bytes:
    if num_trees <= 0 or len(X) < 50:
        return b""
    if backend == "lgb":
        import lightgbm as lgb

        dtrain = lgb.Dataset(X, label=y, free_raw_data=True)
        params = {
            "objective": "multiclass",
            "num_class": num_classes,
            "learning_rate": lr,
            "num_leaves": min(max_leaves, (1 << max_depth) - 1),
            "max_depth": max_depth,
            "min_data_in_leaf": 25,
            "feature_fraction": 0.9,
            "verbose": -1,
            "num_threads": os.cpu_count() or 4,
        }
        booster = lgb.train(params, dtrain, num_boost_round=num_trees)
        return export_lightgbm_ensemble(booster, num_classes, scale=scale)
    else:
        from sklearn.ensemble import RandomForestClassifier

        rf = RandomForestClassifier(
            n_estimators=max(1, num_trees),
            max_depth=max_depth,
            min_samples_leaf=5,
            n_jobs=-1,
            random_state=42,
        )
        rf.fit(X, y)
        return export_sklearn_rf(rf, num_classes, scale=scale)


def _get_file_chunks(path: str, n_chunks: int) -> List[Tuple[int, int]]:
    fsize = os.path.getsize(path)
    if fsize == 0 or n_chunks <= 1:
        return [(0, fsize)]
    step = fsize // n_chunks
    bounds = [0]
    with open(path, "rb") as f:
        for i in range(1, n_chunks):
            f.seek(i * step)
            f.readline()
            bounds.append(f.tell())
    bounds.append(fsize)
    return [(bounds[i], bounds[i + 1]) for i in range(n_chunks) if bounds[i] < bounds[i + 1]]


def _pass1_worker(task: Tuple[str, int, int, int, int]):
    path, start, end, max_sample, seed = task
    wt = Counter()
    wc: Dict[str, Counter] = defaultdict(Counter)
    wsyms: Dict[str, bytes] = {}
    sampled: List[str] = []
    rng = random.Random(seed)
    lc = 0
    with open(path, "rb") as f:
        f.seek(start)
        while f.tell() < end:
            raw = f.readline()
            if not raw:
                break
            line = raw.decode("utf-8", errors="ignore").strip()
            if not line:
                continue
            lc += 1
            if len(sampled) < max_sample:
                sampled.append(line)
            else:
                j = rng.randint(0, lc - 1)
                if j < max_sample:
                    sampled[j] = line
            for undiac, syms, diacs in extract_arabic_words(line):
                if len(syms) > 48:
                    continue
                wt[undiac] += 1
                if undiac not in wsyms:
                    wsyms[undiac] = syms
                wc[undiac][(bytes(diacs[:-1]), diacs[-1])] += 1
    return wt, dict(wc), wsyms, sampled, lc


_P2_STATE = {}


def _pass2_worker(task: Tuple[str, int, int]):
    path, start, end = task
    vocab_set = _P2_STATE["vocab_set"]
    word_to_id = _P2_STATE["word_to_id"]
    fmt = _P2_STATE["format"]
    num_roles = _P2_STATE["num_roles"]
    max_bg_track = _P2_STATE["max_bg_track"]

    tc = np.zeros((num_roles, NUM_DIACRITICS, num_roles, NUM_DIACRITICS), dtype=np.int64)
    bg: Dict[Tuple[int, int], List[int]] = {}
    si_cache: Dict[bytes, dict] = {}
    p2_cache: Dict[Tuple[str, bytes], Tuple[int, int]] = {}

    with open(path, "rb") as f:
        f.seek(start)
        while f.tell() < end:
            raw = f.readline()
            if not raw:
                break
            line = raw.decode("utf-8", errors="ignore").strip()
            if not line:
                continue
            words = extract_arabic_words(line)
            prev_wid = None
            prev_role = 0
            prev_end = DIAC_NONE
            for undiac, syms, diacs in words:
                if len(syms) > 48:
                    prev_wid = None
                    prev_role = 0
                    prev_end = DIAC_NONE
                    continue
                diacs_b = bytes(diacs)
                ckey = (undiac, diacs_b)
                cached = p2_cache.get(ckey)
                if cached is not None:
                    c_end, role = cached
                else:
                    si = si_cache.get(syms)
                    if si is None:
                        si = analyze_surface(syms)
                        if len(si_cache) < 400000:
                            si_cache[syms] = si
                    cpos, _ = resolve_case_pos(undiac, syms, diacs, vocab_set, si)
                    c_end = diacs[cpos]
                    if fmt == "v5":
                        _, tp, mc = _analyze_pos_mc_py(undiac, syms, diacs, cpos, si)
                        role = _cand_role_v5_py(si["particle"], tp, mc)
                    else:
                        from arabic_utils import morph_to_role
                        role = morph_to_role(infer_word_flags(undiac, syms))
                    if len(p2_cache) < 800000:
                        p2_cache[ckey] = (c_end, role)

                tc[prev_role, prev_end, role, c_end] += 1
                prev_role = role
                prev_end = c_end

                wid = word_to_id.get(undiac)
                if prev_wid is not None and wid is not None:
                    pair = (prev_wid, wid)
                    arr = bg.get(pair)
                    if arr is None and len(bg) < max_bg_track:
                        arr = [0] * NUM_DIACRITICS
                        bg[pair] = arr
                    if arr is not None:
                        arr[c_end] += 1
                prev_wid = wid
    return tc, bg


def main():
    parser = argparse.ArgumentParser(description="Train FST + statistical + grammar + tree Arabic diacritizer (.tash)")
    parser.add_argument("--corpus", required=True, help="Path to diacritized Arabic corpus (one sentence per line)")
    parser.add_argument("--output", required=True, help="Output .tash binary model path")
    parser.add_argument("--max-vocab", type=int, default=350000, help="Max words in FST lexicon")
    parser.add_argument("--min-freq", type=int, default=2, help="Min word frequency for FST lexicon")
    parser.add_argument("--max-cands", type=int, default=6, help="Max candidates stored per word")
    parser.add_argument("--max-bigrams", type=int, default=550000, help="Max syntactic word bigrams")
    parser.add_argument("--min-bigram-freq", type=int, default=2, help="Min frequency for word bigrams")
    parser.add_argument("--min-bigram-lift", type=int, default=128, help="Min absolute bigram lift to keep")
    parser.add_argument("--max-tree-lines", type=int, default=300000, help="Reservoir sample lines for tree training")
    parser.add_argument("--max-tree-samples", type=int, default=650000, help="Max samples for each tree model")
    parser.add_argument("--tree-backend", choices=["lgb", "rf", "none"], default="lgb", help="Tree model backend")
    parser.add_argument("--oov-trees", type=int, default=24, help="Boosting rounds / trees for OOV model")
    parser.add_argument("--irab-trees", type=int, default=24, help="Boosting rounds / trees for Irab model")
    parser.add_argument("--morph-trees", type=int, default=20, help="Boosting rounds / trees for Morph/POS model")
    parser.add_argument("--max-depth", type=int, default=6, help="Max depth of trees")
    parser.add_argument("--prune-proclitic-freq", type=int, default=3, help="Prune redundant prefixed words <= freq if stem reproduces top form")
    parser.add_argument("--format", choices=["v5", "v4", "v2"], default="v5", help="Output model format")
    args = parser.parse_args()

    t_start = time.time()
    n_workers = min(4, os.cpu_count() or 1)
    chunks = _get_file_chunks(args.corpus, n_workers)
    print(f"[1/5] Parallel pass 1 over {args.corpus} ({len(chunks)} workers) ...")

    word_total = Counter()
    word_cands: Dict[str, Counter] = defaultdict(Counter)
    word_syms_map: Dict[str, bytes] = {}
    sampled_lines: List[str] = []
    line_count = 0

    if len(chunks) > 1:
        per_sample = max(1000, args.max_tree_lines // len(chunks) + 1)
        tasks = [(args.corpus, s, e, per_sample, 42 + i) for i, (s, e) in enumerate(chunks)]
        ctx = mp.get_context("fork")
        with ctx.Pool(len(chunks)) as pool:
            for wt, wc, wsyms, samp, lc in pool.imap_unordered(_pass1_worker, tasks):
                line_count += lc
                word_total.update(wt)
                for k, v in wsyms.items():
                    if k not in word_syms_map:
                        word_syms_map[k] = v
                for k, cnts in wc.items():
                    word_cands[k].update(cnts)
                sampled_lines.extend(samp)
        if len(sampled_lines) > args.max_tree_lines:
            random.Random(42).shuffle(sampled_lines)
            sampled_lines = sampled_lines[: args.max_tree_lines]
    else:
        wt, wc, wsyms, sampled_lines, line_count = _pass1_worker((args.corpus, chunks[0][0], chunks[0][1], args.max_tree_lines, 42))
        word_total = wt
        word_cands.update(wc)
        word_syms_map = wsyms

    print(f"  Total lines: {line_count:,} | Raw unique words: {len(word_total):,} ({time.time()-t_start:.1f}s)")

    print("[2/5] Filtering vocabulary & building Bitmapped FST ...")
    raw_kept = [
        w for w, cnt in word_total.most_common(args.max_vocab) if cnt >= args.min_freq
    ]
    raw_kept_set = set(raw_kept)
    PROCLITIC_VALS = {"و": DIAC_FATHA, "ف": DIAC_FATHA, "ب": DIAC_KASRA, "ك": DIAC_FATHA, "ل": DIAC_KASRA}
    top_cand_cache = {w: word_cands[w].most_common(1)[0][0] for w in raw_kept}
    kept_words = []
    pruned_redundant = 0
    for w in raw_kept:
        if args.prune_proclitic_freq > 0 and word_total[w] <= args.prune_proclitic_freq and len(w) >= 5 and w[0] in PROCLITIC_VALS:
            stem = w[1:]
            if stem in raw_kept_set and len(word_cands[w]) == 1:
                w_core, w_end = top_cand_cache[w]
                s_core, s_end = top_cand_cache[stem]
                if w_end == s_end and w_core == bytes([PROCLITIC_VALS[w[0]]]) + s_core:
                    si_w = analyze_surface(word_syms_map[w])
                    if si_w["particle"] == PART_NONE:
                        pruned_redundant += 1
                        continue
        kept_words.append(w)
    print(f"  Pruned {pruned_redundant:,} redundant low-freq proclitic surface words; kept {len(kept_words):,}")

    words_syms = [word_syms_map[w] for w in kept_words]
    if args.format == "v5":
        fst_bytes, bfs_order = build_bitmapped_fst_v5(words_syms)
        kept_words = [kept_words[idx] for idx in bfs_order]
        words_syms = [word_syms_map[w] for w in kept_words]
    else:
        fst_bytes = build_bitmapped_fst(words_syms)

    word_to_id: Dict[str, int] = {w: i for i, w in enumerate(kept_words)}
    vocab_set: Set[str] = set(kept_words)

    core_pool = bytearray()
    core_dedup: Dict[bytes, int] = {}
    meta_list: List[Tuple[int, int, int, int]] = []
    cand_records: List[Tuple[int, int, int, int]] = []
    word_top_case: Dict[str, int] = {}
    word_top_verb: Dict[str, int] = {}
    word_top_pos: Dict[str, int] = {}
    word_top_mc: Dict[str, int] = {}
    ambig_words: Set[str] = set()
    word_case_logp: Dict[int, np.ndarray] = {}

    for wid, w in enumerate(kept_words):
        cands_counter = word_cands[w]
        tot = float(sum(cands_counter.values()))
        raw_top = cands_counter.most_common(args.max_cands)
        if tot >= 40.0:
            filtered = [(k, cnt) for k, cnt in raw_top if cnt >= 2 and (cnt / tot) >= 0.004]
            top_cands = filtered if filtered else raw_top[:1]
        elif tot >= 15.0:
            filtered = [(k, cnt) for k, cnt in raw_top if cnt >= 2]
            top_cands = filtered if filtered else raw_top[:1]
        else:
            top_cands = raw_top

        syms = word_syms_map[w]
        si = analyze_surface(syms)
        case_probs = np.full(NUM_DIACRITICS, 1e-4, dtype=np.float64)
        top_case_set = False
        mcs_seen = set()

        cand_start = len(cand_records)
        for (core_b, end_id), cnt in top_cands:
            diacs_full = list(core_b) + [end_id]
            cpos, _ = resolve_case_pos(w, syms, diacs_full, vocab_set, si)
            c_end = diacs_full[cpos]
            tv, tp, mc = _analyze_pos_mc_py(w, syms, diacs_full, cpos, si)
            mcs_seen.add(mc)
            if not top_case_set:
                word_top_case[w] = c_end
                word_top_verb[w] = tv
                word_top_pos[w] = tp
                word_top_mc[w] = mc
                top_case_set = True
            case_probs[c_end] += cnt

            if len(core_b) == 0:
                c_off = 0
            else:
                c_off = core_dedup.get(core_b)
                if c_off is None:
                    c_off = len(core_pool)
                    core_dedup[core_b] = c_off
                    core_pool.extend(core_b)
            log_p = int(round(1000.0 * math.log(max(cnt / tot, 1e-6))))
            log_p = max(-30000, min(0, log_p))
            cand_records.append((c_off, log_p, end_id, 0))
        if len(mcs_seen) >= 2:
            ambig_words.add(w)

        for (core_b, end_id), cnt in cands_counter.items():
            diacs_full = list(core_b) + [end_id]
            cpos, _ = resolve_case_pos(w, syms, diacs_full, vocab_set, si)
            case_probs[diacs_full[cpos]] += cnt * 0.01
        case_probs /= case_probs.sum()
        word_case_logp[wid] = np.log(case_probs)

        flags = infer_word_flags(w, syms)
        meta_list.append((cand_start, len(top_cands), len(syms), flags))

    print(f"  Vocab: {len(kept_words):,} | FST size: {len(fst_bytes)/1e6:.2f} MB | Cands: {len(cand_records):,}")

    del word_cands, word_total

    print(f"[3/5] Parallel pass 2 ({len(chunks)} workers) for 24-role transitions & syntactic bigrams ...")
    num_roles = 24 if args.format == "v5" else 16
    trans_counts = np.zeros((num_roles, NUM_DIACRITICS, num_roles, NUM_DIACRITICS), dtype=np.int64)
    bigram_counts: Dict[Tuple[int, int], List[int]] = {}
    bigram_totals: Counter = Counter()

    _P2_STATE["vocab_set"] = vocab_set
    _P2_STATE["word_to_id"] = word_to_id
    _P2_STATE["format"] = args.format
    _P2_STATE["num_roles"] = num_roles
    _P2_STATE["max_bg_track"] = max(200000, (args.max_bigrams * 3) // max(1, len(chunks)))

    if len(chunks) > 1:
        ctx = mp.get_context("fork")
        with ctx.Pool(len(chunks)) as pool:
            for tc, bg in pool.imap_unordered(_pass2_worker, [(args.corpus, s, e) for s, e in chunks]):
                trans_counts += tc
                for pair, arr in bg.items():
                    cur = bigram_counts.get(pair)
                    if cur is None:
                        bigram_counts[pair] = arr
                        bigram_totals[pair] = sum(arr)
                    else:
                        for d in range(NUM_DIACRITICS):
                            cur[d] += arr[d]
                        bigram_totals[pair] += sum(arr)
    else:
        tc, bg = _pass2_worker((args.corpus, chunks[0][0], chunks[0][1]))
        trans_counts = tc
        bigram_counts = bg
        for pair, arr in bg.items():
            bigram_totals[pair] = sum(arr)
    _P2_STATE.clear()
    print(f"  pass 2 done ({time.time()-t_start:.1f}s) | tracked pairs: {len(bigram_counts):,}")

    top_pairs = [
        p for p, tot in bigram_totals.most_common(args.max_bigrams) if tot >= args.min_bigram_freq
    ]
    bigrams_v5: Dict[Tuple[int, int], List[int]] = {}
    bigrams_compiled: Dict[Tuple[int, int], Tuple[List[int], int]] = {}
    for pair in top_pairs:
        arr = bigram_counts[pair]
        tot = float(sum(arr))
        _, curr_wid = pair
        base_logp = word_case_logp[curr_wid]
        bonuses = []
        for d in range(NUM_DIACRITICS):
            p_bg = (arr[d] + 0.5) / (tot + 0.5 * NUM_DIACRITICS)
            lift = (math.log(p_bg) - float(base_logp[d])) * 800.0
            bonuses.append(int(round(max(-4000.0, min(4000.0, lift)))))
        bigrams_v5[pair] = bonuses
        if args.format != "v5":
            bigrams_compiled[pair] = (bonuses, 0)

    print(f"  Kept bigrams: {len(top_pairs):,}")
    trans_bytes = compute_trans_matrix(trans_counts)
    trans_size = len(trans_bytes) // 2

    oov_bytes = b""
    irab_bytes = b""
    morph_bytes = b""
    if args.tree_backend != "none":
        print(f"[4/5] Training OOV, Irab & Morph tree models ({args.tree_backend}) on {len(sampled_lines):,} sampled lines ...")
        if args.oov_trees > 0:
            X_oov, y_oov = build_oov_dataset(sampled_lines, max_samples=min(450000, args.max_tree_samples))
            print(f"  OOV dataset: {X_oov.shape}")
            oov_bytes = train_tree_model(X_oov, y_oov, args.tree_backend, args.oov_trees, args.max_depth, 0.14, scale=500.0, num_classes=NUM_DIACRITICS, max_leaves=31)
        if args.irab_trees > 0:
            X_irab, y_irab = build_irab_dataset(sampled_lines, vocab_set, word_top_case, word_top_verb, word_top_pos, max_samples=args.max_tree_samples, word_top_mc=word_top_mc)
            print(f"  Irab dataset: {X_irab.shape}")
            irab_bytes = train_tree_model(X_irab, y_irab, args.tree_backend, args.irab_trees, args.max_depth, 0.12, scale=450.0, num_classes=NUM_DIACRITICS, max_leaves=40)
        if args.morph_trees > 0:
            X_morph, y_morph = build_morph_dataset(sampled_lines, vocab_set, word_top_case, word_top_verb, word_top_pos, ambig_words, max_samples=min(550000, args.max_tree_samples))
            print(f"  Morph dataset: {X_morph.shape} (ambig vocab: {len(ambig_words):,})")
            morph_bytes = train_tree_model(X_morph, y_morph, args.tree_backend, args.morph_trees, args.max_depth, 0.12, scale=500.0, num_classes=12, max_leaves=31)
    else:
        print("[4/5] Skipping tree models (--tree-backend none).")

    print(f"[5/5] Writing {args.format} binary model to {args.output} ...")
    if args.format == "v5":
        total_bytes = write_v5_model(
            dst_path=args.output,
            fst_blob=fst_bytes,
            word_meta_list=meta_list,
            cand_records=cand_records,
            core_pool_bytes=bytes(core_pool),
            bigrams=bigrams_v5,
            trans_bytes=trans_bytes,
            trans_size=trans_size,
            oov_blob=oov_bytes,
            irab_blob=irab_bytes,
            morph_blob=morph_bytes,
            weights=list(DEFAULT_WEIGHTS),
            quantize_bigrams=True,
            min_bigram_lift=args.min_bigram_lift,
            has_24_roles=True,
            irab_56f=True,
        )
    else:
        meta_buf = bytearray(len(meta_list) * WORD_META_STRUCT.size)
        for wid, rec in enumerate(meta_list):
            WORD_META_STRUCT.pack_into(meta_buf, wid * WORD_META_STRUCT.size, *rec)
        cand_buf = bytearray(len(cand_records) * WORD_CAND_STRUCT.size)
        for idx, rec in enumerate(cand_records):
            WORD_CAND_STRUCT.pack_into(cand_buf, idx * WORD_CAND_STRUCT.size, *rec)
        bigram_bytes, bigram_cap = build_bigram_table(bigrams_compiled)
        tmp_v2 = args.output if args.format == "v2" else (args.output + ".v2tmp")
        total_bytes = write_tash_model(
            output_path=tmp_v2, fst_bytes=fst_bytes, word_meta_bytes=bytes(meta_buf),
            word_meta_count=len(kept_words), candidates_bytes=bytes(cand_buf),
            candidates_count=len(cand_records), core_pool_bytes=bytes(core_pool),
            bigram_bytes=bigram_bytes, bigram_capacity=bigram_cap,
            trans_matrix_bytes=trans_bytes, trans_matrix_size=trans_size,
            oov_trees_bytes=oov_bytes, irab_trees_bytes=irab_bytes, weights=DEFAULT_WEIGHTS,
        )
        if args.format == "v4":
            convert_v2_to_v3(tmp_v2, args.output, morph_blob=morph_bytes, use_v4=True, quantize_bigrams=True, min_bigram_lift=args.min_bigram_lift)
            os.remove(tmp_v2)
            total_bytes = os.path.getsize(args.output)

    elapsed = time.time() - t_start
    print(f"Done in {elapsed:.2f}s! Final model size: {total_bytes / 1e6:.2f} MB ({total_bytes:,} bytes)")


if __name__ == "__main__":
    main()
