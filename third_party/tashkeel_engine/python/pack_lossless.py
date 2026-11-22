import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
TASH_VERSION_7 = 7
HEADER_V7_STRUCT = struct.Struct("<IHH" + "QI" * 14 + "I" + "QII" * 5)
def align8(x: int) -> int:
    return (x + 7) & ~7
from fst_builder import TASH_MAGIC

TASH_COMP_MAGIC = 0x48535A54  # "TSZH"
TASH_COMP_DEFLATE = 1
COMP_HDR_STRUCT = struct.Struct("<IHHII")

V7F_COMPACT_NGRAM = 1 << 2
V7F_COMPACT_CORE_BG = 1 << 3
V7F_COMPACT_VALENCY = 1 << 4

ORDER_TO_CODE = {3: 0, 4: 1, 5: 2, 12: 3, 13: 4, 22: 5}
CODE_TO_ORDER = {v: k for k, v in ORDER_TO_CODE.items()}


def pack_compact_ngrams(raw_blob: bytes, cap: int) -> tuple[bytes, int]:
    entries = []
    for i in range(cap):
        key, b1, e1, order, b2, e2, _ = struct.unpack_from("<QhBBhBB", raw_blob, i * 16)
        if key == 0:
            continue
        if not (-4096 <= b1 <= 4095 and -4096 <= b2 <= 4095):
            raise ValueError(f"Ngram bonus out of 13-bit range: {b1}, {b2}")
        if not (0 <= e1 <= 15 and 0 <= e2 <= 15):
            raise ValueError(f"Ngram ending out of 4-bit range: {e1}, {e2}")
        if order not in ORDER_TO_CODE:
            raise ValueError(f"Unsupported ngram order: {order}")
        entries.append((key, order, b1, e1, b2, e2))

    entries.sort(key=lambda x: (x[0], x[1]))
    num_buckets = 65536
    buckets = [0] * (num_buckets + 1)
    for key, *_ in entries:
        buckets[(key >> 48) + 1] += 1
    for b in range(num_buckets):
        buckets[b + 1] += buckets[b]
    assert buckets[-1] == len(entries)

    out = bytearray(8 + (num_buckets + 1) * 4 + len(entries) * 11)
    struct.pack_into("<II", out, 0, num_buckets, len(entries))
    struct.pack_into(f"<{num_buckets + 1}I", out, 8, *buckets)
    base = 8 + (num_buckets + 1) * 4
    for idx, (key, order, b1, e1, b2, e2) in enumerate(entries):
        klo = key & 0xFFFFFFFFFFFF
        oc = ORDER_TO_CODE[order]
        payload = (
            ((b1 + 4096) & 0x1FFF)
            | (((b2 + 4096) & 0x1FFF) << 13)
            | ((e1 & 0xF) << 26)
            | ((e2 & 0xF) << 30)
            | ((oc & 0x7) << 34)
        )
        off = base + idx * 11
        out[off : off + 6] = klo.to_bytes(6, "little")
        out[off + 6 : off + 11] = payload.to_bytes(5, "little")

        # Exact round-trip self-check
        rklo = int.from_bytes(out[off : off + 6], "little")
        rp = int.from_bytes(out[off + 6 : off + 11], "little")
        assert rklo == klo
        assert (rp & 0x1FFF) - 4096 == b1
        assert ((rp >> 13) & 0x1FFF) - 4096 == b2
        assert ((rp >> 26) & 0xF) == e1
        assert ((rp >> 30) & 0xF) == e2
        assert CODE_TO_ORDER[(rp >> 34) & 0x7] == order

    return bytes(out), len(entries)


