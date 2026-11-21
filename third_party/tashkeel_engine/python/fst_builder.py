import math
import struct
from collections import deque
from typing import Dict, List, Optional, Tuple

import numpy as np

from arabic_utils import NUM_DIACRITICS

TASH_MAGIC = 0x48534154
TASH_VERSION = 2
TASH_VERSION_3 = 3
TASH_VERSION_4 = 4
TASH_VERSION_5 = 5
UINT32_MAX = 0xFFFFFFFF
UINT64_MAX = 0xFFFFFFFFFFFFFFFF

DEFAULT_WEIGHTS = [450, 950, 800, 1200, 1000, 750, 700, 200, 450, 250, 2400, 1000, 1000]
NUM_WEIGHTS = len(DEFAULT_WEIGHTS)

# FileHeader (v2): uint32 magic, uint16 version, uint16 flags, 9 x (uint64 offset, uint32 count/size)
HEADER_STRUCT = struct.Struct("<IHH" + "QI" * 9)
HEADER_V3_STRUCT = struct.Struct("<IHH" + "QI" * 11)
HEADER_V4_STRUCT = struct.Struct("<IHH" + "QI" * 12)
HEADER_V5_STRUCT = struct.Struct("<IHH" + "QI" * 13)
FST_V5_HDR_STRUCT = struct.Struct("<6I")
BIT_TRIE_NODE_STRUCT = struct.Struct("<QII")
BIT_TRIE_NODE_V3_STRUCT = struct.Struct("<QH")
WORD_META_STRUCT = struct.Struct("<IHBB")
WORD_META_V3_STRUCT = struct.Struct("<3sBBBH")
WORD_CAND_STRUCT = struct.Struct("<IhBB")
WORD_CAND_V3_STRUCT = struct.Struct("<3shB")
WORD_CAND_V5_STRUCT = struct.Struct("<5s")
BIGRAM_ENTRY_STRUCT = struct.Struct("<Q15hH")
BIGRAM_ENTRY_V3_STRUCT = struct.Struct("<3sI")
BIGRAM_ENTRY_V5_STRUCT = struct.Struct("<3sH")
TREE_HDR_STRUCT = struct.Struct("<IIIi")
FLAT_TREE_NODE_STRUCT = struct.Struct("<QiiiHBB")
FLAT_TREE_NODE_V3_STRUCT = struct.Struct("<iHHBB")

NODE_NUM = 0
NODE_LEAF = 1
NODE_CAT = 2
BG_BLOCK_SHIFT_V5 = 5


def build_bitmapped_fst(words_syms: List[bytes]) -> bytes:
    """Builds a breadth-first 64-bit bitmapped succinct trie."""
    children_map: List[Dict[int, int]] = [{}]
    values: List[int] = [UINT32_MAX]

    for word_id, syms in enumerate(words_syms):
        cur = 0
        for s in syms:
            if s >= 64:
                raise ValueError(f"Symbol {s} exceeds 64-bit mask")
            nxt = children_map[cur].get(s)
            if nxt is None:
                nxt = len(children_map)
                children_map[cur][s] = nxt
                children_map.append({})
                values.append(UINT32_MAX)
            cur = nxt
        values[cur] = word_id

    bfs_order: List[int] = []
    old_to_new: Dict[int, int] = {0: 0}
    q = deque([0])
    while q:
        u = q.popleft()
        bfs_order.append(u)
        for s in sorted(children_map[u].keys()):
            v = children_map[u][s]
            old_to_new[v] = len(old_to_new)
            q.append(v)

    buf = bytearray(len(bfs_order) * BIT_TRIE_NODE_STRUCT.size)
    for new_idx, old_idx in enumerate(bfs_order):
        ch_dict = children_map[old_idx]
        mask = 0
        first_child = 0
        if ch_dict:
            sorted_syms = sorted(ch_dict.keys())
            first_child = old_to_new[ch_dict[sorted_syms[0]]]
            for s in sorted_syms:
                mask |= (1 << s)
        BIT_TRIE_NODE_STRUCT.pack_into(
            buf, new_idx * BIT_TRIE_NODE_STRUCT.size, mask, first_child, values[old_idx]
        )
    return bytes(buf)


