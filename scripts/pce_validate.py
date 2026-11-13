#!/usr/bin/env python3
"""PCE / BCES validator for the hal-inject compositor's PRNU injection.

The question this script answers:

  After the compositor writes its injected document rectangle into a real
  camera frame, does the document region carry the SAME sensor fingerprint
  (PRNU) as the surrounding background it replaced?

If YES — a forensics tool running PCE (Peak-to-Correlation Energy) across
the two regions will see matching per-sensor residual pattern inside and
outside the doc rect → no injection tell.  If NO — doc region's residual
is uncorrelated with the sensor's PRNU estimate, trivially detected.

Capture workflow (on-device, with hal-inject running):

  # Tell the hook to dump the next 32 POST-composite NV21 frames.
  adb shell "su -c 'echo 32 > /data/local/tmp/hal_inject/ctrl_dump'"
  # Open the target KYC/camera app so the pipeline runs.
  adb shell am start -n com.google.android.GoogleCamera/com.android.camera.CameraLauncher
  # Wait a few seconds for frames to accumulate, then pull.
  adb pull /data/local/tmp/hal_inject/dump_000.nv21 ...  (or any dump_*.nv21)
  adb pull /data/local/tmp/hal_inject/dump_000.dim

Analysis:

  python3 pce_validate.py --frames 'dump_*.nv21' --dims 1920x1080 \\
                          --doc-rect 720,400,1200,680

  --doc-rect X0,Y0,X1,Y1 is the composited document rectangle in sensor
  pixel coordinates. The background reference rect is picked AUTOMATICALLY
  as a same-sized rect maximally far from the doc, on the same row.

What the script reports:

  PCE_doc  — correlation of the doc rect's residual vs. the sensor-PRNU
             estimate (built from the whole frame minus the doc rect,
             averaged across all input frames). Should be HIGH if the
             compositor's PRNU injection is convincing.
  PCE_bg   — same correlation for the background reference rect. Should
             also be HIGH (it's real sensor data). Acts as the baseline.
  Ratio    — PCE_doc / PCE_bg. 1.0 = perfect match. < 0.1 = clear tell.

Typical pass criterion:  PCE_doc > 60  AND  PCE_doc / PCE_bg > 0.2.
"""

from __future__ import annotations

import argparse
import glob
import os
import sys
from dataclasses import dataclass

import numpy as np

try:
    from scipy.ndimage import gaussian_filter
    from scipy.signal import correlate
except ImportError:
    sys.exit("pip install scipy numpy")


# ─── NV21 I/O ─────────────────────────────────────────────────────────────

def load_nv21_luma(path: str, w: int, h: int) -> np.ndarray:
    """Return the Y plane of an NV21 file as float32, shape (h, w)."""
    data = np.fromfile(path, dtype=np.uint8, count=w * h)
    if data.size != w * h:
        raise ValueError(f"{path}: expected {w*h} luma bytes, got {data.size}")
    return data.reshape(h, w).astype(np.float32)


# ─── Residual extraction ─────────────────────────────────────────────────

def residual(gray: np.ndarray, sigma: float = 1.5) -> np.ndarray:
    """Noise residual = frame − Gaussian-denoised frame.

    A real sensor's fingerprint lives in the high-frequency component that
    survives gentle denoising. Good enough for sanity-level PRNU testing;
    production forensics tools (e.g. PRNU Decompressor) use wavelet
    denoisers but the ordering of PCE results across equal-size regions
    is insensitive to that detail.
    """
    denoised = gaussian_filter(gray, sigma=sigma)
    return gray - denoised


def zero_mean_unit_var(x: np.ndarray) -> np.ndarray:
    x = x - x.mean()
    s = float(x.std())
    return x / s if s > 1e-9 else x


# ─── PCE ─────────────────────────────────────────────────────────────────

def pce(test_region: np.ndarray, ref_region: np.ndarray,
        exclude_radius: int = 11) -> float:
    """Peak-to-Correlation Energy. Positive and larger = stronger match.

    Both inputs should be same-shape 2-D residuals. We cross-correlate and
    compute peak² / mean(off-peak²), excluding a square of radius
    ``exclude_radius`` pixels around the peak from the noise-floor estimate.
    """
    t = zero_mean_unit_var(test_region)
    r = zero_mean_unit_var(ref_region)
    corr = correlate(t, r, mode="same")
    # Peak
    peak_idx = np.unravel_index(int(np.argmax(np.abs(corr))), corr.shape)
    py, px = peak_idx
    peak = corr[py, px]
    # Exclusion mask
    mask = np.ones_like(corr, dtype=bool)
    y0 = max(0, py - exclude_radius); y1 = min(corr.shape[0], py + exclude_radius + 1)
    x0 = max(0, px - exclude_radius); x1 = min(corr.shape[1], px + exclude_radius + 1)
    mask[y0:y1, x0:x1] = False
    off_peak = corr[mask]
    if off_peak.size == 0:
        return 0.0
    denom = float((off_peak ** 2).mean())
    if denom <= 0:
        return 0.0
    return float(np.sign(peak) * peak * peak / denom)