def pack_compact_core_bg(raw_blob: bytes, cap: int) -> tuple[bytes, int]:
    entries = []
    for i in range(cap):
        key, c1, c2, b1, b2 = struct.unpack_from("<QHHhh", raw_blob, i * 16)
        if key == 0:
            continue
        entries.append((key, c1, c2, b1, b2))

    entries.sort(key=lambda x: x[0])
    num_buckets = 65536
    buckets = [0] * (num_buckets + 1)
    for key, *_ in entries:
        buckets[(key >> 48) + 1] += 1
    for b in range(num_buckets):
        buckets[b + 1] += buckets[b]
    assert buckets[-1] == len(entries)

    out = bytearray(8 + (num_buckets + 1) * 4 + len(entries) * 14)
    struct.pack_into("<II", out, 0, num_buckets, len(entries))
    struct.pack_into(f"<{num_buckets + 1}I", out, 8, *buckets)
    base = 8 + (num_buckets + 1) * 4
    for idx, (key, c1, c2, b1, b2) in enumerate(entries):
        klo = key & 0xFFFFFFFFFFFF
        off = base + idx * 14
        out[off : off + 6] = klo.to_bytes(6, "little")
        struct.pack_into("<HHhh", out, off + 6, c1, c2, b1, b2)

    return bytes(out), len(entries)


def pack_compact_valency(raw_blob: bytes, num_words: int) -> bytes:
    vals = struct.unpack_from(f"<{num_words}I", raw_blob, 0)
    bw = (num_words + 63) // 64
    sb = (num_words + 255) // 256
    bits = [0] * bw
    supers = [0] * sb
    nz = []
    for wid, v in enumerate(vals):
        if (wid & 255) == 0:
            supers[wid >> 8] = len(nz)
        if v != 0:
            bits[wid >> 6] |= 1 << (wid & 63)
            nz.append(v)

    hdr_sz = align8(16)
    bw_sz = bw * 8
    sb_sz = align8(sb * 4)
    val_sz = len(nz) * 4
    out = bytearray(hdr_sz + bw_sz + sb_sz + val_sz)
    struct.pack_into("<IIII", out, 0, num_words, len(nz), bw, sb)
    struct.pack_into(f"<{bw}Q", out, hdr_sz, *bits)
    struct.pack_into(f"<{sb}I", out, hdr_sz + bw_sz, *supers)
    struct.pack_into(f"<{len(nz)}I", out, hdr_sz + bw_sz + sb_sz, *nz)
    return bytes(out)


