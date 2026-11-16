"""hts_edit.py IN OUT [--dur S] [--level DB] [--eq MC.npy]
Match a finished Fasih-format voice (one whose training data we do not have) to the others:
speaking rate (every duration model except pauses scaled by S), level (c0 + DB/8.686) and
tone (a mel-cepstral filter added to every spectrum model, c0 left alone)."""
import sys, re, struct, argparse
import numpy as np

NS = 5


def load(path):
    d = open(path, "rb").read()
    i = d.index(b"[DATA]\n") + len(b"[DATA]\n")
    head = d[:i].decode("latin1")
    pos = {}
    for line in head[head.index("[POSITION]"):].splitlines()[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            pos[k] = [tuple(map(int, r.split("-"))) for r in v.split(",")]
    return head, bytearray(d[i:]), pos


def pau_leaves(tree_text):
    """Leaves a pause can reach: questions on the current phone follow the pause's answer,
    questions on anything else are followed both ways."""
    qs = {}
    for q, body in re.findall(r'QS (Q\d+) \{ ([^}]*) \}', tree_text):
        pats = re.findall(r'"([^"]*)"', body)
        names = [re.fullmatch(r"\*-([^+]+)\+\*", x) for x in pats]
        qs[q] = ({m.group(1) for m in names} if all(names) else None)
    nodes = {}
    for m in re.finditer(r'^\s*(-?\d+) (Q\d+) (\S+) (\S+)', tree_text, re.M):
        nodes[m.group(1)] = (m.group(2), m.group(3), m.group(4))
    out = set()
    def walk(ref):
        if ref.startswith('"'):
            out.add(int(ref.strip('"').rsplit("_", 1)[1]) - 1); return
        q, no, yes = nodes[ref]
        cls = qs.get(q)
        if cls is None:
            walk(no); walk(yes)
        else:
            walk(yes if "pau" in cls else no)
    if nodes:
        walk("0")
    return out


def edit(head, data, pos, dur=1.0, level=0.0, eq=None):
    if dur != 1.0:
        a, b = pos["DURATION_PDF"][0]
        n = struct.unpack_from("<I", data, a)[0]
        P = np.frombuffer(bytes(data[a + 4:b + 1]), "<f4").reshape(n, 2 * NS).copy()
        t0, t1 = pos["DURATION_TREE"][0]
        skip = pau_leaves(bytes(data[t0:t1 + 1]).decode("utf-8"))
        keep = np.array([i not in skip for i in range(n)])
        P[keep, :NS] = np.maximum(P[keep, :NS] * dur, 1.0)
        data[a + 4:b + 1] = P.astype("<f4").tobytes()
    if level or eq is not None:
        a, b = pos["STREAM_PDF[MCP]"][0]
        D = int(re.search(r"VECTOR_LENGTH\[MCP\]:(\d+)", head).group(1))
        P = np.frombuffer(bytes(data[a + 4 * NS:b + 1]), "<f4").reshape(-1, 6 * D).astype(np.float64)
        if level:
            P[:, 0] += level / 8.686
        if eq is not None:
            e = np.asarray(eq, np.float64)[:D].copy(); e[0] = 0.0
            P[:, :len(e)] += e
        data[a + 4 * NS:b + 1] = P.astype("<f4").tobytes()
    return head.encode("latin1") + bytes(data)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("inp"); ap.add_argument("out")
    ap.add_argument("--dur", type=float, default=1.0); ap.add_argument("--level", type=float, default=0.0)
    ap.add_argument("--eq")
    a = ap.parse_args()
    h, d, p = load(a.inp)
    open(a.out, "wb").write(edit(h, d, p, a.dur, a.level, np.load(a.eq) if a.eq else None))
