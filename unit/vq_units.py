"""Product vector quantization of a voice's join features (unit/vq_units.py).

usage: vq_units.py VOICE_DIR OUT_UNITS_BIN M        (testing: writes a units.bin with quantized features)
       vq_units.py --pack UNITS_BIN M  > codes      (used by make_packs.py through vq_pack())
The (jd+1) join features of every unit edge are split into M groups; each group is replaced by the
nearest of 256 entries of a codebook learnt on this voice (k-means), so an edge costs M bytes instead
of 2*(jd+1). Only the search uses these numbers (to judge joins); the audio is untouched.
"""
import struct, sys
import numpy as np

def load(path):
    d = open(path, "rb").read()
    assert d[:8] == b"FSUV0003"
    nu, nutt, jd, nc, sr = struct.unpack_from("<IIIII", d, 8); D = jd + 1
    rec = 16 + nc + 2 * D; h = 28 + 4 * jd + 20 * nutt + (nu + 7) // 8
    return d, nu, D, nc, rec, h

def quantize(X, M, seed=0):
    from sklearn.cluster import MiniBatchKMeans
    groups = np.array_split(np.arange(X.shape[1]), M); books = []; codes = []
    for g in groups:
        km = MiniBatchKMeans(256, batch_size=8192, n_init=3, max_iter=100, random_state=seed).fit(X[::2][:, g])
        books.append(km.cluster_centers_.astype(np.float16)); codes.append(km.predict(X[:, g]).astype(np.uint8))
    return groups, books, np.stack(codes, 1)

def decode(groups, books, codes, D):
    Y = np.zeros((len(codes), D), np.float16)
    for gi, g in enumerate(groups): Y[:, g] = books[gi][codes[:, gi]]
    return Y

if __name__ == "__main__":
    vd, out, M = sys.argv[1], sys.argv[2], int(sys.argv[3])
    d, nu, D, nc, rec, h = load(vd + "/units.bin")
    fx = np.frombuffer(d, np.uint8, nu * rec, h).reshape(nu, rec).copy()
    jl = fx[:, 16 + nc:].copy().view("<f2").reshape(nu, D)
    rest = np.frombuffer(d, "<f2", offset=h + nu * rec).reshape(-1, D)
    X = np.concatenate([jl, rest]).astype(np.float32)
    groups, books, codes = quantize(X, M)
    Y = decode(groups, books, codes, D)
    fx[:, 16 + nc:] = Y[:nu].view(np.uint8).reshape(nu, 2 * D)
    open(out, "wb").write(d[:h] + fx.tobytes() + Y[nu:].tobytes())
    err = np.sqrt(((Y.astype(np.float32) - X) ** 2).mean(0) / (X.var(0) + 1e-12)).mean()
    print("M=%d: relative error %.3f" % (M, err))
