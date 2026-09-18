"""
Verify that tools/rollout.py reproduces csai_track.inc exactly.

The plugin logs only (pos, vel) per step; the learner rebuilds the observation
from that. If the two implementations drift, PPO computes its importance ratio
against a distribution the actor never used - the run silently fails to learn and
nothing reports an error. So this is checked numerically rather than assumed.

Produce the input with:
    .\\tools\\train.ps1 -Batches 1 -Sync 0 -BatchSize 8 -ObsDump 300 -Wait

Then:
    python tools/check_obs.py
"""

import os
import sys

import numpy as np

from rollout import Track, OBS_DIM, LOOKAHEAD, PROBE_DIM, WISH_DIM

HERE = os.path.dirname(os.path.abspath(__file__))
CSTRIKE = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrike"
DUMP = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai\out\obsdump.txt")
DATA = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai")


def _map_name():
    """Which map to verify. Was hardcoded, so checking parity on a new map meant
    editing this file - and parity is the one guard against the learner silently
    training on observations the actor never produced."""
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
    with open(DUMP) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            p = line.split()
            if len(p) != 6 + OBS_DIM:
                print("stale dump: %d columns, expected %d (6 state + %d obs)."
                      % (len(p), 6 + OBS_DIM, OBS_DIM))
                print("The observation changed since this dump was written.")
                print("Regenerate:  .\tools\train.ps1 -Batches 1 -Sync 0 -BatchSize 8 -ObsDump 300 -Wait")
                return 2
            rows.append([float(v) for v in p])

    if not rows:
        print("dump is empty")
        return 1

    data = np.array(rows)
    pos = data[:, 0:3]
    vel = data[:, 3:6]
    plugin_obs = data[:, 6:6 + OBS_DIM]

    # Full scan here: if this agrees with the plugin's windowed search, both the
    # observation math and the nearest-point search match.
    # Only the centerline half is recomputed here. The probe needs engine
    # collision, so it is logged in the trajectory and copied through - there is
    # nothing to cross-check, and comparing it would just compare zeros.
    n_center = 7 + 3 * LOOKAHEAD
    mine = np.zeros_like(plugin_obs)
    for i in range(len(rows)):
        idx = track.nearest(pos[i], -1)
        mine[i] = track.build_obs(pos[i], vel[i], idx)
        mine[i, n_center:] = plugin_obs[i, n_center:]

    diff = np.abs(mine - plugin_obs)

    # Two different error scales, and conflating them hides real bugs:
    #
    #  * TYPICAL rows differ only by the %.6f dump formatting -> ~1e-6.
    #  * A FEW rows sit on a projection tie. Track_Project picks the nearer of the
    #    two segments adjacent to the nearest point; when those distances are
    #    nearly equal (measured: 91.226389 vs 91.226551, a 1.6e-4 gap) float32 in
    #    the plugin and float64 here pick different segments, shifting `closest`
    #    and hence the offset columns by ~1e-4.
    #
    # A tie is benign: it perturbs an O(1) network input by 1e-4. Systematic
    # drift is not. So gate on the median (must be formatting-level) and cap the
    # worst case, rather than applying one loose tolerance to everything.
    tol_typical = 2e-5      # p99 must stay at formatting level
    tol_worst = 5e-4        # isolated projection ties

    p50 = float(np.percentile(diff, 50))
    p99 = float(np.percentile(diff, 99))

    print("rows: %d   obs dim: %d (%d centerline checked, %d probe passed through)"
          % (len(rows), OBS_DIM, n_center, PROBE_DIM + WISH_DIM))
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

    if len(bad_rows):
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
