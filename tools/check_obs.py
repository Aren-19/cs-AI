"""Verify rollout.py rebuilds observations identically to csai_track.inc."""

import os
import sys

import numpy as np

from rollout import Track, OBS_DIM, LOOKAHEAD, NO_LINE_SHIFT

HERE = os.path.dirname(os.path.abspath(__file__))
from game import CSTRIKE
DUMP = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai\out\obsdump.txt")
DATA = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai")

def _map_name():
    """Which map to verify: --map, else data/map.txt."""
    for i, a in enumerate(sys.argv):
        if a == "--map" and i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    try:
        with open(os.path.join(os.path.dirname(HERE), "data", "map.txt"),
                  encoding="utf-8-sig") as fh:
            n = fh.read().strip()
            if n:
                return n
    except OSError:
        pass
    return "surf_demise"

MAP = _map_name()
TRACK = os.path.join(DATA, "%s_track.txt" % MAP)

def main():
    if not os.path.exists(DUMP):
        print("no dump at %s - run train.ps1 with -ObsDump first" % DUMP)
        return 1

    track = Track(TRACK)
    rows = []
    lines = []
    shift = NO_LINE_SHIFT
    shifted = 0
    with open(DUMP) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("# line "):
                shift = tuple(float(v) for v in line.split()[2:])
                shifted += 1
                continue
            p = line.split()
            if len(p) != 6 + OBS_DIM:
                print("stale dump: %d columns, expected %d (6 state + %d obs)."
                      % (len(p), 6 + OBS_DIM, OBS_DIM))
                print("The observation changed since this dump was written.")
                print(r"Regenerate:  .\tools\train.ps1 -Batches 1 -Sync 0 -BatchSize 8 -ObsDump 300 -Wait")
                return 2
            rows.append([float(v) for v in p])
            lines.append(shift)

    if not rows:
        print("dump is empty")
        return 1

    data = np.array(rows)
    pos = data[:, 0:3]
    vel = data[:, 3:6]
    plugin_obs = data[:, 6:6 + OBS_DIM]

    n_center = 7 + 3 * LOOKAHEAD
    mine = np.zeros_like(plugin_obs)
    for i in range(len(rows)):
        idx = track.nearest(pos[i], -1)
        mine[i] = track.build_obs(pos[i], vel[i], idx, lines[i])
        mine[i, n_center:] = plugin_obs[i, n_center:]

    diff = np.abs(mine - plugin_obs)

    tol_typical = 2e-5      # p99 must stay at formatting level
    tol_worst = 5e-4        # isolated projection ties

    p50 = float(np.percentile(diff, 50))
    p99 = float(np.percentile(diff, 99))

    print("rows: %d   obs dim: %d (%d centerline checked, %d probe and own state passed through), %d shifted line(s)"
          % (len(rows), OBS_DIM, n_center, OBS_DIM - n_center, shifted))
    print("diff  p50 %.2e   p99 %.2e   max %.2e" % (p50, p99, diff.max()))
    worst_col = int(np.argmax(diff.max(axis=0)))
    print("worst column: %d (max %.3e)" % (worst_col, diff[:, worst_col].max()))

    if p99 > tol_typical:
        print("p99 %.2e exceeds %.0e - systematic drift, not a tie."

              % (p99, tol_typical))
        return 1

    bad_rows = np.where(diff.max(axis=1) > tol_worst)[0]
    ties = int((diff.max(axis=1) > tol_typical).sum())
    print("projection ties (> %.0e): %d of %d" % (tol_typical, ties, len(rows)))
    print("rows over worst-case tolerance (%.0e): %d of %d" % (tol_worst, len(bad_rows), len(rows)))

    # Isolated ties are expected: this search is a full scan, the plugin's is
    # windowed, and a shifted line magnifies the gap slightly.
    if len(bad_rows) > max(1, 0.005 * len(rows)) or diff.max() > 5e-3:
        for r in bad_rows[:5]:
            c = int(np.argmax(diff[r]))
            print("   row %d col %d: plugin %.6f python %.6f (d=%.3e)"
                  % (r, c, plugin_obs[r, c], mine[r, c], diff[r, c]))
        print("\nMISMATCH - the learner is not seeing what the actor saw.")
        return 1

    print("\nobservation parity OK")
    return 0

if __name__ == "__main__":
    sys.exit(main())
