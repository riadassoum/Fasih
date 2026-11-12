# -*- coding: utf-8 -*-
"""Shared ONNX Runtime configuration for ClaritySynth.

Two goals:

1.  **Do not freeze NVDA.** By default ONNX Runtime spawns one worker thread
    per CPU core and runs the graph in parallel. On a modest machine, loading
    and running a large neural TTS model then saturates every core, and
    because model loading holds CPython's GIL for long stretches the whole of
    NVDA (its input thread included) stalls — the user sees NVDA freeze and
    the modifier keys (NVDA/Ctrl/Insert/Caps) stick. Capping the thread count
    and using sequential execution keeps a core free for NVDA itself, which
    removes the freeze at a negligible cost to latency.

2.  **Allow the GPU when asked.** If the user turns on GPU acceleration and a
    CUDA or DirectML execution provider is available, prefer it — a good
    NVIDIA card brings neural latency down close to the classic engines. If
    the provider is missing we silently fall back to CPU, so enabling the
    option can never break speech.

Everything here fails soft: any error returns plain defaults so the model
still loads.
"""

import os
import threading

_lock = threading.Lock()
_use_gpu = False          # set from the driver's "GPU acceleration" setting
_cpu_threads = None       # None => auto (a sensible cap based on cpu count)


def set_gpu(enabled):
    global _use_gpu
    _use_gpu = bool(enabled)


def get_gpu():
    return _use_gpu


def set_cpu_threads(n):
    global _cpu_threads
    try:
        _cpu_threads = int(n) if n else None
    except Exception:
        _cpu_threads = None


def _thread_cap(small_model=False, kind=None):
    """How many intra-op threads to allow.

    Priority: an explicit global override (set_cpu_threads) wins; then a
    per-model calibrated/advanced override for `kind` (from thread_calibration)
    wins; otherwise fall back to the shipped default — 1 thread for the small
    models, 2 for the larger acoustic model.

    small_model=True is for the small, fast models — Piper voices, the split
    RT encoder/decoder, GlowTTS, Kokoro, and the diacritizer. Benchmarking
    shows these are FASTEST on a single thread: they are small enough that
    thread-scheduling overhead outweighs any parallelism, so 2 threads is
    ~2x SLOWER than 1. Only the larger Mixer acoustic model benefits from a
    second thread."""
    if _cpu_threads:
        return max(1, _cpu_threads)
    # per-model calibrated / advanced override
    if kind:
        try:
            try:
                from . import thread_calibration
            except Exception:
                import thread_calibration
            ov = thread_calibration.get_override(kind)
            if ov:
                return max(1, int(ov))
        except Exception:
            pass
    if small_model:
        return 1
    try:
        cores = os.cpu_count() or 2
    except Exception:
        cores = 2
    # Even the larger Mixer acoustic model is ~50x realtime on ONE thread, and
    # extra threads only add latency to the critical first-audio call and take
    # cores from NVDA. Benchmarking showed no case where more than one thread
    # helped a screen-reader workload, so default to one everywhere.
    return 1


def make_session_options(small_model=False, kind=None):
    """A SessionOptions tuned to keep NVDA responsive. small_model=True picks
    the single-thread default for the small TTS/diacritizer models; `kind`
    (e.g. "piper", "mixer", "glowtts", "kokoro", "tashkeel") lets a calibrated
    per-model override take effect. Fails soft to None."""
    try:
        import onnxruntime as ort
        so = ort.SessionOptions()
        n = _thread_cap(small_model=small_model, kind=kind)
        so.intra_op_num_threads = n
        so.inter_op_num_threads = 1
        try:
            so.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        except Exception:
            pass
        try:
            so.graph_optimization_level = \
                ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        except Exception:
            pass
        return so
    except Exception:
        return None


def gpu_available():
    """True only if onnxruntime actually has a GPU execution provider. The
    bundled onnxruntime is the CPU-only build, so this is False unless the
    user has installed an onnxruntime-gpu / DirectML build. Used to tell the
    user the truth instead of letting the GPU switch silently do nothing."""
    try:
        import onnxruntime as ort
        avail = list(ort.get_available_providers())
        return ("CUDAExecutionProvider" in avail
                or "DmlExecutionProvider" in avail)
    except Exception:
        return False


def providers():
    """The execution-provider list to try, GPU first when enabled and
    available, always with a CPU fallback so speech cannot break."""
    try:
        import onnxruntime as ort
        avail = list(ort.get_available_providers())
    except Exception:
        return None
    provs = []
    if _use_gpu:
        # prefer CUDA (NVIDIA), then DirectML (any DirectX 12 GPU on Windows)
        if "CUDAExecutionProvider" in avail:
            provs.append("CUDAExecutionProvider")
        if "DmlExecutionProvider" in avail:
            provs.append("DmlExecutionProvider")
    provs.append("CPUExecutionProvider")
    return provs


def make_session(path, extra_providers=None, kind="mixer"):
    """Build an InferenceSession for a model file with the tuned options and
    provider list. Retries on CPU if a GPU provider fails to initialise, so a
    driver/runtime mismatch degrades to CPU instead of breaking speech. `kind`
    selects the per-model thread setting (defaults to "mixer", the large
    acoustic model, since that is what calls this helper)."""
    import onnxruntime as ort
    so = make_session_options(kind=kind)
    provs = providers()
    if provs is None:
        # last resort: library defaults
        return ort.InferenceSession(path)
    try:
        if so is not None:
            return ort.InferenceSession(path, sess_options=so,
                                        providers=provs)
        return ort.InferenceSession(path, providers=provs)
    except Exception:
        # GPU provider may be listed but fail to load (missing CUDA DLLs,
        # unsupported op); fall back to CPU only.
        try:
            if so is not None:
                return ort.InferenceSession(
                    path, sess_options=so,
                    providers=["CPUExecutionProvider"])
            return ort.InferenceSession(
                path, providers=["CPUExecutionProvider"])
        except Exception:
            return ort.InferenceSession(path)
