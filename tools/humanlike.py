"""How human a run looks, next to a recorded human run on the same map.

Measures the mouse (turn speed and how fast that speed changes), the strafe
keys (how long each is held, how often none is), and the wind-up (where it
jumps and how long it stays in the air in the start zone).

    python tools/humanlike.py <replay> [--human <replay>] [--map <name>] [--summary <file>]

With no --human, the server's own record replay for the map is used. Exits 1
when the run does something no recorded human run does.
"""

import argparse
import os
import sys

import numpy as np

from replay import parse_replay

from game import CSTRIKE
SMDATA  = os.path.join(CSTRIKE, r"addons\sourcemod\data")

IN_JUMP, IN_FORWARD, IN_MOVELEFT, IN_MOVERIGHT = 2, 8, 512, 1024
FL_ONGROUND = 1

# What the recorded human runs stay within, with a little room. Turn speed in
# degrees per tick, its change in degrees per tick per tick.
LIMITS = {
    "air_turn_p99":   7.5,
    "air_accel_p99":  3.0,
    "ground_turn_max": 4.5,
    "short_holds":    0.05,    # share of key presses held 2 ticks or less
}

def wrap(a):
    return (a + 180.0) % 360.0 - 180.0

def human_replay(mapname):
    for style in ("0", "7"):
        p = os.path.join(SMDATA, "replaybot", style, mapname + ".replay")
        if os.path.isfile(p):
            return p
    return None

def zone(mapname):
    p = os.path.join(SMDATA, "csai", mapname + "_zone.txt")
    try:
        with open(p) as fh:
            for line in fh:
                f = line.split()
                if f and f[0] == "start" and len(f) >= 7:
                    a = np.array([float(v) for v in f[1:4]])
                    b = np.array([float(v) for v in f[4:7]])
                    return np.minimum(a, b), np.maximum(a, b)
    except OSError:
        pass
    return None

def key_holds(buttons):
    """Lengths of each unbroken A-only or D-only press."""
    side = np.where((buttons & IN_MOVELEFT) & ~(buttons & IN_MOVERIGHT), 1,
                    np.where((buttons & IN_MOVERIGHT) & ~(buttons & IN_MOVELEFT), -1, 0))
    holds, cur, n = [], 0, 0
    for s in side:
        if s == cur:
            n += 1
            continue
        if cur != 0:
            holds.append(n)
        cur, n = s, 1
    if cur != 0:
        holds.append(n)
    return np.array(holds)

