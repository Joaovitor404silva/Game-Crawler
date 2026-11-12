"""Goto outcome stats from agentd flight logs (the per-goto "result" rows).

python agent_stats.py [logs dir] [--game bloodborne]

Prints counts per status and the stuck rate (plan Phase 5 gate: > 10% -> learned walk graph). A `stuck` with the
target behind a wall is the right answer, not a controller failure: the "short" column counts stucks that ended
within 3 m of the target (pushing at a corner), the ones a better controller could have finished.
"""
import argparse
import glob
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("logs", nargs="?", default=os.path.join(ROOT, "agent", "logs"))
    ap.add_argument("--game", default="*")
    a = ap.parse_args()
    rows = []
    for f in sorted(glob.glob(os.path.join(a.logs, f"{a.game}-*.jsonl"))):
        with open(f, encoding="utf-8") as fh:
            for line in fh:
                if '"result"' in line:
                    try:
                        rows.append(json.loads(line))
                    except ValueError:
                        pass
    if not rows:
        print("no goto result rows (agentd older than the result rows, or no gotos yet)")
        return 1
    by = {}
    for r in rows:
        by.setdefault(r["status"], []).append(r)
    n = len(rows)
    stuck = by.get("stuck", [])
    short = [r for r in stuck if r["left"] <= 3.0]
    out = {"gotos": n, "by_status": {k: len(v) for k, v in sorted(by.items())},
           "stuck_rate": round(len(stuck) / n, 3), "stuck_short": len(short),
           "unstuck_moves_avg": round(sum(r["unstuck"] for r in rows) / n, 2),
           "arrived_s_per_m": round(sum(r["s"] for r in by.get("arrived", [])) /
                                    max(1e-6, sum(r["straight"] for r in by.get("arrived", []))), 3)}
    planned = [r for r in rows if r.get("graph")]
    if planned:  # gotos that followed the learned walk graph (rows from agentd with the walk graph)
        out["graph_gotos"] = len(planned)
        out["graph_stuck_rate"] = round(sum(r["status"] == "stuck" for r in planned) / len(planned), 3)
    print(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
