"""Unit tests for tools/agent_vision.py on synthetic frames (no game, no saved screenshots).

Run: tools\\.venv-vision\\Scripts\\python.exe -I tests\\agent\\test_vision.py  (skips when the venv is missing).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
try:
    import cv2
    import numpy as np
    import agent_vision as av
except ImportError as e:  # vision venv not installed
    print("vision tests skipped:", e)
    sys.exit(0)

FAIL = PASS = 0


def check(cond, what):
    global FAIL, PASS
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print("FAIL", what)


W, H = 960, 400
HP = (0.046, 0.06, 0.2, 0.07, 165, 10)
ST = (0.046, 0.08, 0.15, 0.09, 35, 85)


def frame(hp, st, tick=True, noise=0, seed=1, rally=0.0):
    img = np.full((H, W, 3), 12, np.uint8)
    for (x0, y0, x1, y1, _, _), frac, colour in ((HP, hp, (30, 30, 150)), (ST, st, (40, 120, 40))):
        a, b, c, d = int(x0 * W), int(x1 * W), int(y0 * H), int(y1 * H)
        cv2.rectangle(img, (a - 1, c - 1), (b, d), (40, 40, 40), 1)  # frame
        e = a + int(round(frac * (b - a)))
        if colour[2] == 150 and rally:  # hp lost moments ago: orange (hue ~7, inside the hp hue range) after the tick
            img[c:d, e:e + int(round(rally * (b - a)))] = (20, 55, 160)
        if e > a:
            img[c:d, a:e] = colour
            if tick:
                img[c:d, max(a, e - 2):e] = (235, 235, 235)
    if noise:
        rng = np.random.default_rng(seed)
        img = np.clip(img.astype(np.int16) + rng.integers(-noise, noise + 1, img.shape), 0, 255).astype(np.uint8)
    return img


def test_bars():
    v = av.Vision(profile={"hud_hp": ",".join(map(str, HP)), "hud_stamina": ",".join(map(str, ST))})
    worst = 0
    for hp in (0.0, 0.03, 0.1, 0.37, 0.5, 0.81, 1.0):
        for st in (0.0, 0.25, 1.0):
            for tick in (True, False):
                for noise in (0, 12):
                    r = v.hud(frame(hp, st, tick, noise))
                    worst = max(worst, abs(r["hp"] - hp), abs(r["stamina"] - st))
    print("bars: worst abs error %.3f" % worst)
    check(worst <= 0.02, "bar fill error <= 2%")
    # Rally segment: the white tick marks the real hp, the orange part after it is regainable, not hp.
    worst = 0
    for hp in (0.05, 0.1, 0.4, 0.7):
        for noise in (0, 12):
            r = v.hud(frame(hp, 1.0, True, noise, rally=0.3))
            worst = max(worst, abs(r["hp"] - hp))
    print("rally: worst abs error %.3f" % worst)
    check(worst <= 0.02, "hp ends at the tick, rally segment ignored")


def test_jpeg_and_scale():
    v = av.Vision(profile={"hud_hp": ",".join(map(str, HP))})
    img = frame(0.42, 1.0)
    ok, buf = cv2.imencode(".jpg", cv2.resize(img, (640, 267), interpolation=cv2.INTER_AREA), [cv2.IMWRITE_JPEG_QUALITY, 80])
    r = v.hud(cv2.imdecode(buf, cv2.IMREAD_COLOR))
    check(abs(r["hp"] - 0.42) <= 0.03, "jpeg 640 px hp %.3f" % r["hp"])


def test_profile_parse(tmp=os.path.join(HERE, "_vision_profile")):
    os.makedirs(os.path.join(tmp, "agent", "profiles"), exist_ok=True)
    p = os.path.join(tmp, "agent", "profiles", "g.ini")
    with open(p, "w") as f:
        f.write("# c\nrun_speed = 3\nhud_hp = 0.1,0.2,0.3,0.4,165,10\n")
    prof = av.load_profile("g", tmp)
    check(prof.get("hud_hp") == "0.1,0.2,0.3,0.4,165,10" and prof.get("run_speed") == "3", "profile parse")
    os.remove(p)
    os.removedirs(os.path.join(tmp, "agent", "profiles"))


def test_ocr(backends):
    img = np.full((240, 900, 3), 10, np.uint8)
    cv2.putText(img, "Examine", (330, 120), cv2.FONT_HERSHEY_DUPLEX, 1.6, (225, 225, 225), 2, cv2.LINE_AA)
    cv2.putText(img, "Open the door", (300, 190), cv2.FONT_HERSHEY_DUPLEX, 1.2, (200, 200, 200), 2, cv2.LINE_AA)
    for b in backends:
        r = av.Vision(backend=b, profile={}).prompts(img)
        texts = [x["text"].lower() for x in r]
        check(any("examine" in t for t in texts), "%s ocr Examine %s" % (b, texts))
        check(any("open the door" in t for t in texts), "%s ocr Open the door %s" % (b, texts))


if __name__ == "__main__":
    test_bars()
    test_jpeg_and_scale()
    test_profile_parse()
    test_ocr(["cpu", "gpu"] if "--gpu" in sys.argv else ["cpu"])
    print("vision tests: %d passed, %d failed" % (PASS, FAIL))
    sys.exit(1 if FAIL else 0)
