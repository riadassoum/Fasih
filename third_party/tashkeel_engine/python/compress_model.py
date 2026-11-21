import argparse
import os
import struct
import sys
from typing import Dict, List, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from arabic_utils import (
    DIAC_DAMMA,
    DIAC_DAMMATAN,
    DIAC_FATHA,
    DIAC_FATHATAN,
    DIAC_KASRA,
    DIAC_KASRATAN,
    DIAC_SHADDA_DAMMA,
    DIAC_SHADDA_DAMMATAN,
    DIAC_SHADDA_FATHA,
    DIAC_SHADDA_FATHATAN,
    DIAC_SHADDA_KASRA,
    DIAC_SHADDA_KASRATAN,
    DIAC_SUKUN,
    NUM_DIACRITICS,
)
from fst_builder import (
    BG_BLOCK_SHIFT_V5,
    BIGRAM_ENTRY_STRUCT,
    BIGRAM_ENTRY_V3_STRUCT,
    BIGRAM_ENTRY_V5_STRUCT,
    BIT_TRIE_NODE_STRUCT,
    BIT_TRIE_NODE_V3_STRUCT,
    FLAT_TREE_NODE_STRUCT,
    FLAT_TREE_NODE_V3_STRUCT,
    HEADER_STRUCT,
    HEADER_V3_STRUCT,
    HEADER_V4_STRUCT,
    HEADER_V5_STRUCT,
    NODE_CAT,
    TASH_MAGIC,
    TASH_VERSION_3,
    TASH_VERSION_4,
    TASH_VERSION_5,
    TREE_HDR_STRUCT,
    UINT32_MAX,
    UINT64_MAX,
    WORD_CAND_STRUCT,
    WORD_CAND_V3_STRUCT,
    WORD_CAND_V5_STRUCT,
    WORD_META_STRUCT,
    WORD_META_V3_STRUCT,
    convert_tree_v2_to_v5,
)


def pack_u24(x: int) -> bytes:
    if x < 0 or x >= (1 << 24):
        raise ValueError(f"Value {x} out of 24-bit range")
    return bytes((x & 0xFF, (x >> 8) & 0xFF, (x >> 16) & 0xFF))


def reachable_ending_mask(cands: List[Tuple[int, int, int, int]], wlen: int, core_seqs: List[bytes]) -> int:
    """Computes exact superset of case_pos diacritic IDs reachable via emit_stem + expand_cases."""
    mask = 0
    for idx, (_, _, eid, _) in enumerate(cands):
        mask |= 1 << eid
        seq = core_seqs[idx]
        if wlen >= 4 and len(seq) == wlen - 1:
            for pos in range(max(1, wlen - 4), wlen - 1):
                d = seq[pos]
                mask |= 1 << d
                if d in (DIAC_DAMMA, DIAC_FATHA, DIAC_KASRA):
                    mask |= (1 << DIAC_DAMMA) | (1 << DIAC_FATHA) | (1 << DIAC_KASRA)
                elif d in (DIAC_SHADDA_DAMMA, DIAC_SHADDA_FATHA, DIAC_SHADDA_KASRA):
                    mask |= (1 << DIAC_SHADDA_DAMMA) | (1 << DIAC_SHADDA_FATHA) | (1 << DIAC_SHADDA_KASRA)
        if eid in (DIAC_DAMMA, DIAC_FATHA, DIAC_KASRA):
            mask |= (1 << DIAC_DAMMA) | (1 << DIAC_FATHA) | (1 << DIAC_KASRA)
        elif eid in (DIAC_DAMMATAN, DIAC_FATHATAN, DIAC_KASRATAN):
            mask |= (
                (1 << DIAC_DAMMA) | (1 << DIAC_FATHA) | (1 << DIAC_KASRA) |
                (1 << DIAC_DAMMATAN) | (1 << DIAC_FATHATAN) | (1 << DIAC_KASRATAN)
            )
        elif eid in (DIAC_SHADDA_DAMMA, DIAC_SHADDA_FATHA, DIAC_SHADDA_KASRA):
            mask |= (1 << DIAC_SHADDA_DAMMA) | (1 << DIAC_SHADDA_FATHA) | (1 << DIAC_SHADDA_KASRA)
        elif eid in (DIAC_SHADDA_DAMMATAN, DIAC_SHADDA_FATHATAN, DIAC_SHADDA_KASRATAN):
            mask |= (
                (1 << DIAC_SHADDA_DAMMA) | (1 << DIAC_SHADDA_FATHA) | (1 << DIAC_SHADDA_KASRA) |
                (1 << DIAC_SHADDA_DAMMATAN) | (1 << DIAC_SHADDA_FATHATAN) | (1 << DIAC_SHADDA_KASRATAN)
            )
        elif eid == DIAC_SUKUN:
            mask |= (1 << DIAC_KASRA) | (1 << DIAC_FATHA)
        elif eid == 0:
            mask |= (1 << DIAC_FATHA)
    return mask


