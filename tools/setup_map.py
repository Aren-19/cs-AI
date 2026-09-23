"""Prepare a map for training from a recorded run, or check an existing setup."""

import argparse
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

CSTRIKE = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrike"
DATA = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai")
REPLAYBOT = os.path.join(CSTRIKE, r"addons\sourcemod\data\replaybot")
MAPS = os.path.join(CSTRIKE, "maps")
SHAVIT_DB = os.path.join(CSTRIKE, r"addons\sourcemod\data\sqlite\shavit-local.sq3")

ARTEFACTS = ("track", "states", "prestrafe", "demo")

# csai_episode.inc: further than this from the centerline ends the episode.
MAX_DEVIATION = 600.0

def find_replay(map_name, given):
    if given:
        return given if os.path.isfile(given) else None
    for style in sorted(os.listdir(REPLAYBOT)) if os.path.isdir(REPLAYBOT) else []:
        p = os.path.join(REPLAYBOT, style, "%s.replay" % map_name)
        if os.path.isfile(p):
            return p
    return None

def paths(map_name):
    return {a: os.path.join(DATA, "%s_%s.txt" % (map_name, a)) for a in ARTEFACTS}

def write_zone(map_name):
    """Copy shavit's start zone (main track) to <map>_zone.txt for the plugin."""
    import sqlite3
    if not os.path.isfile(SHAVIT_DB):
        return None
    try:
        db = sqlite3.connect("file:%s?mode=ro" % SHAVIT_DB.replace("\\", "/"), uri=True)
        row = db.execute("SELECT corner1_x, corner1_y, corner1_z, corner2_x, corner2_y, corner2_z "
                         "FROM mapzones WHERE map = ? AND type = 0 AND track = 0 LIMIT 1",
                         (map_name,)).fetchone()
        db.close()
    except sqlite3.Error:
        return None
    if not row:
        return None
    path = os.path.join(DATA, "%s_zone.txt" % map_name)
    with open(path, "w", encoding="ascii") as fh:
        fh.write("# shavit start zone, main track\n")
        fh.write("start %s\n" % " ".join("%.3f" % v for v in row))
    return path

def header_value(path, key, cast=float):
    try:
        with open(path, encoding="utf-8-sig") as fh:
            for line in fh:
                if line.startswith("#") and key in line:
                    return cast(line.split(key)[1].split()[0])
                if not line.startswith("#"):
                    break
    except (OSError, ValueError, IndexError):
        pass
    return None

def data_lines(path):
    try:
        with open(path, encoding="utf-8-sig") as fh:
            return sum(1 for l in fh if l.strip() and not l.startswith("#"))
    except OSError:
        return 0

def run_vs_track(map_name, demo_path):
    """How far does a recorded run stray from the track the reward uses?"""
    try:
        sys.path.insert(0, HERE)
        import numpy as np
        from rollout import Track
    except ImportError:
        return None
    tpath = os.path.join(DATA, "%s_track.txt" % map_name)
    if not (os.path.isfile(tpath) and os.path.isfile(demo_path)):
        return None
    track = Track(tpath)
    if not track.n:
        return None

    pts = []
    try:
        with open(demo_path, encoding="utf-8-sig") as fh:
            for line in fh:
                if line.startswith("#") or not line.strip():
                    continue
                p = line.split()
                if len(p) >= 4:
                    pts.append((float(p[1]), float(p[2]), float(p[3])))
    except (OSError, ValueError):
        return None
    if len(pts) < 10:
        return None
    pts = np.array(pts)

    step = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    teleported = bool((step > 2000).any())

    d = []
    hint = -1
    for i in range(len(pts)):
        j = track.nearest(pts[i], hint)
        hint = j
        _, _, dist = track.project(pts[i], j)
        d.append(dist)
    d = np.array(d)
    return float(d.max()), int((d > MAX_DEVIATION).sum()), len(d), teleported

