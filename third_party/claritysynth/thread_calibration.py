# -*- coding: utf-8 -*-
"""Per-model thread calibration for ClaritySynth.

Different ONNX models run fastest at different intra-op thread counts, and the
sweet spot depends on the user's CPU. Small TTS/diacritizer models are usually
fastest on ONE thread (scheduling overhead dominates), but a larger acoustic
model can benefit from two. Rather than guess, this module can BENCHMARK each
installed model on the user's own machine and remember the fastest setting.

Three things are exposed to the UI:
  * calibrate_all(progress) — measure every model, store the winners, return a
    short human-readable report (realtime factor per model).
  * per-model overrides are persisted to a small JSON file that survives
    updates, and consulted by ort_config via get_override().
  * OUR_DEFAULTS — the shipped defaults; restore_defaults() clears every
    user/calibrated override so the add-on returns to exactly these.

Nothing here ever runs on NVDA's GUI thread — calibration is triggered
explicitly by a button and runs on a worker thread.
"""

import os
import json
import time

try:
    from logHandler import log
except Exception:                       # pragma: no cover
    class _L:
        def __getattr__(self, n):
            return lambda *a, **k: None
    log = _L()


# ClaritySynth's shipped defaults: model-kind -> intra_op thread count. These
# were chosen by benchmarking (see the analysis in the 2.5 notes): every model
# here already runs 20-50x faster than realtime on ONE thread, and adding
# threads only raises per-call latency and steals cores from NVDA and the rest
# of the system for no perceptible gain. So one thread per model is both the
# fastest for latency and the kindest to overall responsiveness. The calibrate
# button lets a user verify the best value on their own CPU; "restore" returns
# to exactly these.
OUR_DEFAULTS = {
    "mixer": 1,        # the Arabic acoustic model (still ~50x realtime at 1)
    "piper": 1,        # Piper / RT split voices
    "glowtts": 1,      # GlowTTS acoustic + vocoder
    "kokoro": 1,       # Kokoro English
    "tashkeel": 1,     # the diacritizer
}

# thread counts we try during calibration
_CANDIDATES = (1, 2, 3, 4)


def _store_path():
    try:
        from . import data_paths
    except Exception:
        import data_paths
    base = data_paths.user_data_dir() if hasattr(
        data_paths, "user_data_dir") else None
    if not base:
        # fall back to the config-derived dir data_paths already uses
        try:
            base = data_paths._base()
        except Exception:
            base = None
    if not base:
        return None
    return os.path.join(base, "thread_overrides.json")


def _load_overrides():
    p = _store_path()
    if not p or not os.path.exists(p):
        return {}
    try:
        with open(p, "r", encoding="utf-8") as f:
            d = json.load(f)
        if isinstance(d, dict):
            return {k: int(v) for k, v in d.items() if str(v).isdigit()}
    except Exception:
        pass
    return {}


def _save_overrides(d):
    p = _store_path()
    if not p:
        return False
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            json.dump(d, f)
        return True
    except Exception:
        return False


# cached view so ort_config can call get_override() cheaply per session build
_cache = None


def get_override(kind):
    """Return the calibrated/overridden thread count for a model kind, or None
    to mean 'use the shipped default'. Consulted by ort_config."""
    global _cache
    if _cache is None:
        _cache = _load_overrides()
    v = _cache.get(kind)
    try:
        return int(v) if v else None
    except Exception:
        return None


def set_override(kind, threads):
    """Manually set (advanced users) the thread count for a model kind."""
    global _cache
    ov = _load_overrides()
    if threads:
        ov[kind] = int(threads)
    else:
        ov.pop(kind, None)
    _save_overrides(ov)
    _cache = ov
    _reset_engines()


def _reset_engines():
    """Ask every engine to drop its cached ONNX sessions so a changed thread
    setting takes effect on the next synth (instead of waiting for a restart).
    Best-effort and silent."""
    for mod_name in ("piper_neural", "kokoro_neural", "glowtts_neural",
                     "ar_tashkeel"):
        try:
            try:
                mod = __import__(mod_name, globals(), locals(), [], 1)
            except Exception:
                mod = __import__(mod_name)
            if hasattr(mod, "reset_sessions"):
                mod.reset_sessions()
        except Exception:
            pass


