class _Rawi(object):
    """Rawi ensemble diacritizer, run in a separate process (rawi_host.exe).

    NVDA never loads numpy or onnxruntime: other add-ons ship their own copies,
    and two copies of those native libraries in one process can hang the whole
    screen reader. The helper is started on first use, every call has a time
    limit, and if the helper hangs or dies it is killed and restarted later,
    while the text is spoken without added diacritics. So speech can never block."""

    TIMEOUT = 3.0            # first call (the model is still loading); 1 s afterwards

    def __init__(self, onnx_path):
        import threading
        vocab_path = os.path.join(os.path.dirname(onnx_path), "vocab.json")
        with open(vocab_path, "r", encoding="utf-8") as f:
            vocab = json.load(f)
        self.char_to_idx = vocab["char_to_idx"]
        self.diac_to_idx = vocab["diac_to_idx"]
        self.idx_to_diac = {v: k for k, v in self.diac_to_idx.items()}
        self._unk_id = self.char_to_idx.get("<UNK>", 1)
        self._model = onnx_path
        self._proc = None
        self._lock = threading.Lock()
        self._fails = 0
        self._res_cache = None
        self._start()

    def _paths(self):
        base = os.path.dirname(os.path.dirname(os.path.abspath(self._model)))   # arabic/lib
        if os.name == "nt":
            return (os.path.join(base, "rawi_host.exe"),
                    os.path.join(base, "onnxruntime", "capi", "onnxruntime.dll"))
        return (os.path.join(base, "rawi_host"),
                os.environ.get("RAWI_ORT_LIB", "libonnxruntime.so"))

    def _start(self):
        import subprocess
        exe, lib = self._paths()
        flags = 0x08000000 if os.name == "nt" else 0          # CREATE_NO_WINDOW
        self._proc = subprocess.Popen(
            [exe, lib, self._model], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, creationflags=flags, bufsize=0)

    def _kill(self):
        self._warm = False
        p, self._proc = self._proc, None
        if p is not None:
            try:
                p.kill()
            except Exception:
                pass

    def _exchange(self, payload, n):
        """Send one request and read the reply on a helper thread, with a time limit."""
        import threading
        import struct
        box = {}
        proc = self._proc

        def io():
            try:
                proc.stdin.write(payload)
                proc.stdin.flush()
                head = self._read(proc.stdout, 4)
                (m,) = struct.unpack("<I", head)
                if m == 0xFFFFFFFF or m != n:
                    box["err"] = True
                    return
                box["res"] = struct.unpack("<%di" % n, self._read(proc.stdout, 4 * n)) if n else ()
            except Exception:
                box["err"] = True
        t = threading.Thread(target=io, daemon=True)
        t.start()
        t.join(self.TIMEOUT if not getattr(self, "_warm", False) else 1.0)
        if t.is_alive() or "res" not in box:
            self._kill()                  # hung or broken: drop it, the io thread ends with the pipe
            return None
        return box["res"]

    @staticmethod
    def _read(f, n):
        buf = b""
        while len(buf) < n:
            chunk = f.read(n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return buf

    def _classes(self, ids):
        import struct
        with self._lock:
            if self._proc is None or self._proc.poll() is not None:
                if self._fails >= 5:
                    return None          # helper keeps failing: speak without added diacritics
                try:
                    self._start()
                except Exception:
                    self._fails += 1
                    return None
            res = self._exchange(struct.pack("<I%dq" % len(ids), len(ids), *ids), len(ids))
            if res is None:
                self._fails += 1
            else:
                self._fails = 0
                self._warm = True
            return res

    def _encode(self, text):
        ids = []
        for ch in text:
            idx = self.char_to_idx.get(ch)
            if idx is None:
                nfc = unicodedata.normalize("NFC", ch)
                idx = self.char_to_idx.get(nfc, self._unk_id)
            ids.append(idx)
        return ids

    def _decode(self, base, cls):
        out = []
        for i, ch in enumerate(base):
            out.append(ch)
            if i < len(cls):
                d = int(cls[i])
                if d:
                    s = self.idx_to_diac.get(d, "")
                    if s:
                        out.append(s)
        return "".join(out)

    def __call__(self, text):
        if not text or not text.strip():
            return text
        cache = self._res_cache
        if cache is None:
            from collections import OrderedDict
            cache = self._res_cache = OrderedDict()
        hit = cache.get(text)
        if hit is not None:
            cache.move_to_end(text)
            return hit
        stripped = _strip(unicodedata.normalize("NFD", text))
        if not stripped:
            return text
        ids = self._encode(stripped)
        if not ids:
            return text
        cls = self._classes(ids)
        if cls is None:
            return text
        result = _restore_hamza(text, self._decode(stripped, cls))
        if len(text) <= 200:
            cache[text] = result
            while len(cache) > 512:
                cache.popitem(last=False)
        return result


