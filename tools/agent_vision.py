"""Vision signals for the agent layer, for what a host cannot read from memory.

Signals (one interface, backend "cpu" or "gpu"):
  hud(img)     -> {"hp": 0..1, "stamina": 0..1}: fill ratio of HUD bars (regions + hues from the game profile).
                  Plain numpy on any backend (a few columns of pixels; a GPU adds only transfer cost).
  prompts(img) -> [{"text", "conf", "box"}]: on-screen text (prompts, menus) with RapidOCR.
                  cpu = onnxruntime CPU, gpu = onnxruntime DirectML (any DX12 GPU, no CUDA install).

Not here, on purpose: moving/stuck (agentcore has it from positions), free space (no user), entities (memory first).

Profile keys (agent\\profiles\\<game>.ini, normalized to the client image, hue in OpenCV 0..180):
  hud_hp = x0,y0,x1,y1,hue_lo,hue_hi      hud_stamina = ...      prompt_region = x0,y0,x1,y1
  ocr_scale = 2   (upscale before OCR; default 2 under 1280 px, else 1)

CLI: python agent_vision.py hud|prompts <image> [--game bloodborne] [--backend cpu|gpu]
Needs tools\\.venv-vision (numpy, opencv-python-headless, onnxruntime-directml, rapidocr).
"""
import argparse
import json
import os
import sys
import time

import cv2
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_profile(game, root=ROOT):
    prof = {}
    path = os.path.join(root, "agent", "profiles", game + ".ini")
    if not os.path.exists(path):
        return prof
    with open(path, encoding="utf-8") as f:
        for line in f:
            if "=" not in line or line.lstrip().startswith("#"):
                continue
            k, v = line.split("=", 1)
            prof[k.strip()] = v.strip()
    return prof


def _floats(s):
    return [float(x) for x in s.split(",")]


def bar_fill(img, region, min_sat=60, min_val=50, col_frac=0.4, max_gap=3):
    """Fill ratio of a horizontal bar that fills from the left.

    region = (x0, y0, x1, y1, hue_lo, hue_hi), normalized; hue_lo > hue_hi wraps (reds).
    A column counts as filled when col_frac of its pixels have the bar hue; the fill ends at the last filled
    column of the run that starts at x0 (gaps up to max_gap columns are bridged: tick marks, JPEG noise).
    """
    h, w = img.shape[:2]
    x0, y0, x1, y1, lo, hi = region
    a, b = int(round(x0 * w)), int(round(x1 * w))
    c, d = int(round(y0 * h)), max(int(round(y0 * h)) + 1, int(round(y1 * h)))
    hsv = cv2.cvtColor(img[c:d, a:b], cv2.COLOR_BGR2HSV)
    hue, sat, val = hsv[..., 0], hsv[..., 1], hsv[..., 2]
    in_hue = (hue >= lo) & (hue <= hi) if lo <= hi else (hue >= lo) | (hue <= hi)
    mask = in_hue & (sat >= min_sat) & (val >= min_val)
    # Count only the rows the bar occupies: small frames make the bar 2 px tall inside a looser band.
    rows = mask.mean(axis=1) >= 0.2
    if rows.any():
        mask, val, sat = mask[rows], val[rows], sat[rows]
    filled = mask.mean(axis=0) >= col_frac
    # Souls-style bars mark the fill end with a bright white tick: the rightmost tick column right after the run.
    tick = ((val >= 170) & (sat <= 80)).mean(axis=0) >= col_frac
    end, gap = -1, 0
    lead = max(max_gap, int(0.12 * len(filled)))  # small frames blur the bar's first pixels
    for i, f in enumerate(filled):
        if f:
            end, gap = i, 0
        else:
            gap += 1
            if gap > max_gap and end >= 0:
                break
            if end < 0 and i >= lead:
                break  # nothing at the start: empty bar
    # A tick inside the run is the real end: what follows it is the regain/rally segment (Bloodborne shows hp lost
    # to a hit or a blood bullet as orange after the tick for a few seconds).
    inner = [i for i in range(max_gap, end - max_gap) if tick[i]]
    if inner:
        end = inner[0]
        while end + 1 < len(tick) and tick[end + 1]:
            end += 1
    elif end >= 0:
        i = end + 1
        while i < len(tick) and i <= end + 1 + max_gap and not tick[i]:
            i += 1
        while i < len(tick) and tick[i]:
            end, i = i, i + 1
    return 0.0 if end < 0 else min(1.0, (end + 1) / max(1, b - a))


class Vision:
    def __init__(self, game="bloodborne", backend="cpu", profile=None):
        self.game, self.backend = game, backend
        self.prof = profile if profile is not None else load_profile(game)
        self._ocr = None

    def hud(self, img):
        out = {}
        for key, name in (("hud_hp", "hp"), ("hud_stamina", "stamina")):
            if key in self.prof:
                out[name] = round(bar_fill(img, _floats(self.prof[key])), 3)
        return out

    def _engine(self):
        if self._ocr is None:
            from rapidocr import RapidOCR
            gpu = self.backend == "gpu"
            self._ocr = RapidOCR(params={"Global.log_level": "error", "EngineConfig.onnxruntime.use_dml": gpu})
        return self._ocr

    def prompts(self, img, min_conf=0.6):
        region = self.prof.get("prompt_region")
        ox = oy = 0
        if region:
            x0, y0, x1, y1 = _floats(region)
            h, w = img.shape[:2]
            ox, oy = int(x0 * w), int(y0 * h)
            img = img[oy:int(y1 * h), ox:int(x1 * w)]
        # Small frames (under 1280 px) are upscaled 2x first: small labels on decorated lines ("Arms" in the
        # Bloodborne pause menu) are missed at native size. Profile key ocr_scale overrides.
        s = float(self.prof.get("ocr_scale", 2 if max(img.shape[:2]) < 1280 else 1))
        if s != 1:
            img = cv2.resize(img, None, fx=s, fy=s, interpolation=cv2.INTER_CUBIC)
        r = self._engine()(img)
        out = []
        if r is None or r.txts is None:
            return out
        for box, txt, conf in zip(r.boxes, r.txts, r.scores):
            if conf >= min_conf and txt.strip():
                bx = [[int(p[0] / s) + ox, int(p[1] / s) + oy] for p in box]
                out.append({"text": txt.strip(), "conf": round(float(conf), 3), "box": bx})
        return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("signal", choices=["hud", "prompts"])
    ap.add_argument("image")
    ap.add_argument("--game", default="bloodborne")
    ap.add_argument("--backend", choices=["cpu", "gpu"], default="cpu")
    a = ap.parse_args(argv)
    img = cv2.imread(a.image)
    if img is None:
        print(json.dumps({"error": "cannot read " + a.image}))
        return 1
    v = Vision(a.game, a.backend)
    t = time.perf_counter()
    res = getattr(v, a.signal)(img)
    print(json.dumps({"signal": a.signal, "backend": a.backend, "ms": round((time.perf_counter() - t) * 1000, 1), "result": res}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
