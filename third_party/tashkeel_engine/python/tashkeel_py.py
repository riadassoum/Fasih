import ctypes
import os
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from typing import List, Optional, Sequence


class TashkeelEngine:
    """Thin ctypes wrapper around libbarq.so / barq.dll for ultra-fast Arabic diacritization."""

    def __init__(self, model_path: str, lib_path: Optional[str] = None):
        if lib_path is None:
            here = os.path.dirname(os.path.abspath(__file__))
            root = os.path.normpath(os.path.join(here, ".."))
            candidates = (
                [os.path.join(root, "windows", "barq.dll"), os.path.join(root, "build", "windows", "barq.dll")]
                if sys.platform == "win32"
                else [
                    os.path.join(root, "linux", "libbarq.so"),
                    os.path.join(root, "build", "linux", "libbarq.so"),
                    os.path.join(root, "build", "libtashkeel.so"),
                ]
            )
            lib_path = next((p for p in candidates if os.path.exists(p)), candidates[-1])
        self._lib = ctypes.CDLL(lib_path)

        self._lib.tashkeel_create.argtypes = [ctypes.c_char_p]
        self._lib.tashkeel_create.restype = ctypes.c_void_p

        self._lib.tashkeel_destroy.argtypes = [ctypes.c_void_p]
        self._lib.tashkeel_destroy.restype = None

        self._lib.tashkeel_diacritize.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        self._lib.tashkeel_diacritize.restype = ctypes.c_void_p

        self._has_into = hasattr(self._lib, "tashkeel_diacritize_into")
        if self._has_into:
            self._lib.tashkeel_diacritize_into.argtypes = [
                ctypes.c_void_p,
                ctypes.c_char_p,
                ctypes.c_size_t,
                ctypes.c_char_p,
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_size_t),
            ]
            self._lib.tashkeel_diacritize_into.restype = ctypes.c_int

        self._lib.tashkeel_free_str.argtypes = [ctypes.c_void_p]
        self._lib.tashkeel_free_str.restype = None

        self._lib.tashkeel_vocab_size.argtypes = [ctypes.c_void_p]
        self._lib.tashkeel_vocab_size.restype = ctypes.c_uint32

        self._lib.tashkeel_get_weight.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self._lib.tashkeel_get_weight.restype = ctypes.c_int32

        self._lib.tashkeel_set_weight.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int32]
        self._lib.tashkeel_set_weight.restype = ctypes.c_int

        self._lib.tashkeel_set_restore_spelling.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self._lib.tashkeel_set_restore_spelling.restype = None

        self._tls = threading.local()
        self._handle = self._lib.tashkeel_create(model_path.encode("utf-8"))
        if not self._handle:
            raise RuntimeError(f"Failed to load model: {model_path}")

    def __del__(self):
        if getattr(self, "_handle", None):
            self._lib.tashkeel_destroy(self._handle)
            self._handle = None

    @property
    def vocab_size(self) -> int:
        return int(self._lib.tashkeel_vocab_size(self._handle))

    def get_weight(self, weight_id: int) -> int:
        return int(self._lib.tashkeel_get_weight(self._handle, weight_id))

    def set_weight(self, weight_id: int, value: int) -> bool:
        return bool(self._lib.tashkeel_set_weight(self._handle, weight_id, value))

    def set_restore_spelling(self, enabled: bool) -> None:
        self._lib.tashkeel_set_restore_spelling(self._handle, 1 if enabled else 0)

    def _get_tls_state(self):
        buf = getattr(self._tls, "buf", None)
        if buf is None:
            buf = ctypes.create_string_buffer(8192)
            self._tls.buf = buf
            self._tls.out_len = ctypes.c_size_t(0)
        return buf, self._tls.out_len

    def diacritize(self, text: str) -> str:
        raw = text.encode("utf-8")
        if self._has_into:
            buf, out_len = self._get_tls_state()
            cap = len(buf)
            needed = len(raw) * 4 + 32
            if needed > cap:
                buf = ctypes.create_string_buffer(needed)
                self._tls.buf = buf
                cap = needed
            ok = self._lib.tashkeel_diacritize_into(
                self._handle, raw, len(raw), buf, cap, ctypes.byref(out_len)
            )
            if ok:
                return buf.raw[: out_len.value].decode("utf-8")
            if out_len.value + 1 > cap:
                new_cap = out_len.value + 1024
                buf = ctypes.create_string_buffer(new_cap)
                self._tls.buf = buf
                ok = self._lib.tashkeel_diacritize_into(
                    self._handle, raw, len(raw), buf, new_cap, ctypes.byref(out_len)
                )
                if ok:
                    return buf.raw[: out_len.value].decode("utf-8")
        out_len = ctypes.c_size_t(0)
        ptr = self._lib.tashkeel_diacritize(self._handle, raw, len(raw), ctypes.byref(out_len))
        if not ptr:
            return text
        try:
            res_bytes = ctypes.string_at(ptr, out_len.value)
            return res_bytes.decode("utf-8")
        finally:
            self._lib.tashkeel_free_str(ptr)

    def diacritize_lines(self, lines: Sequence[str], threads: int = 1) -> List[str]:
        if threads <= 1 or len(lines) < 8:
            return [self.diacritize(l) for l in lines]
        with ThreadPoolExecutor(max_workers=threads) as ex:
            return list(ex.map(self.diacritize, lines, chunksize=max(1, len(lines) // (threads * 4))))