def check(map_name):
    """Returns (ok, list of (label, detail, is_problem))."""
    p = paths(map_name)
    rows = []
    ok = True

    for a in ARTEFACTS:
        if not os.path.isfile(p[a]):
            rows.append((a, "MISSING", True))
            ok = False

    if os.path.isfile(p["track"]):
        n = data_lines(p["track"])
        length = header_value(p["track"], "length=")
        bad = n < 2 or not length or length <= 0
        rows.append(("track", "%d points, %s units" % (n, "%.0f" % length if length else "?"), bad))
        ok = ok and not bad
        # The plugin's MAX_TRACK is 16384; past that it truncates and would
        # report a finish partway through the map.
        if n > 16384:
            rows.append(("track", "OVER the plugin's 16384-point limit", True))
            ok = False

    if os.path.isfile(p["states"]):
        n = data_lines(p["states"])
        t = header_value(p["states"], "clean_time=")
        bad = n < 2
        rows.append(("states", "%d checkpoints, reference %s s"
                     % (n, "%.2f" % t if t else "?"), bad))
        ok = ok and not bad

    if os.path.isfile(p["prestrafe"]):
        n = data_lines(p["prestrafe"])
        bad = n < 8
        rows.append(("prestrafe", "%d ticks" % n, bad))
        ok = ok and not bad

    if os.path.isfile(p["demo"]):
        n = data_lines(p["demo"])
        pre = header_value(p["demo"], "preframes=", int)
        bad = n < 100 or pre is None or pre >= n
        rows.append(("demo", "%d ticks, %s preframes" % (n, pre), bad))
        ok = ok and not bad

    # Every recorded run, against the corridor the reward enforces.
    for path in sorted(glob.glob(os.path.join(DATA, "%s_demo*.txt" % map_name))):
        r = run_vs_track(map_name, path)
        if r is None:
            continue
        worst, out, total, teleported = r
        label = "run " + os.path.basename(path)[len(map_name) + 1:-4]
        if teleported:
            rows.append((label, "has a map teleport - cannot be measured against "
                                "the track past that point", False))
        elif out > 0:
            rows.append((label, "%d of %d ticks more than %.0f units off the track "
                                "(worst %.0f) - do not clone from this one"
                         % (out, total, MAX_DEVIATION, worst), False))
        else:
            rows.append((label, "stays within %.0f units of the track" % worst, False))

    zone = os.path.join(DATA, "%s_zone.txt" % map_name)
    rows.append(("start zone", "present" if os.path.isfile(zone) else
                 "missing - the wind-up hands over on takeoff only", False))

    bsp = os.path.join(MAPS, "%s.bsp" % map_name)
    rows.append(("map file", "present" if os.path.isfile(bsp) else "NOT INSTALLED",
                 not os.path.isfile(bsp)))
    ok = ok and os.path.isfile(bsp)
    return ok, rows

def report(map_name):
    ok, rows = check(map_name)
    width = max(len(r[0]) for r in rows) if rows else 8
    for label, detail, bad in rows:
        print("  %-*s %s%s" % (width, label, detail, "   <-- problem" if bad else ""))
    return ok

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map")
    ap.add_argument("--replay", default=None,
                    help="the recorded run to derive from; default is the timer's "
                         "replay for this map")
    ap.add_argument("--spacing", type=float, default=64.0)
    ap.add_argument("--checkpoints", type=int, default=24)
    ap.add_argument("--check", action="store_true", help="validate, change nothing")
    args = ap.parse_args()

    print("== %s ==" % args.map)
    if write_zone(args.map):
        print("  start zone copied from the timer's database")
    if args.check:
        ok = report(args.map)
        print()
        print("ready to train" if ok else "NOT ready - see the problems above")
        return 0 if ok else 1

    replay = find_replay(args.map, args.replay)
    if not replay:
        print("  no replay found for %s" % args.map)
        print("  run the map once with your timer, or pass --replay <file>")
        return 1
    print("  source: %s" % replay)

    p = paths(args.map)
    os.makedirs(DATA, exist_ok=True)
    cmd = [sys.executable, os.path.join(HERE, "replay.py"), replay,
           "--spacing", str(args.spacing), "--checkpoints", str(args.checkpoints),
           "--track", p["track"], "--states", p["states"],
           "--prestrafe", p["prestrafe"], "--demo", p["demo"]]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print("  replay.py failed:")
        print(res.stdout[-2000:])
        print(res.stderr[-2000:])
        return 1

    for line in res.stdout.splitlines():
        if line.startswith(("clean frames", "clean time", "segment", "  ")) or "dropped" in line:
            print("  %s" % line.strip())

    print()
    ok = report(args.map)
    print()
    if not ok:
        print("NOT ready - see the problems above")
        return 1
    print("ready. Train it with:")
    print("    .\\tools\\daemon.ps1 -Power high -Map %s" % args.map)
    print()
    print("The bot starts from scratch on a new map: the weights are per-map and")
    print("there is nothing to carry over. More recorded runs of your own help it")
    print("most - see HOWTO.md.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
