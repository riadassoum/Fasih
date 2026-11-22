import argparse
import os
import struct
import sys
import zlib
from typing import List, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fst_builder import TASH_MAGIC, convert_tree_v2_to_v5
from pack_lossless import (
    HEADER_V7_STRUCT,
    TASH_VERSION_7,
    align8,
    COMP_HDR_STRUCT,
    TASH_COMP_DEFLATE,
    TASH_COMP_MAGIC,
    V7F_COMPACT_CORE_BG,
    V7F_COMPACT_NGRAM,
    V7F_COMPACT_VALENCY,
    pack_compact_core_bg,
    pack_compact_ngrams,
    pack_compact_valency,
)

TASH_VERSION_8 = 8
HEADER_V8_STRUCT = struct.Struct("<IHH" + "QI" * 14 + "I" + "QII" * 5 + "QIIQIIIIQ")
V8F_CORE2_V5_4B = 1 << 0
V8F_OOV_V8_38F = 1 << 1
DEFAULT_V8_WEIGHTS = [1000, 0, 0]


def read_tash_raw(path: str) -> bytes:
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) >= COMP_HDR_STRUCT.size:
        magic, method, flags, uncomp_sz, crc = COMP_HDR_STRUCT.unpack_from(raw, 0)
        if magic == TASH_COMP_MAGIC:
            if method != TASH_COMP_DEFLATE:
                raise ValueError(f"Unsupported compression method={method}")
            out = zlib.decompress(raw[COMP_HDR_STRUCT.size :], -15)
            if len(out) != uncomp_sz:
                raise ValueError(f"Decompressed size mismatch: {len(out)} != {uncomp_sz}")
            if (zlib.crc32(out) & 0xFFFFFFFF) != crc:
                raise ValueError("CRC-32 mismatch in compressed .tash")
            return out
    return raw


def compress_tash_bytes(raw: bytes) -> bytes:
    co = zlib.compressobj(level=9, method=zlib.DEFLATED, wbits=-15, memLevel=9)
    comp_payload = co.compress(raw) + co.flush()
    crc = zlib.crc32(raw) & 0xFFFFFFFF
    chdr = COMP_HDR_STRUCT.pack(TASH_COMP_MAGIC, TASH_COMP_DEFLATE, 0, len(raw), crc)
    return chdr + comp_payload