def convert_tree_blob(blob: bytes) -> Tuple[bytes, bool]:
    if not blob:
        return b"", False
    nt, nc, nn, bsq = TREE_HDR_STRUCT.unpack_from(blob, 0)
    if nn >= 65535:
        return blob, False
    roots_off = TREE_HDR_STRUCT.size
    nodes_off = roots_off + nt * 4
    for i in range(nn):
        _, _, _, _, _, kind, _ = FLAT_TREE_NODE_STRUCT.unpack_from(blob, nodes_off + i * FLAT_TREE_NODE_STRUCT.size)
        if kind == NODE_CAT:
            return blob, False

    out = bytearray(TREE_HDR_STRUCT.size + nt * 4 + nn * FLAT_TREE_NODE_V3_STRUCT.size)
    out[:nodes_off] = blob[:nodes_off]
    for i in range(nn):
        _, th, l, r, feat, kind, cid = FLAT_TREE_NODE_STRUCT.unpack_from(blob, nodes_off + i * FLAT_TREE_NODE_STRUCT.size)
        if feat >= 256:
            return blob, False
        FLAT_TREE_NODE_V3_STRUCT.pack_into(
            out,
            nodes_off + i * FLAT_TREE_NODE_V3_STRUCT.size,
            th, l & 0xFFFF, r & 0xFFFF, feat & 0xFF, ((kind & 0x0F) << 4) | (cid & 0x0F),
        )
    return bytes(out), True