def measure(path, mapname):
    r = parse_replay(path)
    fr = r.frames
    if len(fr) < 10:
        return None
    pos = np.array([f.pos for f in fr], dtype=float)
    yaw = np.array([f.ang[1] for f in fr], dtype=float)
    btn = np.array([f.buttons for f in fr], dtype=np.int64)
    flg = np.array([f.flags for f in fr], dtype=np.int64)
    pre = int(r.preframes)
    n = len(fr)

    step = np.r_[0.0, np.linalg.norm(np.diff(pos, axis=0), axis=1)]
    ok = step < 180.0                           # a teleport in a segmented run
    ok &= np.r_[True, ok[:-1]] & np.r_[ok[1:], True]
    turn = np.r_[0.0, wrap(np.diff(yaw))]
    accel = np.r_[0.0, np.diff(turn)]
    ground = (flg & FL_ONGROUND) != 0

    run = np.zeros(n, bool)
    run[pre + 2:] = True
    air = run & ok & ~ground
    out = {"file": os.path.basename(path), "ticks": n, "prestrafe": pre}
    if air.sum() > 20:
        out["air_turn_p99"] = float(np.percentile(np.abs(turn[air]), 99))
        out["air_accel_p99"] = float(np.percentile(np.abs(accel[air]), 99))
        b = btn[air]
        out["no_key"] = float(np.mean(((b & IN_MOVELEFT) == 0) & ((b & IN_MOVERIGHT) == 0)))
    h = key_holds(btn[pre:])
    if len(h) and n - pre > 66:          # a run of a second or more
        out["hold_median"] = float(np.median(h))
        out["short_holds"] = float(np.mean(h <= 2))
        secs = (n - pre) * (r.tickrate and 1.0 / r.tickrate or 0.015)
        out["switches_per_s"] = float(len(h) / max(secs, 1e-6))

    # The wind-up: everything before the timer.
    if pre > 4:
        wg = ground[:pre] & ok[:pre]
        wg[:2] = False
        if wg.any():
            out["ground_turn_max"] = float(np.abs(turn[:pre][wg]).max())
        jumped = np.where((btn[:pre] & IN_JUMP) & ground[:pre])[0]
        # the last take-off before the timer starts
        lift = np.where(ground[:pre - 1] & ~ground[1:pre])[0]
        if len(lift):
            j = int(lift[-1]) + 1
            out["zone_air_ticks"] = pre - j
            z = zone(mapname)
            if z is not None:
                mn, mx = z
                p = pos[j - 1]
                out["jump_inside"] = float(min(p[0] - mn[0], mx[0] - p[0], p[1] - mn[1], mx[1] - p[1]))
            w = turn[j:pre]
            out["zone_turn"] = float(np.abs(w).sum())
            sgn = np.sign(w[np.abs(w) > 0.2])
            out["zone_reversals"] = int(np.sum(sgn[1:] != sgn[:-1])) if len(sgn) else 0
        elif len(jumped) == 0:
            out["zone_air_ticks"] = 0
    return out

ROWS = (("air_turn_p99", "air turn speed, 99th pct", "%.2f deg/tick"),
        ("air_accel_p99", "air turn change, 99th pct", "%.2f deg/tick2"),
        ("no_key", "air ticks with no key", "%.2f"),
        ("hold_median", "key hold, median", "%.0f ticks"),
        ("short_holds", "presses of 2 ticks or less", "%.2f"),
        ("switches_per_s", "key changes per second", "%.2f"),
        ("ground_turn_max", "wind-up ground turn, max", "%.2f deg/tick"),
        ("jump_inside", "jump, units inside the zone", "%.0f"),
        ("zone_air_ticks", "air ticks in the zone", "%d"),
        ("zone_turn", "turn in the zone air", "%.0f deg"),
        ("zone_reversals", "turn reversals in the zone air", "%d"))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("--human")
    ap.add_argument("--map")
    ap.add_argument("--summary", help="also write a one-line verdict to this file")
    args = ap.parse_args()

    mapname = args.map or parse_replay(args.replay).map or ""
    human = args.human or human_replay(mapname)
    bot = measure(args.replay, mapname)
    ref = measure(human, mapname) if human else None
    if bot is None:
        print("%s: too short to measure" % args.replay)
        return 1

    print("%-32s %16s %16s" % (mapname, "this run", "human" if ref else ""))
    for key, label, fmt in ROWS:
        a = (fmt % bot[key]) if key in bot else "-"
        b = (fmt % ref[key]) if ref and key in ref else ""
        print("  %-30s %16s %16s" % (label, a, b))

    bad = [k for k, lim in LIMITS.items() if k in bot and bot[k] > lim]
    if bad:
        verdict = "NOT HUMAN: " + ", ".join("%s %.2f > %.2f" % (k, bot[k], LIMITS[k]) for k in bad)
    else:
        verdict = "human-like"
    print(verdict)
    if args.summary:
        parts = [verdict]
        for key, short in (("air_turn_p99", "turn"), ("air_accel_p99", "accel"),
                           ("hold_median", "hold"), ("no_key", "nokey"),
                           ("jump_inside", "jump"), ("zone_air_ticks", "zoneair")):
            if key in bot:
                parts.append("%s %.2f" % (short, bot[key]))
        with open(args.summary, "w") as fh:
            fh.write(" | ".join(parts) + "\n")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
