"""Auto-labelled frames for the vision signals (plan V2): labels come from AgentLink memory, not by hand.

record: drives the player through agentd (attacks, dodges, waits, heals) and after each step saves a clean frame
        (`look overlay=0`) plus the memory hp/stamina read before and after the capture -> <out>/labels.jsonl.
        Stand near an enemy (player invincible) so hp moves; agentd must be running for the game.
score:  runs Vision.hud on every frame and prints the error against memory (stamina max = highest stamina seen).

python agent_vision_dataset.py record --pid <game pid> --n 200 --out <dir>
python agent_vision_dataset.py score  --out <dir> [--game bloodborne]
"""
import argparse
import json
import os
import shutil
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from agentlink import AgentLink  # noqa: E402

ACTIONS = ["tap r1; wait 350", "wait 400", "tap circle; wait 500", "tap r1; wait 300; tap r1; wait 300", "wait 700"]
HEAL = "tap triangle; wait 1800"


class Agentd:
    def __init__(self, port=27070):
        self.port = port

    def call(self, line, timeout=30):
        with socket.create_connection(("127.0.0.1", self.port), timeout=timeout) as s:
            s.sendall((line + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n.\n"):
                d = s.recv(65536)
                if not d:
                    break
                buf += d
        return buf[:-3].decode("utf-8", "replace") if buf.endswith(b"\n.\n") else buf.decode("utf-8", "replace")


def vitals(link):
    p = link.read("player")
    return p.hp, p.hp_max, p.stamina


def record(a):
    os.makedirs(a.out, exist_ok=True)
    link, ad = AgentLink(a.pid), Agentd(a.port)
    rows = []
    for i in range(a.n):
        hp0, hpm, st0 = vitals(link)
        r = ad.call(f"look overlay=0 width={a.width} save=ds_{i:03d}")
        hp1, _, st1 = vitals(link)
        if not r.startswith("image "):
            print("look failed:", r.splitlines()[0])
            break
        src = r.split()[1]
        dst = os.path.join(a.out, f"{i:03d}.jpg")
        shutil.move(src, dst)
        rows.append({"img": os.path.basename(dst), "hp": [hp0, hp1], "hp_max": hpm, "stamina": [st0, st1]})
        ad.call("do " + (HEAL if hpm and hp1 / hpm < a.heal else ACTIONS[i % len(ACTIONS)]))
    ad.call("stop")
    with open(os.path.join(a.out, "labels.jsonl"), "w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row) + "\n")
    print(f"recorded {len(rows)} frames -> {a.out}")


def score(a):
    import cv2
    from agent_vision import Vision
    v = Vision(a.game)
    rows = [json.loads(x) for x in open(os.path.join(a.out, "labels.jsonl"), encoding="utf-8")]
    st_max = max(max(r["stamina"]) for r in rows) or 1
    err = {"hp": [], "stamina": []}
    skipped = 0
    for r in rows:
        img = cv2.imread(os.path.join(a.out, r["img"]))
        got = v.hud(img)
        for key, lab, top in (("hp", r["hp"], r["hp_max"]), ("stamina", r["stamina"], st_max)):
            if key not in got or not top:
                continue
            lo, hi = sorted(lab)
            if (hi - lo) / top > a.moving:  # value changed during the capture: label is ambiguous
                skipped += 1
                continue
            # Bloodborne stamina goes below 0 after an action; the bar then shows empty.
            err[key].append(abs(got[key] - min(1.0, max(0.0, (lo + hi) / 2 / top))))
    out = {"frames": len(rows), "skipped_moving": skipped}
    for key, e in err.items():
        if e:
            e.sort()
            out[key] = {"n": len(e), "mean": round(sum(e) / len(e), 4), "p95": round(e[int(0.95 * (len(e) - 1))], 4),
                        "max": round(e[-1], 4), "within_5pct": round(sum(x <= 0.05 for x in e) / len(e), 3)}
    print(json.dumps(out, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("mode", choices=["record", "score"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--port", type=int, default=27070)
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--width", type=int, default=960)
    ap.add_argument("--heal", type=float, default=0.25, help="heal when hp fraction drops below this")
    ap.add_argument("--game", default="bloodborne")
    ap.add_argument("--moving", type=float, default=0.03, help="skip labels that changed more than this during capture")
    a = ap.parse_args()
    record(a) if a.mode == "record" else score(a)


if __name__ == "__main__":
    main()