def write_v5_model(
    dst_path: str,
    fst_blob: bytes,
    word_meta_list: List[Tuple[int, int, int, int]],
    cand_records: List[Tuple[int, int, int, int]],
    core_pool_bytes: bytes,
    bigrams: Dict[Tuple[int, int], List[int]],
    trans_bytes: bytes,
    trans_size: int,
    oov_blob: bytes,
    irab_blob: bytes,
    morph_blob: bytes,
    weights: List[int],
    quantize_bigrams: bool = True,
    min_bigram_lift: int = 128,
    has_24_roles: bool = True,
    irab_56f: bool = False,
) -> int:
    meta_cnt = len(word_meta_list)
    cand_cnt = len(cand_records)

    # 1. Pack core_pool into 4-bit nibbles (deduplicated)
    nib_bytes = bytearray()
    nib_count = 0
    core_map: Dict[Tuple[int, int], int] = {}

    def alloc_core(old_off: int, core_len: int) -> int:
        nonlocal nib_count
        if core_len == 0:
            return 0
        key = (old_off, core_len)
        existing = core_map.get(key)
        if existing is not None:
            return existing
        seq = core_pool_bytes[old_off : old_off + core_len]
        start_nib = nib_count
        for d in seq:
            if (nib_count & 1) == 0:
                nib_bytes.append(d & 0x0F)
            else:
                nib_bytes[-1] |= (d & 0x0F) << 4
            nib_count += 1
        core_map[key] = start_nib
        return start_nib

    cand_v5_ok = True
    meta_v3 = bytearray(meta_cnt * WORD_META_V3_STRUCT.size)
    cands_v5 = bytearray(cand_cnt * WORD_CAND_V5_STRUCT.size)
    cands_v3 = bytearray(cand_cnt * WORD_CAND_V3_STRUCT.size)
    end_masks = [0] * meta_cnt

    for wid in range(meta_cnt):
        c_start, c_count, wlen, rflags = word_meta_list[wid]
        raw_cands = []
        core_seqs = []
        core_len = max(0, wlen - 1)
        for k in range(c_count):
            c_idx = c_start + k
            co, lp, eid, fl = cand_records[c_idx]
            raw_cands.append((co, lp, eid, fl))
            core_seqs.append(core_pool_bytes[co : co + core_len] if core_len > 0 else b"")
            new_co = alloc_core(co, core_len)
            WORD_CAND_V3_STRUCT.pack_into(
                cands_v3, c_idx * WORD_CAND_V3_STRUCT.size,
                pack_u24(new_co), lp, ((fl & 0x0F) << 4) | (eid & 0x0F),
            )
            if new_co >= (1 << 20):
                cand_v5_ok = False
            if cand_v5_ok:
                lp_q = min(4095, max(0, int(round(-lp / 3.0))))
                packed = (new_co & 0xFFFFF) | ((eid & 0xF) << 20) | ((fl & 0xF) << 24) | ((lp_q & 0xFFF) << 28)
                cands_v5[c_idx * 5 : (c_idx + 1) * 5] = packed.to_bytes(5, "little")
        emask = reachable_ending_mask(raw_cands[:10], wlen, core_seqs[:10])
        end_masks[wid] = emask
        WORD_META_V3_STRUCT.pack_into(
            meta_v3, wid * WORD_META_V3_STRUCT.size,
            pack_u24(c_start), min(c_count, 255), wlen, rflags, emask,
        )

    cands_bytes = bytes(cands_v5) if cand_v5_ok else bytes(cands_v3)

    # 2. Pack Bigrams into Block-Indexed CSR (5B/entry + int8 vals)
    rows: List[List[Tuple[int, List[int]]]] = [[] for _ in range(meta_cnt)]
    for (prev_id, curr_id), bonuses in bigrams.items():
        if prev_id < meta_cnt and curr_id < meta_cnt:
            rows[prev_id].append((curr_id, bonuses))

    bg_row_off = bytearray((meta_cnt + 1) * 4)
    bg_block_base = bytearray()
    bg_entries = bytearray()
    bg_vals = bytearray()
    val_count = 0
    entry_count = 0
    cur_block_base = 0
    block_sz = 1 << BG_BLOCK_SHIFT_V5

    for prev_id in range(meta_cnt):
        struct.pack_into("<I", bg_row_off, prev_id * 4, entry_count)
        row = rows[prev_id]
        if not row:
            continue
        row.sort(key=lambda x: x[0])
        for curr_id, bonuses in row:
            emask = end_masks[curr_id]
            if min_bigram_lift > 0:
                max_abs = max((abs(bonuses[ce]) for ce in range(NUM_DIACRITICS) if (emask >> ce) & 1), default=0)
                if max_abs < min_bigram_lift:
                    continue
            if (entry_count & (block_sz - 1)) == 0:
                cur_block_base = val_count
                bg_block_base.extend(struct.pack("<I", cur_block_base))
            local_off = val_count - cur_block_base
            if local_off >= 65536:
                raise ValueError("Bigram block local offset overflow")
            bg_entries.extend(BIGRAM_ENTRY_V5_STRUCT.pack(pack_u24(curr_id), local_off))
            entry_count += 1
            for ce in range(NUM_DIACRITICS):
                if (emask >> ce) & 1:
                    if quantize_bigrams:
                        q = max(-127, min(127, int(round(bonuses[ce] / 32.0))))
                        bg_vals.extend(struct.pack("<b", q))
                    else:
                        bg_vals.extend(struct.pack("<h", max(-32000, min(32000, int(bonuses[ce])))))
                    val_count += 1
    struct.pack_into("<I", bg_row_off, meta_cnt * 4, entry_count)
    block_cnt = len(bg_block_base) // 4

    # 3. Pack trees to 4-byte format
    oov_v5, oov_ok = convert_tree_v2_to_v5(oov_blob)
    irab_v5, irab_ok = convert_tree_v2_to_v5(irab_blob)
    morph_v5, morph_ok = convert_tree_v2_to_v5(morph_blob)
    all_trees_v5 = (not oov_blob or oov_ok) and (not irab_blob or irab_ok) and (not morph_blob or morph_ok)
    if not all_trees_v5:
        oov_v5, _ = convert_tree_blob(oov_blob)
        irab_v5, _ = convert_tree_blob(irab_blob)
        morph_v5, _ = convert_tree_blob(morph_blob)

    flags_v5 = (
        (1 if all_trees_v5 else 0)
        | (2 if cand_v5_ok else 0)
        | (4 if quantize_bigrams else 0)
        | (8 if has_24_roles else 0)
        | (16 if irab_56f else 0)
    )
    weights_bytes = struct.pack(f"<{len(weights)}i", *[int(w) for w in weights])

    def align8(x: int) -> int:
        return (x + 7) & ~7

    off = align8(HEADER_V5_STRUCT.size)
    o_fst = off; off = align8(off + len(fst_blob))
    o_meta = off; off = align8(off + len(meta_v3))
    o_cand = off; off = align8(off + len(cands_bytes))
    o_pool = off; off = align8(off + len(nib_bytes))
    o_bg_row = off; off = align8(off + len(bg_row_off))
    o_bg_blk = off; off = align8(off + len(bg_block_base))
    o_bg_ent = off; off = align8(off + len(bg_entries))
    o_bg_val = off; off = align8(off + len(bg_vals))
    o_trans = off; off = align8(off + len(trans_bytes))
    o_oov = off; off = align8(off + len(oov_v5))
    o_irab = off; off = align8(off + len(irab_v5))
    o_morph = off; off = align8(off + len(morph_v5))
    o_weights = off
    total_size = align8(off + len(weights_bytes))

    hdr_bytes = HEADER_V5_STRUCT.pack(
        TASH_MAGIC, TASH_VERSION_5, flags_v5,
        o_fst, len(fst_blob),
        o_meta, meta_cnt,
        o_cand, cand_cnt,
        o_pool, len(nib_bytes),
        o_bg_row, meta_cnt + 1,
        o_bg_blk, block_cnt,
        o_bg_ent, entry_count,
        o_bg_val, val_count,
        o_trans, trans_size,
        o_oov, len(oov_v5),
        o_irab, len(irab_v5),
        o_morph, len(morph_v5),
        o_weights, len(weights),
    )

    with open(dst_path, "wb") as f:
        f.write(b"\x00" * total_size)
        for sec_off, blob in [
            (0, hdr_bytes), (o_fst, fst_blob), (o_meta, meta_v3), (o_cand, cands_bytes),
            (o_pool, nib_bytes), (o_bg_row, bg_row_off), (o_bg_blk, bg_block_base),
            (o_bg_ent, bg_entries), (o_bg_val, bg_vals), (o_trans, trans_bytes),
            (o_oov, oov_v5), (o_irab, irab_v5), (o_morph, morph_v5), (o_weights, weights_bytes),
        ]:
            f.seek(sec_off)
            f.write(blob)

    print(f"Wrote model ({total_size/1e6:.2f} MB):")
    print(f"  FST: {len(fst_blob)/1e6:.2f} MB | Meta: {len(meta_v3)/1e6:.2f} MB | Cands: {len(cands_bytes)/1e6:.2f} MB | Core: {len(nib_bytes)/1e6:.2f} MB")
    print(f"  Bigrams CSR5: {(len(bg_row_off)+len(bg_block_base)+len(bg_entries)+len(bg_vals))/1e6:.2f} MB ({entry_count:,} pairs) | Trans: {len(trans_bytes)/1e6:.2f} MB")
    print(f"  Trees (4B/node={all_trees_v5}): OOV={len(oov_v5)/1e6:.2f} MB | Irab={len(irab_v5)/1e6:.2f} MB | Morph={len(morph_v5)/1e6:.2f} MB")
    return total_size


