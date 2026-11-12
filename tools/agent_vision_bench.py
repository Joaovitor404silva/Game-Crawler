"""Bench for tools/agent_vision.py: latency per signal x backend, and a fixed-rate load loop for game-cost runs.

  python agent_vision_bench.py latency <img...> [--game bloodborne] [--n 50]
      -> p50/p95 ms per signal x backend (first call = warm-up, reported apart)
  python agent_vision_bench.py loop <img> --signal prompts --backend gpu --hz 5 --secs 30
      -> runs the signal at a fixed rate (measure the game's FPS meanwhile); prints achieved Hz + latency
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # also under python -I

import cv2
import numpy as np

import agent_vision as av


def pct(xs, p):
    return round(float(np.percentile(xs, p)), 2) if xs else None


def latency(imgs, game, n):
    out = []
    for backend in ("cpu", "gpu"):
        v = av.Vision(game, backend)
        for sig in ("hud", "prompts"):
            t = time.perf_counter(); getattr(v, sig)(imgs[0]); first = (time.perf_counter() - t) * 1000
            ms = []
            for i in range(n):
                t = time.perf_counter(); getattr(v, sig)(imgs[i % len(imgs)]); ms.append((time.perf_counter() - t) * 1000)
            out.append({"signal": sig, "backend": backend, "first_ms": round(first, 1), "p50": pct(ms, 50), "p95": pct(ms, 95)})
    return out


def loop(img, game, signal, backend, hz, secs):
    v = av.Vision(game, backend)
    getattr(v, signal)(img)  # warm-up
    period, ms, t_end = 1.0 / hz, [], time.perf_counter() + secs
    nxt = time.perf_counter()
    while time.perf_counter() < t_end:
        t = time.perf_counter(); getattr(v, signal)(img); ms.append((time.perf_counter() - t) * 1000)
        nxt += period
        time.sleep(max(0.0, nxt - time.perf_counter()))
    return {"signal": signal, "backend": backend, "runs": len(ms), "hz": round(len(ms) / secs, 2), "p50": pct(ms, 50), "p95": pct(ms, 95)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["latency", "loop"])
    ap.add_argument("images", nargs="+")
    ap.add_argument("--game", default="bloodborne")
    ap.add_argument("--n", type=int, default=50)
    ap.add_argument("--signal", default="prompts")
    ap.add_argument("--backend", default="cpu")
    ap.add_argument("--hz", type=float, default=5)
    ap.add_argument("--secs", type=float, default=30)
    a = ap.parse_args()
    imgs = [cv2.imread(p) for p in a.images]
    if any(i is None for i in imgs):
        print(json.dumps({"error": "cannot read an image"})); return 1
    res = latency(imgs, a.game, a.n) if a.mode == "latency" else loop(imgs[0], a.game, a.signal, a.backend, a.hz, a.secs)
    print(json.dumps(res, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