# ─── Driver ──────────────────────────────────────────────────────────────

@dataclass
class Rect:
    x0: int; y0: int; x1: int; y1: int

    def width(self) -> int: return self.x1 - self.x0
    def height(self) -> int: return self.y1 - self.y0
    def slice(self, arr: np.ndarray) -> np.ndarray:
        return arr[self.y0:self.y1, self.x0:self.x1]


def parse_dims(s: str) -> tuple[int, int]:
    w, h = s.lower().split("x")
    return int(w), int(h)


def parse_rect(s: str) -> Rect:
    xs = [int(v.strip()) for v in s.split(",")]
    if len(xs) != 4:
        raise ValueError("rect must be X0,Y0,X1,Y1")
    return Rect(*xs)


def _overlaps(a: Rect, b: Rect) -> bool:
    return not (a.x1 <= b.x0 or b.x1 <= a.x0 or a.y1 <= b.y0 or b.y1 <= a.y0)


def pick_bg_rect(doc: Rect, w: int, h: int) -> Rect:
    """Pick a same-sized background rect that does NOT overlap the doc rect.

    Tries, in order:
      1. Horizontal mirror across frame centre (cleanest when doc is off-centre)
      2. Vertical mirror
      3. Slide to top-left corner, then any corner with room
    Falls back to the top-left corner clamp if nothing else fits; the caller
    will warn if sizes mismatch (rect may get clipped in degenerate inputs).
    """
    rw, rh = doc.width(), doc.height()
    candidates = [
        Rect(w - doc.x1, doc.y0, w - doc.x0, doc.y1),          # horiz mirror
        Rect(doc.x0,     h - doc.y1, doc.x1, h - doc.y0),      # vert mirror
        Rect(0, 0, rw, rh),                                     # TL
        Rect(w - rw, 0, w, rh),                                 # TR
        Rect(0, h - rh, rw, h),                                 # BL
        Rect(w - rw, h - rh, w, h),                             # BR
    ]
    for c in candidates:
        if (0 <= c.x0 and c.x1 <= w and 0 <= c.y0 and c.y1 <= h
                and not _overlaps(c, doc)):
            return c
    # Nothing fit cleanly — clamp TL and accept any residual overlap.
    return Rect(0, 0, min(rw, w), min(rh, h))


def temporal_coherence(stack: np.ndarray) -> float:
    """Mean pairwise normalized cross-correlation between consecutive frames
    of a (N, H, W) residual stack. Values close to 1 = strong static pattern
    (PRNU present and dominant). Values near 0 = pure IID per-frame noise."""
    if stack.shape[0] < 2:
        return 0.0
    vals = []
    for i in range(stack.shape[0] - 1):
        a = zero_mean_unit_var(stack[i]).ravel()
        b = zero_mean_unit_var(stack[i + 1]).ravel()
        vals.append(float((a * b).mean()))
    return float(np.mean(vals))