def compact_v7_lossless(src_path: str, dst_compact_path: str, dst_compressed_path: str = "") -> None:
    with open(src_path, "rb") as f:
        data = f.read()

    h = HEADER_V7_STRUCT.unpack_from(data, 0)
    magic, ver, flags5 = h[0], h[1], h[2]
    if magic != TASH_MAGIC or ver != TASH_VERSION_7:
        raise ValueError(f"Expected uncompressed V7 .tash, got magic={hex(magic)} ver={ver}")

    cand5 = (flags5 & 2) != 0
    bg_int8 = (flags5 & 4) != 0

    fst_off, fst_sz = h[3], h[4]
    meta_off, meta_cnt = h[5], h[6]
    cand_off, cand_cnt = h[7], h[8]
    pool_off, pool_sz = h[9], h[10]
    bg_row_off, bg_row_cnt = h[11], h[12]
    bg_blk_off, bg_blk_cnt = h[13], h[14]
    bg_ent_off, bg_ent_cnt = h[15], h[16]
    bg_val_off, bg_val_cnt = h[17], h[18]
    tr_off, tr_sz = h[19], h[20]
    oov_off, oov_sz = h[21], h[22]
    irab_off, irab_sz = h[23], h[24]
    morph_off, morph_sz = h[25], h[26]
    w_off, w_cnt = h[27], h[28]

    ng_off, ng_cap, ng_cnt = h[29], h[30], h[31]
    val_off, val_cnt, v7_flags = h[32], h[33], h[34]
    tr3_off, tr3_sz, res1 = h[35], h[36], h[37]
    i2_off, i2_sz, i2_fc = h[38], h[39], h[40]
    cb_off, cb_cap, cb_cnt = h[41], h[42], h[43]
    w7_off, w7_cnt, res2 = h[44], h[45], h[46]

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
    w_blob = data[w_off : w_off + w_cnt * 4]
    tr3_blob = data[tr3_off : tr3_off + tr3_sz] if tr3_sz > 0 else b""
    i2_blob = data[i2_off : i2_off + i2_sz] if i2_sz > 0 else b""
    w7_blob = data[w7_off : w7_off + w7_cnt * 4] if w7_cnt > 0 else b""

    new_v7_flags = v7_flags

    if ng_cap > 0 and not (v7_flags & V7F_COMPACT_NGRAM):
        raw_ng = data[ng_off : ng_off + ng_cap * 16]
        ng_blob, ng_cnt_out = pack_compact_ngrams(raw_ng, ng_cap)
        ng_cap_out = len(ng_blob)
        new_v7_flags |= V7F_COMPACT_NGRAM
        print(f"Compacted v7_ngrams: {len(raw_ng)/1e6:.2f} MB -> {len(ng_blob)/1e6:.2f} MB ({ng_cnt_out:,} entries)")
    else:
        ng_blob = data[ng_off : ng_off + (ng_cap if (v7_flags & V7F_COMPACT_NGRAM) else ng_cap * 16)] if ng_cap > 0 else b""
        ng_cap_out, ng_cnt_out = ng_cap, ng_cnt

    if cb_cap > 0 and not (v7_flags & V7F_COMPACT_CORE_BG):
        raw_cb = data[cb_off : cb_off + cb_cap * 16]
        cb_blob, cb_cnt_out = pack_compact_core_bg(raw_cb, cb_cap)
        cb_cap_out = len(cb_blob)
        new_v7_flags |= V7F_COMPACT_CORE_BG
        print(f"Compacted v7_core_bg: {len(raw_cb)/1e6:.2f} MB -> {len(cb_blob)/1e6:.2f} MB ({cb_cnt_out:,} entries)")
    else:
        cb_blob = data[cb_off : cb_off + (cb_cap if (v7_flags & V7F_COMPACT_CORE_BG) else cb_cap * 16)] if cb_cap > 0 else b""
        cb_cap_out, cb_cnt_out = cb_cap, cb_cnt

    if val_cnt > 0 and not (v7_flags & V7F_COMPACT_VALENCY):
        raw_val = data[val_off : val_off + val_cnt * 4]
        val_blob = pack_compact_valency(raw_val, val_cnt)
        val_cnt_out = len(val_blob)
        new_v7_flags |= V7F_COMPACT_VALENCY
        print(f"Compacted v7_valency: {len(raw_val)/1e6:.2f} MB -> {len(val_blob)/1e6:.2f} MB ({val_cnt:,} words)")
    else:
        val_blob = data[val_off : val_off + (val_cnt if (v7_flags & V7F_COMPACT_VALENCY) else val_cnt * 4)] if val_cnt > 0 else b""
        val_cnt_out = val_cnt

    off = align8(HEADER_V7_STRUCT.size)
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
    total_size = off

    hdr_bytes = HEADER_V7_STRUCT.pack(
        TASH_MAGIC, TASH_VERSION_7, flags5,
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
        o_i2, len(i2_blob), i2_fc,
        o_cb, cb_cap_out, cb_cnt_out,
        o_w7, w7_cnt, res2,
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
    for sec_off, blob in sections:
        out_buf[sec_off : sec_off + len(blob)] = blob

    raw_compact = bytes(out_buf)
    if dst_compact_path:
        with open(dst_compact_path, "wb") as f:
            f.write(raw_compact)
        print(f"Wrote Layer-1 lossless compact model: {dst_compact_path} ({len(raw_compact)/1e6:.2f} MB, -{100*(1-len(raw_compact)/len(data)):.1f}%)")

    if dst_compressed_path:
        co = zlib.compressobj(level=9, method=zlib.DEFLATED, wbits=-15, memLevel=9)
        comp_payload = co.compress(raw_compact) + co.flush()
        crc = zlib.crc32(raw_compact) & 0xFFFFFFFF
        chdr = COMP_HDR_STRUCT.pack(TASH_COMP_MAGIC, TASH_COMP_DEFLATE, 0, len(raw_compact), crc)
        final_bytes = chdr + comp_payload
        with open(dst_compressed_path, "wb") as f:
            f.write(final_bytes)
        print(
            f"Wrote Layer-2 lossless compressed model: {dst_compressed_path} "
            f"({len(final_bytes)/1e6:.2f} MB, -{100*(1-len(final_bytes)/len(data)):.1f}% vs {len(data)/1e6:.2f} MB)"
        )


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--compact-out", default="")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    compact_v7_lossless(args.src, args.compact_out, args.out)
