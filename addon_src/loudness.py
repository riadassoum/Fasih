# Fasih output stage: DC/rumble high-pass (after PlanetBlindTech's post-filter), make-up
# gain and a look-ahead peak limiter. Pure Python (array module): NVDA never loads numpy.
#
# The limiter only turns the gain down and back up smoothly (2 ms look-ahead, about 60 ms
# release), so it never clips or reshapes the waveform. That lets Fasih play about 6 dB
# louder than before, so volume 100 is as loud as other screen-reader voices.
import array

BLOCK = 48                 # 2 ms at 24 kHz: look-ahead and gain-update step
CEIL = 0.97 * 32767        # highest sample value the limiter lets through
RELEASE = 1.024            # gain may rise by this factor per block (about 6 dB in 60 ms)
HP_POLE = 0.9965           # DC-blocking high-pass, same as PlanetBlindTech's filter
TRIM_LEVEL = 200           # about -44 dBFS: below this, the start of an utterance counts as silence
TRIM_KEEP = 120            # 5 ms at 24 kHz kept before the first sound, so its onset is not cut


class Output:
    def __init__(self, gain_db=6.0):
        self.makeup = 10 ** (gain_db / 20.0)
        self.trim_next = False     # set when an utterance starts: drop its leading silence
        self.trim_enabled = True
        self._trim_tail = []
        self.reset()

    def _trim(self, a):
        """Drop the silence before the first sound of an utterance (keeping 5 ms of it)."""
        for i, v in enumerate(a):
            if v > TRIM_LEVEL or v < -TRIM_LEVEL:
                keep = (self._trim_tail + list(a[max(0, i - TRIM_KEEP):i]))[-TRIM_KEEP:]
                self.trim_next = False
                self._trim_tail = []
                return array.array("h", keep) + a[i:]
        self._trim_tail = (self._trim_tail + list(a[-TRIM_KEEP:]))[-TRIM_KEEP:]
        return array.array("h")

    def reset(self):
        self._x1 = 0.0
        self._y1 = 0.0
        self._held = []        # the delayed block (already high-passed and amplified)
        self._held_peak = 0.0
        self._g = 1.0          # gain at the start of the held block
        self._carry = []       # samples not yet forming a whole block

    def _prep(self, samples):
        # high-pass + make-up gain, sample by sample
        x1, y1, m, p = self._x1, self._y1, self.makeup, HP_POLE
        out = []
        ap = out.append
        for x in samples:
            y = x - x1 + p * y1
            x1, y1 = x, y
            ap(y * m)
        self._x1, self._y1 = x1, y1
        return out

    def _emit_held(self, next_peak, dst):
        """Play the held block, ramping its gain to a value safe for it and the next block."""
        blk = self._held
        if not blk:
            return
        # never enter a block with more gain than it can take (the first block of an
        # utterance used to start at full gain, so a full-scale onset clipped)
        g0 = min(self._g, CEIL / max(self._held_peak, 1e-9))
        lim = min(1.0, CEIL / max(self._held_peak, next_peak, 1e-9))
        g1 = min(lim, g0 * RELEASE, 1.0)
        n = len(blk)
        step = (g1 - g0) / n
        g = g0
        for v in blk:
            g += step
            s = int(v * g)
            dst.append(32767 if s > 32767 else (-32768 if s < -32768 else s))
        self._g = g1

    def process(self, data):
        a = array.array("h")
        a.frombytes(data[:len(data) & ~1])
        if self.trim_next and self.trim_enabled:
            a = self._trim(a)
        if not a:
            return b""
        buf = self._carry + self._prep(a)
        out = array.array("h")
        nfull = len(buf) // BLOCK * BLOCK
        for i in range(0, nfull, BLOCK):
            blk = buf[i:i + BLOCK]
            pk = max(max(blk), -min(blk))
            self._emit_held(pk, out)
            self._held, self._held_peak = blk, pk
        self._carry = buf[nfull:]
        return out.tobytes()

    def flush(self):
        """End of an utterance: play what is still held back."""
        out = array.array("h")
        tail = self._carry
        self._carry = []
        pk = max(max(tail), -min(tail)) if tail else 0.0
        self._emit_held(pk, out)
        self._held, self._held_peak = tail, pk
        self._emit_held(0.0, out)
        self._held, self._held_peak = [], 0.0
        return out.tobytes()