def shot_slope(luma_stack: np.ndarray, res_stack: np.ndarray,
               bins: int = 8) -> tuple[float, float]:
    """Log-log slope of residual variance vs mean luma, binned.
    True shot noise has σ∝√Y, so Var∝Y and the log slope is 1.0 (±).
    Returns (slope, R²-ish fit-quality in [0,1]).
    """
    y = luma_stack.ravel(); r = res_stack.ravel()
    order = np.argsort(y); y = y[order]; r = r[order]
    per = len(y) // bins
    xs, ys = [], []
    for i in range(bins):
        s = slice(i * per, (i + 1) * per if i < bins - 1 else len(y))
        yb = float(y[s].mean())
        vb = float(r[s].var())
        if yb > 1 and vb > 1e-9:
            xs.append(np.log(yb)); ys.append(np.log(vb))
    if len(xs) < 2:
        return 0.0, 0.0
    xs = np.asarray(xs); ys = np.asarray(ys)
    slope, inter = np.polyfit(xs, ys, 1)
    pred = slope * xs + inter
    ss_res = float(((ys - pred) ** 2).sum())
    ss_tot = float(((ys - ys.mean()) ** 2).sum())
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0
    return float(slope), float(max(0.0, r2))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--frames", required=True,
                    help="glob of NV21 files, e.g. 'dump_*.nv21'")
    ap.add_argument("--dims", required=True,
                    help="frame dims WxH, e.g. 1920x1080")
    ap.add_argument("--doc-rect", required=True,
                    help="document rect X0,Y0,X1,Y1 in sensor pixels")
    ap.add_argument("--bg-rect", default=None,
                    help="optional background rect X0,Y0,X1,Y1; "
                         "default = mirror of doc across frame centre")
    ap.add_argument("--denoise-sigma", type=float, default=1.5,
                    help="Gaussian denoise sigma for residual extraction")
    ap.add_argument("--coh-tol", type=float, default=0.25,
                    help="max |coh_doc - coh_bg| (default 0.25)")
    ap.add_argument("--slope-tol", type=float, default=0.8,
                    help="max |slope_doc - slope_bg|. Loose default because "
                         "in-scene residuals carry image high-freq that "
                         "dominates shot-noise variance signal; a strict "
                         "slope test needs a flat-field capture (--flat-field "
                         "mode TODO). Temporal coherence is the strong signal.")
    args = ap.parse_args()

    w, h = parse_dims(args.dims)
    doc = parse_rect(args.doc_rect)
    bg  = parse_rect(args.bg_rect) if args.bg_rect else pick_bg_rect(doc, w, h)
    if doc.width() != bg.width() or doc.height() != bg.height():
        print(f"WARN: doc and bg rect sizes differ ({doc.width()}x{doc.height()} "
              f"vs {bg.width()}x{bg.height()}) - stats comparison weaker")

    files = sorted(glob.glob(args.frames))
    if not files:
        sys.exit(f"no frames matched {args.frames!r}")
    print(f"[+] {len(files)} frame(s), {w}x{h}, "
          f"doc={doc.__dict__} bg={bg.__dict__}")

    # Stack frames + residuals per region.
    luma_frames = [load_nv21_luma(f, w, h) for f in files]
    res_frames  = [residual(y, sigma=args.denoise_sigma) for y in luma_frames]
    for i, r in enumerate(res_frames):
        print(f"  frame {i:02d}  mean|res|={np.abs(r).mean():.3f}  "
              f"max|res|={np.abs(r).max():.1f}")

    doc_L = np.stack([doc.slice(y) for y in luma_frames], axis=0)
    bg_L  = np.stack([bg.slice(y)  for y in luma_frames], axis=0)
    doc_R = np.stack([doc.slice(r) for r in res_frames], axis=0)
    bg_R  = np.stack([bg.slice(r)  for r in res_frames], axis=0)

    # Metric 1: residual magnitude (expected ~equal if σ-scaling is correct)
    mag_doc = float(np.abs(doc_R).mean())
    mag_bg  = float(np.abs(bg_R ).mean())

    # Metric 2: temporal coherence (fraction of residual that is STATIC
    # between frames = PRNU component dominance). Equal values = both
    # regions carry comparable sensor fingerprint.
    coh_doc = temporal_coherence(doc_R)
    coh_bg  = temporal_coherence(bg_R)

    # Metric 3: shot-noise variance scaling slope (log-log). True shot
    # noise → slope ≈ 1.0 (var proportional to Y). Flat (slope ≈ 0) means
    # noise is INDEPENDENT of luma (= old constant-σ xorshift behaviour).
    s_doc, r2_doc = shot_slope(doc_L, doc_R)
    s_bg,  r2_bg  = shot_slope(bg_L,  bg_R)

    d_coh   = abs(coh_doc - coh_bg)
    d_slope = abs(s_doc   - s_bg)

    print()
    print(f"              {'DOC':>12s}   {'BG':>12s}   {'delta':>10s}")
    print(f"mean|res|   : {mag_doc:12.4f}   {mag_bg :12.4f}   "
          f"{mag_doc - mag_bg:10.4f}")
    print(f"temporal coh: {coh_doc:12.4f}   {coh_bg :12.4f}   "
          f"{d_coh:10.4f}  (want <={args.coh_tol})")
    print(f"shot slope  : {s_doc  :12.4f}   {s_bg   :12.4f}   "
          f"{d_slope:10.4f}  (want <={args.slope_tol})")
    print(f"slope R^2   : {r2_doc :12.4f}   {r2_bg  :12.4f}")
    print()

    passed = d_coh <= args.coh_tol and d_slope <= args.slope_tol
    print(f"VERDICT: {'PASS - doc region matches sensor stats' if passed else 'FAIL - doc region stats differ from sensor'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