def convert_v2_to_v3(
    src_path: str,
    dst_path: str,
    morph_blob: bytes = b"",
    use_v4: bool = False,
    quantize_bigrams: bool = False,
    min_bigram_lift: int = 0,
) -> None:
    with open(src_path, "rb") as f:
        data = f.read()

    hdr = HEADER_STRUCT.unpack(data[:HEADER_STRUCT.size])
    magic, version, _ = hdr[0], hdr[1], hdr[2]
    if magic != TASH_MAGIC or version != 2:
        raise ValueError(f"Expected v2 .tash file, got magic={hex(magic)} version={version}")

    fst_off, fst_cnt = hdr[3], hdr[4]
    meta_off, meta_cnt = hdr[5], hdr[6]
    cand_off, cand_cnt = hdr[7], hdr[8]
    pool_off, pool_sz = hdr[9], hdr[10]
    bg_off, bg_cap = hdr[11], hdr[12]
    trans_off, trans_sz = hdr[13], hdr[14]
    oov_off, oov_sz = hdr[15], hdr[16]
    irab_off, irab_sz = hdr[17], hdr[18]
    w_off, w_cnt = hdr[19], hdr[20]

    NON_TERM_V3 = 0x1FFFFF
    if fst_cnt >= NON_TERM_V3 or meta_cnt >= NON_TERM_V3:
        raise ValueError("FST node or vocab count exceeds 21-bit limit")

    fst_v3 = bytearray(fst_cnt * BIT_TRIE_NODE_V3_STRUCT.size)
    for i in range(fst_cnt):
        mask, fc, val = BIT_TRIE_NODE_STRUCT.unpack_from(data, fst_off + i * 16)
        if mask >= (1 << 38):
            raise ValueError(f"FST mask uses bit >= 38 at node {i}")
        v3_val = NON_TERM_V3 if val == UINT32_MAX else val
        lo = mask | ((fc & NON_TERM_V3) << 38) | ((v3_val & 0x1F) << 59)
        hi = (v3_val >> 5) & 0xFFFF
        BIT_TRIE_NODE_V3_STRUCT.pack_into(fst_v3, i * BIT_TRIE_NODE_V3_STRUCT.size, lo, hi)

    nib_bytes = bytearray()
    nib_count = 0
    core_map = {}

    def alloc_core(old_off: int, core_len: int) -> int:
        nonlocal nib_count
        if core_len == 0:
            return 0
        key = (old_off, core_len)
        existing = core_map.get(key)
        if existing is not None:
            return existing
        seq = data[pool_off + old_off : pool_off + old_off + core_len]
        start_nib = nib_count
        for d in seq:
            if (nib_count & 1) == 0:
                nib_bytes.append(d & 0x0F)
            else:
                nib_bytes[-1] |= (d & 0x0F) << 4
            nib_count += 1
        core_map[key] = start_nib
        return start_nib

    meta_v3 = bytearray(meta_cnt * WORD_META_V3_STRUCT.size)
    cands_v3 = bytearray(cand_cnt * WORD_CAND_V3_STRUCT.size)
    end_masks = [0] * meta_cnt

    for wid in range(meta_cnt):
        c_start, c_count, wlen, rflags = WORD_META_STRUCT.unpack_from(data, meta_off + wid * 8)
        raw_cands = []
        core_seqs = []
        core_len = max(0, wlen - 1)
        for k in range(c_count):
            c_idx = c_start + k
            co, lp, eid, fl = WORD_CAND_STRUCT.unpack_from(data, cand_off + c_idx * 8)
            raw_cands.append((co, lp, eid, fl))
            core_seqs.append(data[pool_off + co : pool_off + co + core_len] if core_len > 0 else b"")
            new_co = alloc_core(co, core_len)
            WORD_CAND_V3_STRUCT.pack_into(
                cands_v3, c_idx * WORD_CAND_V3_STRUCT.size,
                pack_u24(new_co), lp, ((fl & 0x0F) << 4) | (eid & 0x0F),
            )
        emask = reachable_ending_mask(raw_cands[:10], wlen, core_seqs[:10])
        end_masks[wid] = emask
        WORD_META_V3_STRUCT.pack_into(
            meta_v3, wid * WORD_META_V3_STRUCT.size,
            pack_u24(c_start), min(c_count, 255), wlen, rflags, emask,
        )

    rows = [[] for _ in range(meta_cnt)]
    for i in range(bg_cap):
        unp = BIGRAM_ENTRY_STRUCT.unpack_from(data, bg_off + i * BIGRAM_ENTRY_STRUCT.size)
        k = unp[0]
        if k == UINT64_MAX:
            continue
        prev_id = (k >> 32) & 0xFFFFFFFF
        curr_id = k & 0xFFFFFFFF
        if prev_id < meta_cnt and curr_id < meta_cnt:
            rows[prev_id].append((curr_id, unp[1 : 1 + NUM_DIACRITICS]))

    bg_row_off = bytearray((meta_cnt + 1) * 4)
    bg_entries = bytearray()
    bg_vals = bytearray()
    val_count = 0
    entry_count = 0

    for prev_id in range(meta_cnt):
        struct.pack_into("<I", bg_row_off, prev_id * 4, entry_count)
        row = rows[prev_id]
        if not row:
            continue
        row.sort(key=lambda x: x[0])
        for curr_id, bonuses in row:
            emask = end_masks[curr_id]
            if min_bigram_lift > 0:
                max_abs = max((abs(bonuses[ce]) for ce in range(NUM_DIACRITICS) if (emask >> ce) & 1), default=0)
                if max_abs < min_bigram_lift:
                    continue
            bg_entries.extend(BIGRAM_ENTRY_V3_STRUCT.pack(pack_u24(curr_id), val_count))
            entry_count += 1
            for ce in range(NUM_DIACRITICS):
                if (emask >> ce) & 1:
                    if quantize_bigrams:
                        q = max(-127, min(127, int(round(bonuses[ce] / 32.0))))
                        bg_vals.extend(struct.pack("<b", q))
                    else:
                        bg_vals.extend(struct.pack("<h", bonuses[ce]))
                    val_count += 1
    struct.pack_into("<I", bg_row_off, meta_cnt * 4, entry_count)

    oov_blob = data[oov_off : oov_off + oov_sz]
    irab_blob = data[irab_off : irab_off + irab_sz]
    oov_v3, oov_compact = convert_tree_blob(oov_blob)
    irab_v3, irab_compact = convert_tree_blob(irab_blob)
    morph_v3, morph_compact = convert_tree_blob(morph_blob)
    flags_v3 = (
        (1 if oov_compact else 0)
        | (2 if irab_compact else 0)
        | (4 if quantize_bigrams else 0)
        | (8 if morph_compact else 0)
    )

    trans_bytes = data[trans_off : trans_off + trans_sz * 2]
    weights_bytes = data[w_off : w_off + w_cnt * 4]

    def align8(x: int) -> int:
        return (x + 7) & ~7

    hdr_sz = HEADER_V4_STRUCT.size if use_v4 else HEADER_V3_STRUCT.size
    off = align8(hdr_sz)
    o_fst = off; off = align8(off + len(fst_v3))
    o_meta = off; off = align8(off + len(meta_v3))
    o_cand = off; off = align8(off + len(cands_v3))
    o_pool = off; off = align8(off + len(nib_bytes))
    o_bg_row = off; off = align8(off + len(bg_row_off))
    o_bg_ent = off; off = align8(off + len(bg_entries))
    o_bg_val = off; off = align8(off + len(bg_vals))
    o_trans = off; off = align8(off + len(trans_bytes))
    o_oov = off; off = align8(off + len(oov_v3))
    o_irab = off; off = align8(off + len(irab_v3))
    o_weights = off; off = align8(off + len(weights_bytes))
    if use_v4:
        o_morph = off
        total_size = align8(off + len(morph_v3))
        hdr_bytes = HEADER_V4_STRUCT.pack(
            TASH_MAGIC, TASH_VERSION_4, flags_v3,
            o_fst, fst_cnt, o_meta, meta_cnt, o_cand, cand_cnt, o_pool, len(nib_bytes),
            o_bg_row, meta_cnt + 1, o_bg_ent, entry_count, o_bg_val, val_count,
            o_trans, trans_sz, o_oov, len(oov_v3), o_irab, len(irab_v3),
            o_weights, w_cnt, o_morph, len(morph_v3),
        )
        sections = [
            (0, hdr_bytes), (o_fst, fst_v3), (o_meta, meta_v3), (o_cand, cands_v3),
            (o_pool, nib_bytes), (o_bg_row, bg_row_off), (o_bg_ent, bg_entries),
            (o_bg_val, bg_vals), (o_trans, trans_bytes), (o_oov, oov_v3),
            (o_irab, irab_v3), (o_weights, weights_bytes), (o_morph, morph_v3),
        ]
    else:
        total_size = align8(o_weights + len(weights_bytes))
        hdr_bytes = HEADER_V3_STRUCT.pack(
            TASH_MAGIC, TASH_VERSION_3, flags_v3,
            o_fst, fst_cnt, o_meta, meta_cnt, o_cand, cand_cnt, o_pool, len(nib_bytes),
            o_bg_row, meta_cnt + 1, o_bg_ent, entry_count, o_bg_val, val_count,
            o_trans, trans_sz, o_oov, len(oov_v3), o_irab, len(irab_v3),
            o_weights, w_cnt,
        )
        sections = [
            (0, hdr_bytes), (o_fst, fst_v3), (o_meta, meta_v3), (o_cand, cands_v3),
            (o_pool, nib_bytes), (o_bg_row, bg_row_off), (o_bg_ent, bg_entries),
            (o_bg_val, bg_vals), (o_trans, trans_bytes), (o_oov, oov_v3),
            (o_irab, irab_v3), (o_weights, weights_bytes),
        ]

    with open(dst_path, "wb") as f:
        f.write(b"\x00" * total_size)
        for sec_off, blob in sections:
            f.seek(sec_off)
            f.write(blob)

    ver_str = "v4" if use_v4 else "v3"
    print(f"Converted v2 ({len(data)/1e6:.2f} MB) -> {ver_str} ({total_size/1e6:.2f} MB), ratio={total_size/len(data):.3f}x")