def build_bitmapped_fst_v5(words_syms: List[bytes]) -> Tuple[bytes, List[int]]:
    """Builds a lossless FST with rank-indexed terminals, leaf elimination, and linear tail collapse.
    Returns (fst_blob, old_wid_in_new_bfs_order).
    """
    children_map: List[Dict[int, int]] = [{}]
    values: List[int] = [UINT32_MAX]

    for word_id, syms in enumerate(words_syms):
        cur = 0
        for s in syms:
            if s >= 38:
                raise ValueError(f"Symbol {s} >= 38 in FST")
            nxt = children_map[cur].get(s)
            if nxt is None:
                nxt = len(children_map)
                children_map[cur][s] = nxt
                children_map.append({})
                values.append(UINT32_MAX)
            cur = nxt
        values[cur] = word_id

    n_raw = len(children_map)
    # Bottom-up pure linear tail detection (post-order by index since parent < child)
    is_pure = [False] * n_raw
    for u in range(n_raw - 1, -1, -1):
        ch = children_map[u]
        if len(ch) == 0:
            is_pure[u] = values[u] != UINT32_MAX
        elif len(ch) == 1 and values[u] == UINT32_MAX:
            v = next(iter(ch.values()))
            is_pure[u] = is_pure[v]

    # BFS with tail collapsing for pure linear chains of length >= 2
    bfs_nodes: List[int] = []
    old_to_new: Dict[int, int] = {0: 0}
    tail_data: Dict[int, Tuple[bytes, int]] = {}  # old_u -> (tail_syms, old_wid)
    q = deque([0])
    while q:
        u = q.popleft()
        bfs_nodes.append(u)
        if u != 0 and is_pure[u] and len(children_map[u]) == 1:
            # Check chain length >= 2
            cur = u
            seq = []
            while len(children_map[cur]) == 1:
                sym, nxt = next(iter(children_map[cur].items()))
                seq.append(sym)
                cur = nxt
            if len(seq) >= 2 and values[cur] != UINT32_MAX:
                tail_data[u] = (bytes(seq), values[cur])
                continue
        for s in sorted(children_map[u].keys()):
            v = children_map[u][s]
            old_to_new[v] = len(old_to_new)
            q.append(v)

    num_nodes = len(bfs_nodes)
    bitmap_words = (num_nodes + 63) // 64
    super_blocks = (num_nodes + 255) // 256

    is_term_bits = bytearray(bitmap_words * 8)
    has_child_bits = bytearray(bitmap_words * 8)
    internal_u64: List[int] = []
    tails_pool = bytearray()
    old_wid_order: List[int] = []

    # First pass: collect BFS terminal words to compute old->new word_id mapping!
    # Wait: tail_data words are NOT in is_term_bits (they store new_wid directly in tail_pool)!
    # So assign 0..M-1 to is_term_bits nodes in BFS order, and M..V-1 to tail_data words!
    new_wid_of_old: Dict[int, int] = {}
    for new_idx, old_u in enumerate(bfs_nodes):
        if old_u not in tail_data and values[old_u] != UINT32_MAX:
            new_wid = len(old_wid_order)
            old_wid_order.append(values[old_u])
            new_wid_of_old[old_u] = new_wid
            byte_i = new_idx >> 3
            is_term_bits[byte_i] |= 1 << (new_idx & 7)

    for old_u, (_, old_wid) in tail_data.items():
        new_wid = len(old_wid_order)
        old_wid_order.append(old_wid)
        new_wid_of_old[old_u] = new_wid

    for new_idx, old_u in enumerate(bfs_nodes):
        if old_u in tail_data:
            seq, _ = tail_data[old_u]
            new_wid = new_wid_of_old[old_u]
            toff = len(tails_pool)
            if toff >= (1 << 26):
                raise ValueError("FST tail pool exceeds 26-bit offset")
            tails_pool.append(len(seq))
            tails_pool.extend(seq)
            tails_pool.extend(bytes((new_wid & 0xFF, (new_wid >> 8) & 0xFF, (new_wid >> 16) & 0xFF)))
            has_child_bits[new_idx >> 3] |= 1 << (new_idx & 7)
            internal_u64.append(toff << 38)  # child_mask == 0 marks tail!
        elif children_map[old_u]:
            has_child_bits[new_idx >> 3] |= 1 << (new_idx & 7)
            ch = children_map[old_u]
            sorted_syms = sorted(ch.keys())
            first_child = old_to_new[ch[sorted_syms[0]]]
            mask = 0
            for s in sorted_syms:
                mask |= 1 << s
            internal_u64.append(mask | (first_child << 38))

    def build_super(bits_ba: bytearray) -> bytes:
        sup = bytearray(super_blocks * 4)
        u64_arr = struct.unpack(f"<{bitmap_words}Q", bits_ba)
        running = 0
        for sb in range(super_blocks):
            struct.pack_into("<I", sup, sb * 4, running)
            w0 = sb * 4
            for w in range(w0, min(w0 + 4, bitmap_words)):
                running += u64_arr[w].bit_count()
        if (super_blocks & 1) != 0:
            sup.extend(b"\x00\x00\x00\x00")
        return bytes(sup)

    is_term_sup = build_super(is_term_bits)
    has_child_sup = build_super(has_child_bits)
    internal_bytes = struct.pack(f"<{len(internal_u64)}Q", *internal_u64) if internal_u64 else b""

    hdr = FST_V5_HDR_STRUCT.pack(
        num_nodes,
        len(internal_u64),
        len(old_wid_order),
        len(tails_pool),
        bitmap_words,
        super_blocks,
    )
    blob = hdr + bytes(is_term_bits) + is_term_sup + bytes(has_child_bits) + has_child_sup + internal_bytes + bytes(tails_pool)
    return blob, old_wid_order