def restore_defaults():
    """Clear ALL overrides so every model reverts to OUR_DEFAULTS."""
    global _cache
    _save_overrides({})
    _cache = {}
    _reset_engines()

# --- Benchmarking by driving the REAL engines --------------------------------
# Rather than reconstruct each model's exact input tensors (fragile, and there
# are many: mixer128/mixer80/vocoders, six tashkeel libraries, Piper/RT,
# GlowTTS, Kokoro), we time each engine THE WAY IT IS ACTUALLY USED: force a
# thread count, drop cached sessions, run one real synth/diacritize call, and
# measure. This automatically covers every installed model, bundled or
# downloaded, and never benchmarks something that is not actually present.

# thread counts to try; kept small so calibration finishes in a few seconds
_TRY_THREADS = (1, 2, 4)

# a short, representative Arabic phrase (already diacritized for the TTS path)
_AR_DIAC = "\u0627\u0644\u0633\u0651\u064e\u0644\u0627\u0645\u064f "\
           "\u0639\u064e\u0644\u064e\u064a\u0652\u0643\u064f\u0645"
_AR_PLAIN = "\u0627\u0644\u0633\u0644\u0627\u0645 "\
            "\u0639\u0644\u064a\u0643\u0645 \u0648\u0631\u062d\u0645\u0629 "\
            "\u0627\u0644\u0644\u0647"
_EN_TEXT = "This is a short speed test."


def _import(name):
    try:
        return __import__(name, globals(), locals(), [], 1)
    except Exception:
        try:
            return __import__(name)
        except Exception:
            return None