def build_v8_model(
    base_path: str,
    dst_path: str,
    oov_v8_path: str = "",
    extra_core_path: str = "",
    core2_blob_path: str = "",
    core_bg_path: str = "",
    ngrams_path: str = "",
    valency_path: str = "",
    trans3_path: str = "",
    irab2_blob_path: str = "",
    v8_weights: Optional[List[int]] = None,
    v7_weights: Optional[List[int]] = None,
    base_weights: Optional[List[int]] = None,
    core2_feat_count: int = 64,
    irab2_feat_count: int = 96,
    compact_tables: bool = True,
    compress_out: bool = False,
) -> int:
    data = read_tash_raw(base_path)
    magic, ver, flags5 = struct.unpack_from("<IHH", data, 0)
    if magic != TASH_MAGIC or ver not in (TASH_VERSION_7, TASH_VERSION_8):
        raise ValueError(f"Expected V7 or V8 base model, got magic={hex(magic)} ver={ver}")

    h7 = HEADER_V7_STRUCT.unpack_from(data, 0)
    h8 = HEADER_V8_STRUCT.unpack_from(data, 0) if ver == TASH_VERSION_8 else None

    cand5 = (flags5 & 2) != 0
    bg_int8 = (flags5 & 4) != 0

    fst_off, fst_sz = h7[3], h7[4]
    meta_off, meta_cnt = h7[5], h7[6]
    cand_off, cand_cnt = h7[7], h7[8]
    pool_off, pool_sz = h7[9], h7[10]
    bg_row_off, bg_row_cnt = h7[11], h7[12]
    bg_blk_off, bg_blk_cnt = h7[13], h7[14]
    bg_ent_off, bg_ent_cnt = h7[15], h7[16]
    bg_val_off, bg_val_cnt = h7[17], h7[18]
    tr_off, tr_sz = h7[19], h7[20]
    oov_off, oov_sz = h7[21], h7[22]
    irab_off, irab_sz = h7[23], h7[24]
    morph_off, morph_sz = h7[25], h7[26]
    w_off, w_cnt = h7[27], h7[28]

    ng_off, ng_cap, ng_cnt = h7[29], h7[30], h7[31]
    val_off, val_cnt, v7_flags = h7[32], h7[33], h7[34]
    tr3_off, tr3_sz, res1 = h7[35], h7[36], h7[37]
    i2_off, i2_sz, i2_fc = h7[38], h7[39], h7[40]
    cb_off, cb_cap, cb_cnt = h7[41], h7[42], h7[43]
    w7_off, w7_cnt, res2 = h7[44], h7[45], h7[46]

    fst_blob = data[fst_off : fst_off + fst_sz]
    meta_blob = data[meta_off : meta_off + meta_cnt * 8]
    cand_blob = data[cand_off : cand_off + cand_cnt * (5 if cand5 else 6)]
    pool_blob = data[pool_off : pool_off + pool_sz]
    bg_row_blob = data[bg_row_off : bg_row_off + bg_row_cnt * 4]
    bg_blk_blob = data[bg_blk_off : bg_blk_off + bg_blk_cnt * 4]
    bg_ent_blob = data[bg_ent_off : bg_ent_off + bg_ent_cnt * 5]
    bg_val_blob = data[bg_val_off : bg_val_off + bg_val_cnt * (1 if bg_int8 else 2)]
    tr_blob = data[tr_off : tr_off + tr_sz * 2]
    oov_blob = data[oov_off : oov_off + oov_sz]
    irab_blob = data[irab_off : irab_off + irab_sz]
    morph_blob = data[morph_off : morph_off + morph_sz]
    if base_weights is not None:
        w_blob = struct.pack(f"<{len(base_weights)}i", *[int(x) for x in base_weights])
        w_cnt = len(base_weights)
    else:
        w_blob = data[w_off : w_off + w_cnt * 4]

    new_v7_flags = v7_flags
    new_v8_flags = h8[53] if h8 is not None else 0

    # Ngrams
    if ngrams_path and os.path.exists(ngrams_path):
        with open(ngrams_path, "rb") as f:
            raw_ng_cap, raw_ng_cnt = struct.unpack("<II", f.read(8))
            raw_ng = f.read()
        if compact_tables:
            ng_blob, ng_cnt_out = pack_compact_ngrams(raw_ng, raw_ng_cap)
            ng_cap_out = len(ng_blob)
            new_v7_flags |= V7F_COMPACT_NGRAM
        else:
            ng_blob, ng_cap_out, ng_cnt_out = raw_ng, raw_ng_cap, raw_ng_cnt
            new_v7_flags &= ~V7F_COMPACT_NGRAM
    else:
        if ng_cap > 0 and compact_tables and not (v7_flags & V7F_COMPACT_NGRAM):
            raw_ng = data[ng_off : ng_off + ng_cap * 16]
            ng_blob, ng_cnt_out = pack_compact_ngrams(raw_ng, ng_cap)
            ng_cap_out = len(ng_blob)
            new_v7_flags |= V7F_COMPACT_NGRAM
        else:
            ng_sz = ng_cap if (v7_flags & V7F_COMPACT_NGRAM) else ng_cap * 16
            ng_blob = data[ng_off : ng_off + ng_sz] if ng_cap > 0 else b""
            ng_cap_out, ng_cnt_out = ng_cap, ng_cnt

    # Valency
    if valency_path and os.path.exists(valency_path):
        with open(valency_path, "rb") as f:
            raw_val = f.read()
        raw_val_cnt = len(raw_val) // 4
        if compact_tables:
            val_blob = pack_compact_valency(raw_val, raw_val_cnt)
            val_cnt_out = len(val_blob)
            new_v7_flags |= V7F_COMPACT_VALENCY
        else:
            val_blob, val_cnt_out = raw_val, raw_val_cnt
            new_v7_flags &= ~V7F_COMPACT_VALENCY
    else:
        if val_cnt > 0 and compact_tables and not (v7_flags & V7F_COMPACT_VALENCY):
            raw_val = data[val_off : val_off + val_cnt * 4]
            val_blob = pack_compact_valency(raw_val, val_cnt)
            val_cnt_out = len(val_blob)
            new_v7_flags |= V7F_COMPACT_VALENCY
        else:
            val_sz = val_cnt if (v7_flags & V7F_COMPACT_VALENCY) else val_cnt * 4
            val_blob = data[val_off : val_off + val_sz] if val_cnt > 0 else b""
            val_cnt_out = val_cnt

    # Trans3
    if trans3_path and os.path.exists(trans3_path):
        with open(trans3_path, "rb") as f:
            tr3_blob = f.read()
    else:
        tr3_blob = data[tr3_off : tr3_off + tr3_sz] if tr3_sz > 0 else b""

    # Irab2
    if irab2_blob_path and os.path.exists(irab2_blob_path):
        with open(irab2_blob_path, "rb") as f:
            raw_i2 = f.read()
        packed_i2, ok_v5 = convert_tree_v2_to_v5(raw_i2)
        if ok_v5:
            i2_blob = packed_i2
            new_v7_flags = (new_v7_flags & ~2) | 1
        else:
            i2_blob = raw_i2
            new_v7_flags &= ~3
        i2_fc_out = irab2_feat_count
    else:
        i2_blob = data[i2_off : i2_off + i2_sz] if i2_sz > 0 else b""
        i2_fc_out = i2_fc

    # CoreBg
    if core_bg_path and os.path.exists(core_bg_path):
        with open(core_bg_path, "rb") as f:
            raw_cb_cap, raw_cb_cnt = struct.unpack("<II", f.read(8))
            raw_cb = f.read()
        if compact_tables:
            cb_blob, cb_cnt_out = pack_compact_core_bg(raw_cb, raw_cb_cap)
            cb_cap_out = len(cb_blob)
            new_v7_flags |= V7F_COMPACT_CORE_BG
        else:
            cb_blob, cb_cap_out, cb_cnt_out = raw_cb, raw_cb_cap, raw_cb_cnt
            new_v7_flags &= ~V7F_COMPACT_CORE_BG
    else:
        if cb_cap > 0 and compact_tables and not (v7_flags & V7F_COMPACT_CORE_BG):
            raw_cb = data[cb_off : cb_off + cb_cap * 16]
            cb_blob, cb_cnt_out = pack_compact_core_bg(raw_cb, cb_cap)
            cb_cap_out = len(cb_blob)
            new_v7_flags |= V7F_COMPACT_CORE_BG
        else:
            cb_sz = cb_cap if (v7_flags & V7F_COMPACT_CORE_BG) else cb_cap * 16
            cb_blob = data[cb_off : cb_off + cb_sz] if cb_cap > 0 else b""
            cb_cap_out, cb_cnt_out = cb_cap, cb_cnt

    # V7 weights
    if v7_weights is not None:
        w7_blob = struct.pack(f"<{len(v7_weights)}i", *[int(x) for x in v7_weights])
        w7_cnt_out = len(v7_weights)
    else:
        w7_blob = data[w7_off : w7_off + w7_cnt * 4] if w7_cnt > 0 else b""
        w7_cnt_out = w7_cnt

    # V8: OOV-V8 (38 features, 4B/node)
    if oov_v8_path and os.path.exists(oov_v8_path):
        with open(oov_v8_path, "rb") as f:
            oov_raw = f.read()
        # Check if already v5_4B or raw v2 (v2 node is 24 bytes, v5 is 4 bytes)
        nt, nc, nn, bsq = struct.unpack_from("<IIIi", oov_raw, 0)
        expected_v5 = 16 + nt * 4 + nn * 4
        if len(oov_raw) == expected_v5:
            oov_blob = oov_raw
        else:
            oov_blob, ok_v5 = convert_tree_v2_to_v5(oov_raw)
            if not ok_v5:
                raise ValueError("OOV-V8 tree blob could not pack to v5_4B!")
        new_v8_flags |= V8F_OOV_V8_38F

    # V8: ExtraCore table
    if extra_core_path and os.path.exists(extra_core_path):
        with open(extra_core_path, "rb") as f:
            ec_blob = f.read()
        _, _, ec_cnt, _, _, _ = struct.unpack_from("<IIIIII", ec_blob, 0)
    elif h8 is not None and h8[48] > 0:
        ec_blob = data[h8[47] : h8[47] + h8[48]]
        ec_cnt = h8[49]
    else:
        ec_blob, ec_cnt = b"", 0

    # V8: Core2-GBDT
    if core2_blob_path and os.path.exists(core2_blob_path):
        with open(core2_blob_path, "rb") as f:
            raw_c2 = f.read()
        nt, nc, nn, bsq = struct.unpack_from("<IIIi", raw_c2, 0)
        if len(raw_c2) == 16 + nt * 4 + nn * 4:
            c2_blob = raw_c2
            new_v8_flags |= V8F_CORE2_V5_4B
        else:
            packed_c2, ok_v5 = convert_tree_v2_to_v5(raw_c2)
            if ok_v5:
                c2_blob = packed_c2
                new_v8_flags |= V8F_CORE2_V5_4B
            else:
                c2_blob = raw_c2
                new_v8_flags &= ~V8F_CORE2_V5_4B
        c2_fc_out = core2_feat_count
    elif h8 is not None and h8[51] > 0:
        c2_blob = data[h8[50] : h8[50] + h8[51]]
        c2_fc_out = h8[52]
    else:
        c2_blob, c2_fc_out = b"", 0

    # V8 weights
    if v8_weights is not None:
        w8_list = [int(x) for x in v8_weights]
    elif h8 is not None and h8[54] > 0:
        w8_list = list(struct.unpack_from(f"<{h8[54]}i", data, h8[55]))
    else:
        w8_list = list(DEFAULT_V8_WEIGHTS)
    w8_blob = struct.pack(f"<{len(w8_list)}i", *w8_list)

    off = align8(HEADER_V8_STRUCT.size)
    o_fst = off; off = align8(off + len(fst_blob))
    o_meta = off; off = align8(off + len(meta_blob))
    o_cand = off; off = align8(off + len(cand_blob))
    o_pool = off; off = align8(off + len(pool_blob))
    o_bg_row = off; off = align8(off + len(bg_row_blob))
    o_bg_blk = off; off = align8(off + len(bg_blk_blob))
    o_bg_ent = off; off = align8(off + len(bg_ent_blob))
    o_bg_val = off; off = align8(off + len(bg_val_blob))
    o_tr = off; off = align8(off + len(tr_blob))
    o_oov = off; off = align8(off + len(oov_blob))
    o_irab = off; off = align8(off + len(irab_blob))
    o_morph = off; off = align8(off + len(morph_blob))
    o_w = off; off = align8(off + len(w_blob))
    o_ng = off if ng_blob else 0; off = align8(off + len(ng_blob))
    o_val = off if val_blob else 0; off = align8(off + len(val_blob))
    o_tr3 = off if tr3_blob else 0; off = align8(off + len(tr3_blob))
    o_i2 = off if i2_blob else 0; off = align8(off + len(i2_blob))
    o_cb = off if cb_blob else 0; off = align8(off + len(cb_blob))
    o_w7 = off if w7_blob else 0; off = align8(off + len(w7_blob))
    o_ec = off if ec_blob else 0; off = align8(off + len(ec_blob))
    o_c2 = off if c2_blob else 0; off = align8(off + len(c2_blob))
    o_w8 = off; off = align8(off + len(w8_blob))
    total_size = off

    hdr_bytes = HEADER_V8_STRUCT.pack(
        TASH_MAGIC, TASH_VERSION_8, flags5,
        o_fst, len(fst_blob),
        o_meta, meta_cnt,
        o_cand, cand_cnt,
        o_pool, len(pool_blob),
        o_bg_row, bg_row_cnt,
        o_bg_blk, bg_blk_cnt,
        o_bg_ent, bg_ent_cnt,
        o_bg_val, bg_val_cnt,
        o_tr, tr_sz,
        o_oov, len(oov_blob),
        o_irab, len(irab_blob),
        o_morph, len(morph_blob),
        o_w, w_cnt,
        o_ng, ng_cap_out, ng_cnt_out,
        o_val, val_cnt_out, new_v7_flags,
        o_tr3, len(tr3_blob), res1,
        o_i2, len(i2_blob), i2_fc_out,
        o_cb, cb_cap_out, cb_cnt_out,
        o_w7, w7_cnt_out, res2,
        o_ec, len(ec_blob), ec_cnt,
        o_c2, len(c2_blob), c2_fc_out,
        new_v8_flags, len(w8_list), o_w8,
    )

    out_buf = bytearray(total_size)
    sections = [
        (0, hdr_bytes), (o_fst, fst_blob), (o_meta, meta_blob), (o_cand, cand_blob),
        (o_pool, pool_blob), (o_bg_row, bg_row_blob), (o_bg_blk, bg_blk_blob),
        (o_bg_ent, bg_ent_blob), (o_bg_val, bg_val_blob), (o_tr, tr_blob),
        (o_oov, oov_blob), (o_irab, irab_blob), (o_morph, morph_blob), (o_w, w_blob),
    ]
    if ng_blob: sections.append((o_ng, ng_blob))
    if val_blob: sections.append((o_val, val_blob))
    if tr3_blob: sections.append((o_tr3, tr3_blob))
    if i2_blob: sections.append((o_i2, i2_blob))
    if cb_blob: sections.append((o_cb, cb_blob))
    if w7_blob: sections.append((o_w7, w7_blob))
    if ec_blob: sections.append((o_ec, ec_blob))
    if c2_blob: sections.append((o_c2, c2_blob))
    sections.append((o_w8, w8_blob))
    for sec_off, blob in sections:
        out_buf[sec_off : sec_off + len(blob)] = blob

    raw_bytes = bytes(out_buf)
    os.makedirs(os.path.dirname(os.path.abspath(dst_path)), exist_ok=True)
    if compress_out:
        final_bytes = compress_tash_bytes(raw_bytes)
        with open(dst_path, "wb") as f:
            f.write(final_bytes)
        print(
            f"Wrote compressed V8 model {dst_path}: {len(final_bytes)/1e6:.2f} MB "
            f"(raw={len(raw_bytes)/1e6:.2f} MB) | oov={len(oov_blob)/1e3:.0f}KB(38f={bool(new_v8_flags&2)}) "
            f"extra_core={ec_cnt:,}({len(ec_blob)/1e3:.0f}KB) core_bg={cb_cnt_out:,} "
            f"core2={len(c2_blob)/1e3:.0f}KB(v5={bool(new_v8_flags&1)}) w8={w8_list}"
        )
        return len(final_bytes)
    else:
        with open(dst_path, "wb") as f:
            f.write(raw_bytes)
        print(
            f"Wrote uncompressed V8 model {dst_path}: {len(raw_bytes)/1e6:.2f} MB | "
            f"oov={len(oov_blob)/1e3:.0f}KB(38f={bool(new_v8_flags&2)}) "
            f"extra_core={ec_cnt:,}({len(ec_blob)/1e3:.0f}KB) core_bg={cb_cnt_out:,} "
            f"core2={len(c2_blob)/1e3:.0f}KB(v5={bool(new_v8_flags&1)}) w8={w8_list}"
        )
        return len(raw_bytes)


