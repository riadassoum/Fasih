import concurrent.futures
import ctypes
import os
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ENGINE_DIR = os.path.dirname(SCRIPT_DIR)
ROOT_DIR = os.path.dirname(os.path.dirname(ENGINE_DIR))
sys.path.insert(0, os.path.join(ENGINE_DIR, "python"))

from arabic_utils import compute_metrics, extract_arabic_words, strip_diacritics


class FastEngine:
    def __init__(self, model_path: str, workers: int = 4):
        candidates = [
            os.path.join(ENGINE_DIR, "linux", "libbarq.so"),
            os.path.join(ENGINE_DIR, "build", "libbarq.so"),
            os.path.join(ENGINE_DIR, "windows", "barq.dll"),
            os.path.join(ENGINE_DIR, "build", "libtashkeel.so"),
        ]
        lib_path = next((c for c in candidates if os.path.exists(c)), candidates[0])
        self._lib = ctypes.CDLL(lib_path)
        self._lib.barq_create.argtypes = [ctypes.c_char_p]
        self._lib.barq_create.restype = ctypes.c_void_p
        self._lib.barq_destroy.argtypes = [ctypes.c_void_p]
        self._lib.barq_diacritize.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        self._lib.barq_diacritize.restype = ctypes.c_void_p
        self._lib.barq_free_str.argtypes = [ctypes.c_void_p]
        self._lib.tashkeel_get_weight.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self._lib.tashkeel_get_weight.restype = ctypes.c_int32
        self._lib.tashkeel_set_weight.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int32]
        self._lib.tashkeel_set_weight.restype = ctypes.c_int
        self._h = self._lib.barq_create(model_path.encode("utf-8"))
        if not self._h:
            raise RuntimeError(f"Failed to load model: {model_path}")
        self._pool = concurrent.futures.ThreadPoolExecutor(max_workers=workers)

    def _dia_chunk(self, lines):
        fn = self._lib.barq_diacritize
        free_fn = self._lib.barq_free_str
        h = self._h
        out = []
        for s in lines:
            b = s.encode("utf-8")
            n = ctypes.c_size_t(0)
            p = fn(h, b, len(b), ctypes.byref(n))
            if not p:
                out.append(s)
            else:
                out.append(ctypes.string_at(p, n.value).decode("utf-8", errors="replace"))
                free_fn(p)
        return out

    def diacritize_lines(self, lines):
        if len(lines) < 64:
            return self._dia_chunk(lines)
        n_workers = self._pool._max_workers
        step = (len(lines) + n_workers - 1) // n_workers
        chunks = [lines[i : i + step] for i in range(0, len(lines), step)]
        res = []
        for part in self._pool.map(self._dia_chunk, chunks):
            res.extend(part)
        return res

    def close(self):
        self._pool.shutdown(wait=True)
        if self._h:
            self._lib.barq_destroy(self._h)
            self._h = None


def load_lines(path, max_lines=50000):
    res = []
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            res.append(line)
            if len(res) >= max_lines:
                break
    return res


def eval_set(eng, name, path, max_lines=50000):
    gl = load_lines(path, max_lines)
    pl = [strip_diacritics(l) for l in gl]
    t0 = time.perf_counter()
    preds = eng.diacritize_lines(pl)
    dt = time.perf_counter() - t0
    m = compute_metrics(gl, preds)
    wer_nc_err, tot_w = 0, 0
    for g, p in zip(gl, preds):
        gw, pw = extract_arabic_words(g), extract_arabic_words(p)
        if len(gw) != len(pw):
            continue
        for (gu, _, gd), (pu, _, pd) in zip(gw, pw):
            if gu != pu or len(gd) != len(pd) or not gd:
                continue
            tot_w += 1
            if gd[:-1] != pd[:-1]:
                wer_nc_err += 1
    wps = m["total_words"] / max(dt, 1e-9)
    print(
        f"{name:18s} | N={len(gl):5d} W={m['total_words']:6d} | "
        f"DER={m['der_pct']:.2f}% WER={m['wer_pct']:.2f}% "
        f"WER_nc={100.0*wer_nc_err/max(1,tot_w):.2f}% Irab={m['irab_err_pct']:.2f}% | "
        f"{dt:.2f}s ({wps:,.0f} w/s total)"
    )


def eval_mr(eng, name, path):
    gl = load_lines(path, 50000)
    plain_lines = []
    split_toks = []
    for line in gl:
        toks = line.split()
        split_toks.append(toks)
        plain_lines.append(" ".join(strip_diacritics(t.split("/")[0]) for t in toks))
    t0 = time.perf_counter()
    pred_lines = eng.diacritize_lines(plain_lines)
    dt = time.perf_counter() - t0
    tot_c = err_c = tot_w = err_w = err_w_nc = tot_e = err_e = 0
    for toks, pred_line in zip(split_toks, pred_lines):
        p_toks = pred_line.split()
        if len(p_toks) != len(toks):
            continue
        for g_tok, p_tok in zip(toks, p_toks):
            pw = extract_arabic_words(p_tok)
            best = None
            for alt in g_tok.split("/"):
                gw = extract_arabic_words(alt)
                if len(gw) != len(pw):
                    continue
                ec = ee = ew = ew_nc = tc = tw = te = 0
                ok = True
                for (gu, _, gd), (pu, _, pd) in zip(gw, pw):
                    if gu != pu or len(gd) != len(pd):
                        ok = False
                        break
                    tw += 1
                    if gd != pd:
                        ew += 1
                    if gd[:-1] != pd[:-1]:
                        ew_nc += 1
                    for a, b in zip(gd, pd):
                        tc += 1
                        if a != b:
                            ec += 1
                    if gd:
                        te += 1
                        if gd[-1] != pd[-1]:
                            ee += 1
                if ok and (best is None or (ec, ee, ew) < (best[0], best[1], best[2])):
                    best = (ec, ee, ew, ew_nc, tc, tw, te)
            if best:
                tot_c += best[4]
                err_c += best[0]
                tot_w += best[5]
                err_w += best[2]
                err_w_nc += best[3]
                tot_e += best[6]
                err_e += best[1]
    print(
        f"{name:18s} | N={len(gl):5d} W={tot_w:6d} | "
        f"DER={100*err_c/max(1,tot_c):.2f}% WER={100*err_w/max(1,tot_w):.2f}% "
        f"WER_nc={100*err_w_nc/max(1,tot_w):.2f}% Irab={100*err_e/max(1,tot_e):.2f}% | {dt:.2f}s"
    )


def main():
    model_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ENGINE_DIR, "models", "barq.tash")
    workers = min(8, max(2, (os.cpu_count() or 4)))
    eng = FastEngine(model_path, workers=workers)
    bench_dir = os.path.join(ROOT_DIR, "tashkeel_bench")
    t_all = time.perf_counter()
    eval_set(eng, "WikiNews-2014", os.path.join(bench_dir, "wikinews_2014.txt"))
    eval_mr(eng, "WikiNews-2014-MR", os.path.join(bench_dir, "wikinews_2014_multiref.txt"))
    eval_mr(eng, "WikiNews-2024-MR", os.path.join(bench_dir, "wikinews_2024.txt"))
    eval_set(eng, "TigreVal-5k", os.path.join(bench_dir, "tigre_val_sample.txt"), 5000)
    eval_set(eng, "TigreTest-5k", os.path.join(bench_dir, "tigre_test_sample.txt"), 5000)
    print(f"Total 5-benchmark runtime: {time.perf_counter() - t_all:.2f}s (workers={workers})")
    eng.close()


if __name__ == "__main__":
    main()