def splitmix64(x: int) -> int:
    x = (x + 0x9E3779B97F4A7C15) & UINT64_MAX
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & UINT64_MAX
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & UINT64_MAX
    return (x ^ (x >> 31)) & UINT64_MAX


def build_bigram_table(bigrams: Dict[Tuple[int, int], Tuple[List[int], int]]) -> Tuple[bytes, int]:
    if not bigrams:
        return b"", 0
    cap = 16
    while cap < len(bigrams) * 2:
        cap *= 2
    mask = cap - 1
    entry_size = BIGRAM_ENTRY_STRUCT.size
    buf = bytearray(cap * entry_size)
    empty_entry = BIGRAM_ENTRY_STRUCT.pack(UINT64_MAX, *([0] * NUM_DIACRITICS), 0)
    for i in range(cap):
        buf[i * entry_size : (i + 1) * entry_size] = empty_entry

    for (prev_id, curr_id), (bonuses, prev_mask) in bigrams.items():
        key = ((prev_id & 0xFFFFFFFF) << 32) | (curr_id & 0xFFFFFFFF)
        idx = splitmix64(key) & mask
        for step in range(64):
            slot = (idx + step) & mask
            existing_key = struct.unpack_from("<Q", buf, slot * entry_size)[0]
            if existing_key == UINT64_MAX:
                clamped = [max(-32000, min(32000, int(b))) for b in bonuses]
                BIGRAM_ENTRY_STRUCT.pack_into(
                    buf, slot * entry_size, key, *clamped, int(prev_mask) & 0xFFFF
                )
                break
    return bytes(buf), cap


def export_lightgbm_ensemble(booster, num_classes: int, scale: float = 1000.0) -> bytes:
    """Serializes a LightGBM Booster (supports numeric & categorical splits) into v2 FlatTreeEnsemble."""
    dump = booster.dump_model()
    tree_infos = dump["tree_info"]
    num_trees = len(tree_infos)

    tree_roots: List[int] = []
    flat_nodes: List[Tuple[int, int, int, int, int, int, int]] = []

    for t_info in tree_infos:
        t_idx = int(t_info["tree_index"])
        class_id = t_idx % num_classes
        tree_roots.append(len(flat_nodes))

        def emit_node(node: dict) -> int:
            my_idx = len(flat_nodes)
            flat_nodes.append((0, 0, 0, 0, 0, 0, 0))
            if "leaf_value" in node:
                val_q = int(round(float(node["leaf_value"]) * scale))
                val_q = max(-32000, min(32000, val_q))
                flat_nodes[my_idx] = (0, val_q, 0, 0, 0, NODE_LEAF, class_id)
            else:
                feat = int(node["split_feature"])
                dec_type = node.get("decision_type", "<=")
                left_idx = emit_node(node["left_child"])
                right_idx = emit_node(node["right_child"])
                if dec_type == "==":
                    cats = str(node["threshold"]).split("||")
                    cat_mask = 0
                    for c_str in cats:
                        c_val = int(float(c_str))
                        if 0 <= c_val < 64:
                            cat_mask |= (1 << c_val)
                    flat_nodes[my_idx] = (cat_mask, 0, left_idx, right_idx, feat, NODE_CAT, class_id)
                else:
                    thresh = int(math.floor(float(node["threshold"])))
                    flat_nodes[my_idx] = (0, thresh, left_idx, right_idx, feat, NODE_NUM, class_id)
            return my_idx

        emit_node(t_info["tree_structure"])

    out = bytearray(
        TREE_HDR_STRUCT.size
        + num_trees * 4
        + len(flat_nodes) * FLAT_TREE_NODE_STRUCT.size
    )
    TREE_HDR_STRUCT.pack_into(out, 0, num_trees, num_classes, len(flat_nodes), 0)
    off = TREE_HDR_STRUCT.size
    for r in tree_roots:
        struct.pack_into("<I", out, off, r)
        off += 4
    for n in flat_nodes:
        FLAT_TREE_NODE_STRUCT.pack_into(out, off, *n)
        off += FLAT_TREE_NODE_STRUCT.size
    return bytes(out)