def _time_call(fn, runs=6):
    """Median seconds for `fn`, or None if it errors / produces nothing."""
    try:
        # warm
        for _ in range(2):
            fn()
        times = []
        for _ in range(runs):
            t0 = time.time()
            fn()
            times.append(time.time() - t0)
        times.sort()
        return times[len(times) // 2]
    except Exception:
        return None


def _calibrate_kind(kind, call_builder, progress=None):
    """Try each thread count for one model kind, keep the fastest. Returns
    (best_threads, best_seconds) or None if the kind is not usable.

    Tie-breaking favours FEWER threads: fewer threads means lower latency to
    first audio and more free cores for NVDA, so we only pick a higher thread
    count when it is clearly (>10%) faster, never on measurement noise."""
    best = None
    for nt in _TRY_THREADS:
        set_override(kind, nt)
        call = call_builder()
        if call is None:
            restore_for(kind)
            return None
        sec = _time_call(call)
        if sec is None:
            continue
        if best is None:
            best = (nt, sec)
        else:
            # only switch to more threads if meaningfully (>10%) faster
            if sec < best[1] * 0.90:
                best = (nt, sec)
    return best


def restore_for(kind):
    """Remove a single kind's override (used when a kind turns out unusable)."""
    global _cache
    ov = _load_overrides()
    ov.pop(kind, None)
    _save_overrides(ov)
    _cache = ov


def _installed_mixer_models():
    """Names of the Arabic acoustic models actually present (mixer128,
    mixer80, fastpitch)."""
    found = []
    tts = _import("tts_neural")
    vp = _import("voice_packs")
    names = {"mixer128": "mixer128.onnx", "mixer80": "mixer80.onnx",
             "fastpitch": "fp_ms.onnx"}
    dirs = []
    try:
        if vp:
            import os as _os
            for d in vp._iter_dirs(_os.path.join("tts_arabic", "data")):
                dirs.append(d)
    except Exception:
        pass
    for mid, fn in names.items():
        for d in dirs:
            try:
                import os as _os
                if _os.path.exists(_os.path.join(d, fn)):
                    found.append(mid)
                    break
            except Exception:
                pass
    return found


def calibrate_all(progress=None):
    """Benchmark every installed model — the Arabic acoustic models
    (mixer128/mixer80/fastpitch), each installed tashkeel library
    (libtashkeel, rawi, catt, catt_ed, shakkala, shakkelha), the selected
    Piper/RT voice, GlowTTS, and Kokoro — at several thread counts on THIS
    CPU, and store the fastest setting for each. Returns a human-readable
    report. Runs on a worker thread."""

    def say(s):
        if progress:
            try:
                progress(s)
            except Exception:
                pass

    report = []
    winners = {}

    # ---- Arabic acoustic model(s): mixer128 / mixer80 / fastpitch ----
    tts = _import("tts_neural")
    if tts is not None:
        mixers = _installed_mixer_models() or []
        # benchmark the currently-selected model under the shared "mixer" kind
        say("Testing Arabic voice model...")

        def mixer_call():
            try:
                if not tts._try_init():
                    return None
            except Exception:
                return None

            def run():
                tts.synth_wave(_AR_DIAC, speaker=0, pace=1.0)
            return run
        best = _calibrate_kind("mixer", lambda: mixer_call(), progress)
        if best:
            winners["mixer"] = best[0]
            dur = 3.0
            report.append(
                "Arabic voice (%s): fastest on %d thread(s)"
                % (", ".join(mixers) if mixers else "mixer", best[0]))

    # ---- tashkeel: benchmark the diacritizer (all libraries share one
    # thread setting, applied via kind="tashkeel"). We time each INSTALLED
    # library so the report is informative, but store a single winner under
    # "tashkeel" (the fastest observed), matching how the setting is applied. -
    at = _import("ar_tashkeel")
    if at is not None:
        try:
            libs = [n for n in at.available() if n != "off"]
        except Exception:
            libs = []
        best_overall = None
        for lib in libs:
            say("Testing diacritizer: %s..." % lib)

            def lib_call(lib=lib):
                fn = None
                try:
                    fn = at._get(lib)
                except Exception:
                    fn = None
                if not fn:
                    return None
                ctr = [0]

                def run():
                    ctr[0] += 1
                    fn(_AR_PLAIN + str(ctr[0]))
                return run
            best = _calibrate_kind("tashkeel", lambda lib=lib: lib_call(lib),
                                   progress)
            if best:
                report.append("Diacritizer %s: fastest on %d thread(s)"
                              % (lib, best[0]))
                if best_overall is None or best[1] < best_overall[1]:
                    best_overall = best
        if best_overall:
            winners["tashkeel"] = best_overall[0]

    # ---- Piper / RT voice (the selected or first installed) ----
    pn = _import("piper_neural")
    if pn is not None:
        say("Testing Piper / RT voice...")

        def piper_call():
            try:
                onnx, cfg, dec = pn._find_voice()
            except Exception:
                onnx = None
            if not onnx:
                return None

            def run():
                pn.synth_wave(_EN_TEXT)
            return run
        best = _calibrate_kind("piper", lambda: piper_call(), progress)
        if best:
            winners["piper"] = best[0]
            report.append("Piper / RT voices: fastest on %d thread(s)"
                          % best[0])

    # ---- GlowTTS (if any installed) ----
    gt = _import("glowtts_neural")
    if gt is not None:
        try:
            avail = gt.available_voices()
        except Exception:
            avail = []
        if avail:
            say("Testing GlowTTS...")
            gt.select_voice(avail[0][0])

            def glow_call():
                def run():
                    gt.synth_wave("Merhaba, bir test.")
                return run
            best = _calibrate_kind("glowtts", lambda: glow_call(), progress)
            if best:
                winners["glowtts"] = best[0]
                report.append("GlowTTS: fastest on %d thread(s)" % best[0])

    # ---- Kokoro (if installed) ----
    kk = _import("kokoro_neural")
    if kk is not None:
        try:
            ok = kk.is_available()
        except Exception:
            ok = False
        if ok:
            say("Testing Kokoro...")

            def kok_call():
                def run():
                    kk.synth_wave(_EN_TEXT, length_scale=1.0, volume=1.0)
                return run
            best = _calibrate_kind("kokoro", lambda: kok_call(), progress)
            if best:
                winners["kokoro"] = best[0]
                report.append("Kokoro English: fastest on %d thread(s)"
                              % best[0])

    # persist winners (already written per-kind by set_override during the
    # sweep; make sure the FINAL best is what stays)
    if winners:
        ov = _load_overrides()
        ov.update({k: int(v) for k, v in winners.items()})
        _save_overrides(ov)
        global _cache
        _cache = ov
    _reset_engines()

    if not report:
        return ("No models could be benchmarked. Install at least one voice "
                "or diacritization library, then try again.")
    header = "Calibration complete. Settings saved for this computer:\n\n"
    return header + "\n".join(report)
