"""Control-quality stats for a replay: strafe angle, key changes, speed."""

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay import parse_replay

IN_MOVELEFT, IN_MOVERIGHT = 512, 1024

def analyse(path):
    r = parse_replay(path)
    fr = r.run_frames
    tr = r.tickrate or 66.67
    dt = 1.0 / tr
    if len(fr) < 3:
        return None

    pitch = np.array([f.ang[0] for f in fr])
    dpitch = np.abs(np.diff(pitch)) if len(pitch) > 1 else np.zeros(1)

    yaw = np.array([f.ang[1] for f in fr])
    dyaw = np.array([math.remainder(float(yaw[i] - yaw[i - 1]), 360.0)
                     for i in range(1, len(yaw))])
    adyaw = np.abs(dyaw)

    # Large view jumps are not ordinary steering; exclude them so the jitter
    # percentiles describe the smooth part.
    big = adyaw > 90.0
    smooth = adyaw[~big]

    keys = []
    for f in fr:
        left = bool(f.buttons & IN_MOVELEFT)
        right = bool(f.buttons & IN_MOVERIGHT)
        if left != right:
            keys.append(1 if right else -1)
    nflips = sum(1 for i in range(1, len(keys)) if keys[i] != keys[i - 1])

    phis, speeds = [], []
    for i in range(1, len(fr)):
        a, b = fr[i - 1], fr[i]
        vx, vy = (b.pos[0] - a.pos[0]) / dt, (b.pos[1] - a.pos[1]) / dt
        sh = math.hypot(vx, vy)
        if sh < 50.0:
            continue
        left = bool(b.buttons & IN_MOVELEFT)
        right = bool(b.buttons & IN_MOVERIGHT)
        if left == right:
            continue
        wish = b.ang[1] + (90.0 if left else -90.0)
        phis.append(math.remainder(wish - math.degrees(math.atan2(vy, vx)), 360.0))
        speeds.append(sh)

    phis = np.array(phis) if phis else np.zeros(0)
    speeds = np.array(speeds) if len(speeds) else np.zeros(1)
    secs = len(fr) * dt

    inwin = float(np.mean((np.abs(phis) > 85) & (np.abs(phis) < 95)) * 100) if len(phis) else 0.0
    return {
        "name": os.path.basename(path).replace(".replay", ""),
        "secs": secs,
        "ticks": len(fr),
        "flips_per_s": float(nflips) / secs,
        "smooth_med": float(np.median(smooth)) if len(smooth) else 0.0,
        "smooth_p95": float(np.percentile(smooth, 95)) if len(smooth) else 0.0,
        "phi_in_window": inwin,
        "pitch_med": float(np.median(pitch)),
        "pitch_dp95": float(np.percentile(dpitch, 95)),
        "phi_med_abs": float(np.median(np.abs(phis))) if len(phis) else 0.0,
        "speed_med": float(np.median(speeds)),
        "speed_max": float(np.max(speeds)),
    }

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    rows = [a for a in (analyse(p) for p in sys.argv[1:]) if a]
    if not rows:
        print("nothing to analyse")
        return 1

    print("%-26s %6s %7s %8s %9s %10s %8s %7s %6s" %
          ("replay", "secs", "flips/s", "med|dyaw|", "p95|dyaw|", "phi in win",
           "med spd", "pitch", "dp95"))
    for a in rows:
        print("%-26s %6.2f %7.2f %8.2f %9.2f %9.1f%% %8.0f %7.1f %6.2f" %
              (a["name"], a["secs"], a["flips_per_s"], a["smooth_med"],
               a["smooth_p95"], a["phi_in_window"], a["speed_med"],
               a["pitch_med"], a["pitch_dp95"]))

    print()
    print("phi in win = %% of strafing frames with |phi| in (85,95) deg, i.e. in the")
    print("             only range where air acceleration does anything at surf speed.")
    print("pitch      = median view pitch, and 95th pct of per-tick change.")
    print("             Human on surf_demise: 9.3 deg, 0.30 deg/tick.")
    print("flips/s    = strafe-key switches per second, from the buttons.")
    print("             Human on surf_demise: 0.77/s overall, 98.1%% phi in window.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
