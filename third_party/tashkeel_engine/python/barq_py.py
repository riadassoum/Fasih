import ctypes
import os
import sys
from typing import Optional


class BarqEngine:
    """Thin ctypes wrapper around libbarq.so / barq.dll for fast Arabic diacritization."""

    def __init__(self, model_path: str, lib_path: Optional[str] = None):
        if lib_path is None:
            here = os.path.dirname(os.path.abspath(__file__))
            root = os.path.normpath(os.path.join(here, ".."))
            candidates = (
                [
                    os.path.join(root, "windows", "barq.dll"),
                    os.path.join(root, "build", "windows", "barq.dll"),
                ]
                if sys.platform == "win32"
                else [
                    os.path.join(root, "linux", "libbarq.so"),
                    os.path.join(root, "build", "linux", "libbarq.so"),
                    os.path.join(root, "build", "libbarq.so"),
                ]
            )
            lib_path = next((p for p in candidates if os.path.exists(p)), candidates[0])
        self._lib = ctypes.CDLL(lib_path)

        self._lib.barq_create.argtypes = [ctypes.c_char_p]
        self._lib.barq_create.restype = ctypes.c_void_p

        self._lib.barq_destroy.argtypes = [ctypes.c_void_p]
        self._lib.barq_destroy.restype = None

        self._lib.barq_diacritize.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        self._lib.barq_diacritize.restype = ctypes.c_void_p

        self._lib.barq_free_str.argtypes = [ctypes.c_void_p]
        self._lib.barq_free_str.restype = None

        self._lib.barq_vocab_size.argtypes = [ctypes.c_void_p]
        self._lib.barq_vocab_size.restype = ctypes.c_uint32

        self._lib.barq_get_weight.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self._lib.barq_get_weight.restype = ctypes.c_int32

        self._lib.barq_set_weight.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int32]
        self._lib.barq_set_weight.restype = ctypes.c_int

        self._lib.barq_set_restore_spelling.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self._lib.barq_set_restore_spelling.restype = None

        self._handle = self._lib.barq_create(model_path.encode("utf-8"))
        if not self._handle:
            raise RuntimeError(f"Failed to load model: {model_path}")

    def close(self) -> None:
        if getattr(self, "_handle", None):
            self._lib.barq_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "BarqEngine":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def __del__(self):
        self.close()

    @property
    def vocab_size(self) -> int:
        return int(self._lib.barq_vocab_size(self._handle))

    def get_weight(self, weight_id: int) -> int:
        return int(self._lib.barq_get_weight(self._handle, weight_id))

    def set_weight(self, weight_id: int, value: int) -> bool:
        return bool(self._lib.barq_set_weight(self._handle, weight_id, value))

    def set_restore_spelling(self, enabled: bool) -> None:
        self._lib.barq_set_restore_spelling(self._handle, 1 if enabled else 0)

    def diacritize(self, text: str) -> str:
        raw = text.encode("utf-8")
        out_len = ctypes.c_size_t(0)
        ptr = self._lib.barq_diacritize(self._handle, raw, len(raw), ctypes.byref(out_len))
        if not ptr:
            return text
        try:
            res_bytes = ctypes.string_at(ptr, out_len.value)
            return res_bytes.decode("utf-8")
        finally:
            self._lib.barq_free_str(ptr)