def export_sklearn_rf(rf_model, num_classes: int, scale: float = 1000.0) -> bytes:
    estimators = getattr(rf_model, "estimators_", [rf_model])
    num_trees = len(estimators)
    tree_roots: List[int] = []
    flat_nodes: List[Tuple[int, int, int, int, int, int, int]] = []

    actual_trees = 0
    for est in estimators:
        tr = est.tree_
        for c in range(num_classes):
            tree_roots.append(len(flat_nodes))
            actual_trees += 1

            def emit_sk(node_id: int, cid: int) -> int:
                my_idx = len(flat_nodes)
                flat_nodes.append((0, 0, 0, 0, 0, 0, 0))
                if tr.children_left[node_id] == -1:
                    counts = tr.value[node_id][0]
                    tot = float(np.sum(counts)) + 1e-9
                    prob = 0.0
                    if cid in est.classes_:
                        idx = list(est.classes_).index(cid)
                        prob = float(counts[idx]) / tot
                    score = math.log(max(prob, 1e-4)) / max(1, num_trees)
                    val_q = int(round(score * scale))
                    flat_nodes[my_idx] = (0, val_q, 0, 0, 0, NODE_LEAF, cid)
                else:
                    feat = int(tr.feature[node_id])
                    thresh = int(math.floor(float(tr.threshold[node_id])))
                    left_i = emit_sk(int(tr.children_left[node_id]), cid)
                    right_i = emit_sk(int(tr.children_right[node_id]), cid)
                    flat_nodes[my_idx] = (0, thresh, left_i, right_i, feat, NODE_NUM, cid)
                return my_idx

            emit_sk(0, c)

    out = bytearray(
        TREE_HDR_STRUCT.size
        + actual_trees * 4
        + len(flat_nodes) * FLAT_TREE_NODE_STRUCT.size
    )
    TREE_HDR_STRUCT.pack_into(out, 0, actual_trees, num_classes, len(flat_nodes), 0)
    off = TREE_HDR_STRUCT.size
    for r in tree_roots:
        struct.pack_into("<I", out, off, r)
        off += 4
    for n in flat_nodes:
        FLAT_TREE_NODE_STRUCT.pack_into(out, off, *n)
        off += FLAT_TREE_NODE_STRUCT.size
    return bytes(out)


def convert_tree_v2_to_v5(blob: bytes) -> Tuple[bytes, bool]:
    """Converts preorder v2 FlatTreeEnsemble to 4-byte aligned uint32_t format (100% lossless)."""
    if not blob:
        return b"", False
    nt, nc, nn, bsq = TREE_HDR_STRUCT.unpack_from(blob, 0)
    roots_off = TREE_HDR_STRUCT.size
    nodes_off = roots_off + nt * 4
    nodes_u32 = bytearray(nn * 4)
    for i in range(nn):
        _, th, l, r, feat, kind, cid = FLAT_TREE_NODE_STRUCT.unpack_from(blob, nodes_off + i * FLAT_TREE_NODE_STRUCT.size)
        if kind == NODE_LEAF:
            if th < -32768 or th > 32767 or cid >= 32:
                return blob, False
            val_u32 = (1 << 31) | ((cid & 0x1F) << 26) | (th & 0xFFFF)
        elif kind == NODE_NUM:
            if l != i + 1:
                return blob, False
            r_delta = r - i
            th_p8 = th + 8
            if r_delta <= 0 or r_delta > 0x1FFFF or feat >= 128 or th_p8 < 0 or th_p8 > 127:
                return blob, False
            val_u32 = ((th_p8 & 0x7F) << 24) | ((feat & 0x7F) << 17) | (r_delta & 0x1FFFF)
        else:
            return blob, False
        struct.pack_into("<I", nodes_u32, i * 4, val_u32)
    return bytes(blob[:nodes_off]) + bytes(nodes_u32), True