def update_v8_weights_inplace(
    model_path: str,
    base_weights: Optional[List[int]] = None,
    v7_weights: Optional[List[int]] = None,
    v8_weights: Optional[List[int]] = None,
):
    with open(model_path, "r+b") as f:
        hdr = HEADER_V8_STRUCT.unpack(f.read(HEADER_V8_STRUCT.size))
        if hdr[0] != TASH_MAGIC or hdr[1] != TASH_VERSION_8:
            raise ValueError("Not an uncompressed V8 .tash model")
        if base_weights is not None:
            w_off, w_cnt = hdr[27], hdr[28]
            assert len(base_weights) == w_cnt
            f.seek(w_off)
            f.write(struct.pack(f"<{w_cnt}i", *[int(x) for x in base_weights]))
        if v7_weights is not None:
            w7_off, w7_cnt = hdr[44], hdr[45]
            assert len(v7_weights) == w7_cnt
            f.seek(w7_off)
            f.write(struct.pack(f"<{w7_cnt}i", *[int(x) for x in v7_weights]))
        if v8_weights is not None:
            w8_cnt, w8_off = hdr[54], hdr[55]
            assert len(v8_weights) == w8_cnt
            f.seek(w8_off)
            f.write(struct.pack(f"<{w8_cnt}i", *[int(x) for x in v8_weights]))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--oov", default="")
    ap.add_argument("--extra-core", default="")
    ap.add_argument("--core2", default="")
    ap.add_argument("--core2-feats", type=int, default=72)
    ap.add_argument("--core-bg", default="")
    ap.add_argument("--ngrams", default="")
    ap.add_argument("--valency", default="")
    ap.add_argument("--trans3", default="")
    ap.add_argument("--irab2", default="")
    ap.add_argument("--v8-weights", default="")
    ap.add_argument("--v7-weights", default="")
    ap.add_argument("--compress", action="store_true")
    args = ap.parse_args()
    w8 = [int(x) for x in args.v8_weights.split(",")] if args.v8_weights else None
    w7 = [int(x) for x in args.v7_weights.split(",")] if args.v7_weights else None
    build_v8_model(
        args.base,
        args.out,
        oov_v8_path=args.oov,
        extra_core_path=args.extra_core,
        core2_blob_path=args.core2,
        core2_feat_count=args.core2_feats,
        core_bg_path=args.core_bg,
        ngrams_path=args.ngrams,
        valency_path=args.valency,
        trans3_path=args.trans3,
        irab2_blob_path=args.irab2,
        v8_weights=w8,
        v7_weights=w7,
        compress_out=args.compress,
    )
