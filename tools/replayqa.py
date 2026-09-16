"""
Replay quality analysis — does the bot steer like a player?

Finish time alone hides *how* a run was produced. These are the measures that
actually distinguish surfing from thrashing, and the one that matters most is
the wish-angle distribution.

Source air acceleration applies only the component of the horizontal wish vector
perpendicular to velocity: addspeed = wishspd - speed*cos(phi). At surf speed
(3614 u/s on surf_demise) that means any |phi| < 89.5 deg produces *zero*
acceleration, and phi near 180 brakes at up to sv_airaccelerate*maxspeed*frametime
= 720 u/s per tick. A human therefore sits at |phi| ~ 90 essentially always:
measured 98.1% of frames within (85, 95) degrees.

So "what fraction of frames are in the usable window" is a direct, physical
measure of whether a policy has found the control law at all.

    python tools/replayqa.py <a.replay> [b.replay ...]
"""

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

    yaw = np.array([f.ang[1] for f in fr])
    dyaw = np.array([math.remainder(float(yaw[i] - yaw[i - 1]), 360.0)
                     for i in range(1, len(yaw))])
    adyaw = np.abs(dyaw)

    # Large view jumps are not ordinary steering; exclude them so the jitter
    # percentiles describe the smooth part.
    big = adyaw > 90.0
    smooth = adyaw[~big]

    # A side switch is a change of STRAFE KEY, read from the buttons.
    #
    # This used to be counted as a >90 deg jump in view yaw, which measures
    # nothing: the view turns smoothly through a switch, only the key changes.
    # That version reported 0.00/s for every replay including the human's - who
    # demonstrably switches 10 times in the first 15.8% of this map - and the
    # bogus zero was taken as evidence the policy never switched sides.
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

    print("%-26s %6s %7s %8s %9s %10s %8s" %
          ("replay", "secs", "flips/s", "med|dyaw|", "p95|dyaw|", "phi in win", "med spd"))
    for a in rows:
        print("%-26s %6.2f %7.2f %8.2f %9.2f %9.1f%% %8.0f" %
              (a["name"], a["secs"], a["flips_per_s"], a["smooth_med"],
               a["smooth_p95"], a["phi_in_window"], a["speed_med"]))

    print()
    print("phi in win = %% of strafing frames with |phi| in (85,95) deg, i.e. in the")
    print("             only range where air acceleration does anything at surf speed.")
    print("flips/s    = strafe-key switches per second, from the buttons.")
    print("             Human on surf_demise: 0.77/s overall, 98.1%% phi in window.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