def write_tash_model(
    output_path: str,
    fst_bytes: bytes,
    word_meta_bytes: bytes,
    word_meta_count: int,
    candidates_bytes: bytes,
    candidates_count: int,
    core_pool_bytes: bytes,
    bigram_bytes: bytes,
    bigram_capacity: int,
    trans_matrix_bytes: bytes,
    trans_matrix_size: int,
    oov_trees_bytes: bytes,
    irab_trees_bytes: bytes,
    weights: Optional[List[int]] = None,
) -> int:
    fst_nodes_count = len(fst_bytes) // BIT_TRIE_NODE_STRUCT.size
    w_list = list(weights) if weights is not None else list(DEFAULT_WEIGHTS)
    weights_bytes = struct.pack(f"<{len(w_list)}i", *w_list)

    def align8(x: int) -> int:
        return (x + 7) & ~7

    off = align8(HEADER_STRUCT.size)
    fst_off = off; off = align8(off + len(fst_bytes))
    meta_off = off; off = align8(off + len(word_meta_bytes))
    cand_off = off; off = align8(off + len(candidates_bytes))
    pool_off = off; off = align8(off + len(core_pool_bytes))
    bg_off = off; off = align8(off + len(bigram_bytes))
    trans_off = off; off = align8(off + len(trans_matrix_bytes))
    oov_off = off; off = align8(off + len(oov_trees_bytes))
    irab_off = off; off = align8(off + len(irab_trees_bytes))
    weights_off = off
    total_size = align8(off + len(weights_bytes))

    hdr = HEADER_STRUCT.pack(
        TASH_MAGIC, TASH_VERSION, 0,
        fst_off, fst_nodes_count,
        meta_off, word_meta_count,
        cand_off, candidates_count,
        pool_off, len(core_pool_bytes),
        bg_off, bigram_capacity,
        trans_off, trans_matrix_size,
        oov_off, len(oov_trees_bytes),
        irab_off, len(irab_trees_bytes),
        weights_off, len(w_list),
    )

    with open(output_path, "wb") as f:
        f.write(b"\x00" * total_size)
        for sec_off, blob in [
            (0, hdr), (fst_off, fst_bytes), (meta_off, word_meta_bytes),
            (cand_off, candidates_bytes), (pool_off, core_pool_bytes),
            (bg_off, bigram_bytes), (trans_off, trans_matrix_bytes),
            (oov_off, oov_trees_bytes), (irab_off, irab_trees_bytes),
            (weights_off, weights_bytes),
        ]:
            f.seek(sec_off)
            f.write(blob)

    return total_size


def update_tash_weights(model_path: str, weights: List[int]) -> None:
    """Updates the calibrated weights array inside an existing .tash model in place."""
    with open(model_path, "r+b") as f:
        magic, version = struct.unpack("<IH", f.read(6))
        if magic != TASH_MAGIC or version not in (2, 3, 4, 5):
            raise ValueError("Not a valid .tash file")
        f.seek(0)
        hdr_map = {
            2: HEADER_STRUCT,
            3: HEADER_V3_STRUCT,
            4: HEADER_V4_STRUCT,
            5: HEADER_V5_STRUCT,
        }
        hdr_s = hdr_map[version]
        unpacked = hdr_s.unpack(f.read(hdr_s.size))
        if version == 4:
            weights_off = unpacked[-4]
            weights_count = unpacked[-3]
        else:
            weights_off = unpacked[-2]
            weights_count = unpacked[-1]
        if len(weights) != weights_count:
            raise ValueError(f"Expected {weights_count} weights, got {len(weights)}")
        f.seek(weights_off)
        f.write(struct.pack(f"<{weights_count}i", *[int(w) for w in weights]))
