"""One-line training summary."""

import argparse
import csv
import os
import sys

from game import CSTRIKE

BLOCKS = " .:-=+*#%@"

def spark(values, width=48):
    if not values:
        return ""
    if len(values) > width:
        # average into buckets rather than subsample, so spikes are not dropped
        step = len(values) / float(width)
        buckets = []
        for i in range(width):
            a = int(i * step)
            b = max(int((i + 1) * step), a + 1)
            chunk = values[a:b]
            buckets.append(sum(chunk) / len(chunk))
        values = buckets
    lo, hi = min(values), max(values)
    if hi - lo < 1e-12:
        return BLOCKS[0] * len(values)
    out = []
    for v in values:
        t = (v - lo) / (hi - lo)
        out.append(BLOCKS[min(int(t * (len(BLOCKS) - 1)), len(BLOCKS) - 1)])
    return "".join(out)

def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--log", default=os.path.join(here, "..", "data", "train_log.csv"))
    ap.add_argument("--tail", type=int, default=20)
    ap.add_argument("--map", default=None, help="default: data/map.txt")
    ap.add_argument("--track-length", type=float, default=None, help="default: from the map's track")
    args = ap.parse_args()

    # Several maps share one log; this is about one of them.
    mapname = args.map
    if not mapname:
        try:
            with open(os.path.join(here, "..", "data", "map.txt"), encoding="utf-8-sig") as fh:
                mapname = fh.read().strip()
        except OSError:
            pass
    mapname = mapname or "surf_demise"
    data = os.path.join(CSTRIKE, "addons", "sourcemod", "data", "csai")

    def header_value(path, key):
        try:
            with open(path, encoding="utf-8-sig") as fh:
                for line in fh:
                    if not line.startswith("#"):
                        break
                    if key in line:
                        return float(line.split(key)[1].split()[0])
        except (OSError, ValueError, IndexError):
            pass
        return None

    if args.track_length is None:
        args.track_length = header_value(os.path.join(data, "%s_track.txt" % mapname), "length=") or 0.0
    reference = header_value(os.path.join(data, "%s_states.txt" % mapname), "clean_time=")

    if not os.path.exists(args.log):
        print("no log at %s" % args.log)
        return 1

    with open(args.log) as fh:
        rows = list(csv.DictReader(fh))
    rows = [r for r in rows if r.get("map") in (None, "", mapname)]
    if not rows:
        print("no generations for %s in the log" % mapname)
        return 1
    print("map         : %s" % mapname)

    def col(name, cast=float):
        return [cast(r[name]) for r in rows]

    gen = col("gen", int)
    ret = col("mean_return")
    gain = col("mean_progress")
    best = col("best_progress")
    ent = col("entropy")
    eps = col("episodes", int)
    steps = col("steps", int)
    wall = col("wall")

    total_steps = sum(steps)
    print("generations : %d  (gen %d .. %d)" % (len(rows), gen[0], gen[-1]))
    print("episodes    : %d" % sum(eps))
    print("agent steps : %d" % total_steps)
    if wall[-1] > 0:
        print("wall        : %.1f s  (%.0f steps/s end-to-end incl. learner)"
              % (wall[-1], total_steps / wall[-1]))
    print()

    def line(name, vals, fmt="%.4f", scale=1.0):
        print("%-12s %s  %s -> %s" % (name, spark(vals),
                                      fmt % (vals[0] * scale), fmt % (vals[-1] * scale)))

    line("return", ret, "%.2f")
    line("gain %", gain, "%.3f", 100.0)
    line("best %", best, "%.3f", 100.0)
    line("entropy", ent, "%.4f")

    print()
    n = min(args.tail, len(rows))
    print("last %d generations:" % n)
    print("  gen   eps  steps   return    gain%    best%    dist(u)  fell  fin  entropy")
    for r in rows[-n:]:
        print("  %-5s %-4s %-7s %-9s %-7s %-7s %-9s %-5s %-4s %s"
              % (r["gen"], r["episodes"], r["steps"],
                 "%.2f" % float(r["mean_return"]),
                 "%.3f" % (float(r["mean_progress"]) * 100),
                 "%.3f" % (float(r["best_progress"]) * 100),
                 "%.0f" % (float(r["mean_progress"]) * args.track_length),
                 r["fell"], r["finished"],
                 "%.4f" % float(r["entropy"])))

    # a crude but honest read on whether it is still improving
    if len(ret) >= 6:
        half = len(ret) // 2
        early = sum(gain[:half]) / half
        late = sum(gain[half:]) / (len(gain) - half)
        print()
        if late > early * 1.05:
            print("trend: improving (mean gain %.3f%% -> %.3f%%)" % (early * 100, late * 100))
        elif late < early * 0.95:
            print("trend: REGRESSING (mean gain %.3f%% -> %.3f%%)" % (early * 100, late * 100))
        else:
            print("trend: flat (mean gain %.3f%% -> %.3f%%)" % (early * 100, late * 100))

    print()
    if reference:
        print("record to beat: %.3f s from leaving the start zone (%s)" % (reference, mapname))
    else:
        print("no record found for %s" % mapname)
    return 0

if __name__ == "__main__":
    sys.exit(main())
