"""Report end-to-end finish rate and times from the batch files on disk."""

import argparse
import datetime
import glob
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from rollout import read_batch, Track, EP_FINISHED, OUTCOME_NAMES
from learn import read_done

from game import CSTRIKE
DATA = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai")
OUT = os.path.join(DATA, "out")
TICKRATE = 66.67

def reference_time(states_path):
    try:
        with open(states_path, encoding="utf-8-sig") as fh:
            for line in fh:
                if line.startswith("#") and "clean_time=" in line:
                    return float(line.split("clean_time=")[1].split()[0])
                if not line.startswith("#"):
                    break
    except (OSError, ValueError, IndexError):
        pass
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default="surf_demise")
    ap.add_argument("--frameskip", type=int, default=2)
    ap.add_argument("--append", action="store_true", help="add a row to data/finish_log.csv")
    ap.add_argument("--quiet", action="store_true", help="one line only")
    args = ap.parse_args()

    tpath = os.path.join(DATA, "%s_track.txt" % args.map)
    if not os.path.isfile(tpath):
        print("no track for %s at %s" % (args.map, tpath))
        print("run:  python tools/setup_map.py %s" % args.map)
        return 1
    track = Track(tpath)
    if not track.n:
        print("the track file for %s is empty" % args.map)
        return 1
    ref = reference_time(os.path.join(DATA, "%s_states.txt" % args.map))

    # Several maps share one out directory; each batch's .done names its map.
    # A batch without one is still being written.
    files = []
    for f in sorted(glob.glob(os.path.join(OUT, "*batch_*.bin")), key=os.path.getmtime):
        if os.path.basename(f).startswith("a99_"):
            continue
        donep = os.path.splitext(f)[0] + ".done"
        if not os.path.exists(donep) or read_done(donep).get("map", "") != args.map:
            continue
        files.append(f)
    n = 0
    times = []
    deaths = []
    outcomes = {}
    for f in files:
        try:
            eps = read_batch(f)
        except (OSError, ValueError, IndexError):
            continue          # a batch still being written
        for e in eps:
            # start_state 0 is the beginning of the map. Everything else was
            # spawned partway along and cannot speak to an end-to-end run.
            if e.start_state != 0 or not e.n:
                continue
            n += 1
            name = OUTCOME_NAMES.get(e.outcome, "?")
            outcomes[name] = outcomes.get(name, 0) + 1
            if e.outcome == EP_FINISHED:
                times.append(e.n * args.frameskip / TICKRATE)
            else:
                deaths.append(e.best_s / track.length)

    if n == 0:
        print("no complete-run episodes on disk (batches %d)" % len(files))
        return 1

    rate = 100.0 * len(times) / n
    best = min(times) if times else 0.0
    med = float(np.median(times)) if times else 0.0
    line = ("%d runs from the start, %d finished (%.1f%%)" % (n, len(times), rate))
    if times:
        line += "  best %.2fs median %.2fs" % (best, med)
        if ref:
            line += "  (human %.2fs, %+.1f%%)" % (ref, 100.0 * (med - ref) / ref)
    print(line)

    if not args.quiet and deaths:
        d = np.array(deaths)
        print("the %d that did not: die at %.1f%% of the track on average, "
              "half of them between %.1f%% and %.1f%%"
              % (len(d), d.mean() * 100,
                 np.percentile(d, 25) * 100, np.percentile(d, 75) * 100))
        hist, edges = np.histogram(d, bins=20, range=(0.0, 1.0))
        top = hist.max()
        for c, lo in zip(hist, edges[:-1]):
            if c:
                print("   %3.0f-%3.0f%%  %5d  %s"
                      % (lo * 100, lo * 100 + 5, c, "#" * int(50.0 * c / top)))

    if args.append:
        path = os.path.join(ROOT, "data", "finish_log.csv")
        new = not os.path.exists(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "a", encoding="utf-8") as fh:
            if new:
                fh.write("time,runs,finished,rate,best_s,median_s,median_death\n")
            fh.write("%s,%d,%d,%.3f,%.3f,%.3f,%.4f\n"
                     % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                        n, len(times), rate, best, med,
                        float(np.median(deaths)) if deaths else 0.0))
        print("appended to data/finish_log.csv")
    return 0

if __name__ == "__main__":
    sys.exit(main())
