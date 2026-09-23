"""Write reports/latest.md from the training log and recent replays."""

import argparse
import csv
import datetime
import glob
import math
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from rollout import N_ACTIONS

BLOCKS = " .:-=+*#%@"

CSTRIKE = os.path.join(r"C:\Program Files (x86)\Steam\steamapps\common",
                       "Counter-Strike Source", "cstrike")
DATA = os.path.join(CSTRIKE, "addons", "sourcemod", "data", "csai")
REPLAYBOT = os.path.join(CSTRIKE, "addons", "sourcemod", "data", "replaybot", "0")

def current_map(root):
    """Which map is being trained. The daemon publishes this; default for old setups."""
    f = os.path.join(root, "data", "map.txt")
    try:
        with open(f, encoding="utf-8-sig") as fh:
            name = fh.read().strip()
            if name:
                return name
    except OSError:
        pass
    return "surf_demise"

def human_reference(map_name):
    """The time to beat, taken from the states file the map's own replay produced."""
    path = os.path.join(DATA, "%s_states.txt" % map_name)
    try:
        with open(path) as fh:
            for line in fh:
                if line.startswith("#") and "clean_time=" in line:
                    return float(line.split("clean_time=")[1].split()[0])
                if not line.startswith("#"):
                    break
    except (OSError, ValueError, IndexError):
        pass
    return 0.0
def track_units(map_name):
    """Track length, read from the map's own track file."""
    path = os.path.join(DATA, "%s_track.txt" % map_name)
    try:
        with open(path) as fh:
            for line in fh:
                if line.startswith("#") and "length=" in line:
                    return float(line.split("length=")[1].split()[0])
                if not line.startswith("#"):
                    break
    except (OSError, ValueError, IndexError):
        pass
    return 0.0

def spark(vals, width=56):
    if not vals:
        return ""
    if len(vals) > width:
        step = len(vals) / float(width)
        vals = [sum(vals[int(i * step):max(int((i + 1) * step), int(i * step) + 1)]) /
                max(len(vals[int(i * step):max(int((i + 1) * step), int(i * step) + 1)]), 1)
                for i in range(width)]
    lo, hi = min(vals), max(vals)
    if hi - lo < 1e-12:
        return BLOCKS[0] * len(vals)
    return "".join(BLOCKS[min(int((v - lo) / (hi - lo) * (len(BLOCKS) - 1)), len(BLOCKS) - 1)]
                   for v in vals)

def load(path):
    if not os.path.exists(path):
        return []
    with io.open(path, encoding="utf-8") as fh:
        return list(csv.DictReader(fh))

def fmt_dur(secs):
    secs = int(secs)
    h, m = secs // 3600, (secs % 3600) // 60
    return ("%dh %02dm" % (h, m)) if h else ("%dm %02ds" % (m, secs % 60))

def replay_table():
    """Control-quality stats for the most recent bot replays, next to the human."""
    try:
        from replayqa import analyse
    except ImportError:
        return []
    bots = sorted(glob.glob(os.path.join(DATA, "replays", "*.replay")),
                  key=os.path.getmtime, reverse=True)[:3]
    human = os.path.join(REPLAYBOT, "%s.replay" % MAP)
    rows = []
    for p in bots + ([human] if os.path.exists(human) else []):
        try:
            a = analyse(p)
            if a:
                a["is_human"] = (p == human)
                rows.append(a)
            else:
                rows.append({"name": os.path.basename(p), "error": "no frames read"})
        except Exception as e:
            rows.append({"name": os.path.basename(p),
                         "error": "%s: %s" % (type(e).__name__, e)})
    return rows

def eval_runs(limit_bytes=4000000):
    """Every eval run on record, as (gen, finished, fraction, seconds)."""
    path = os.path.join(ROOT, "logs", "eval_main.log")
    out = []
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as fh:
            if size > limit_bytes:
                fh.seek(size - limit_bytes)
                fh.readline()
            blob = fh.read().decode("utf-8", "replace")
    except OSError:
        return out
    cur = 0
    for line in blob.splitlines():
        h = re.search(r"eval: \d+ \w+ runs from state 0, policy gen (\d+)", line)
        if h:
            cur = int(h.group(1))
            continue
        m = re.search(r"eval run \d+/\d+: (\w+) at ([\d.]+)% in ([\d.]+)s", line)
        if m:
            out.append((cur, m.group(1).upper() == "FINISHED",
                        float(m.group(2)), float(m.group(3))))
    return out

def build(log_path, out_dir):
    rows = load(log_path)
    now = datetime.datetime.now()
    L = []
    A = L.append

    A("# CsAI training report")
    A("")
    A("generated %s" % now.strftime("%Y-%m-%d %H:%M:%S"))
    A("")
    if HUMAN_TIME > 0:
        A("Target: **%s**, 66 tick. Human reference **%.2f s**, 100%% of track."
          % (MAP, HUMAN_TIME))
    else:
        A("Target: **%s**, 66 tick. No reference run found for this map." % MAP)
    A("")

    if not rows:
        A("No training data yet (`data/train_log.csv` is empty).")
        return "\n".join(L)

    gen = [int(r["gen"]) for r in rows]
    gain = [float(r["mean_progress"]) * 100 for r in rows]
    best = [float(r["best_progress"]) * 100 for r in rows]
    ent = [float(r["entropy"]) for r in rows]
    steps = [int(r["steps"]) for r in rows]
    eps = [int(r["episodes"]) for r in rows]
    fin = [int(r["finished"]) for r in rows]
    wall, prevw = 0.0, None
    for r in rows:
        try:
            w = float(r["wall"])
        except (ValueError, KeyError):
            continue
        if prevw is not None and w > prevw:
            wall += w - prevw
        prevw = w

    w = min(max(len(gain) // 4, 5), 150)
    recent = sum(gain[-w:]) / min(w, len(gain))
    prior = sum(gain[-2 * w:-w]) / max(min(w, len(gain) - w), 1) if len(gain) > w else recent
    if recent > prior * 1.05:
        trend = "**improving** (%.2f%% -> %.2f%% over the last two windows)" % (prior, recent)
    elif recent < prior * 0.95:
        trend = "**regressing** (%.2f%% -> %.2f%%)" % (prior, recent)
    else:
        trend = "**flat** (%.2f%% -> %.2f%%)" % (prior, recent)

    A("## Complete runs")
    A("")
    A("Episodes that began at the start of the map and reached the end. Counted")
    A("by the learner, which sees every episode exactly once.")
    A("")
    fr = [r for r in rows if r.get("runs_from_start") not in (None, "")]
    if fr:
        fr_recent = fr[-40:]
        fr_tot = sum(int(r["runs_from_start"]) for r in fr_recent)
        fr_fin = sum(int(r["finished_from_start"]) for r in fr_recent)
        fr_times = [float(r["best_full_run_s"]) for r in fr_recent
                 if float(r.get("best_full_run_s") or 0) > 0]
        A("| | |")
        A("|---|---|")
        A("| over the last %d generations | %d runs from the start, %d finished |"
          % (len(fr_recent), fr_tot, fr_fin))
        A("| finish rate | **%.0f%%** |" % (100.0 * fr_fin / fr_tot if fr_tot else 0))
        if fr_times and HUMAN_TIME > 0:
            A("| fastest complete run | **%.2f s** against the human's %.2f s (%+.1f%%) |"
              % (min(fr_times), HUMAN_TIME, 100.0 * (min(fr_times) - HUMAN_TIME) / HUMAN_TIME))
        A("")
        A("| gen | runs | finished | rate | fastest |")
        A("|---:|---:|---:|---:|---:|")
        for r in fr[-10:]:
            fr_n = int(r["runs_from_start"])
            fr_f = int(r["finished_from_start"])
            b = float(r.get("best_full_run_s") or 0)
            A("| %s | %d | %d | %.0f%% | %s |"
              % (r["gen"], fr_n, fr_f, 100.0 * fr_f / fr_n if fr_n else 0,
                 ("%.2fs" % b) if b > 0 else "-"))
        A("")
    else:
        A("Not recorded yet - this is logged from the generation after the learner")
        A("was last restarted.")
        A("")
        A("Readings taken by sampling the batch files on disk are in")
        A("`data/finish_log.csv`, but they run low: the learner deletes a batch as")
        A("soon as it uses one, so what is left on disk is weighted towards")
        A("batches that were too stale to use - which came from older policies.")
        A("")

    ev = eval_runs()
    if ev:
        newest = max(g for g, _, _, _ in ev)
        ev = [e for e in ev if e[0] >= newest - 150]      # this policy, not an old one
    if len(ev) >= 8:
        fails = [f for _, ok, f, _ in ev if not ok]
        wins = [t for _, ok, _, t in ev if ok]
        gens = sorted(set(g for g, _, _, _ in ev))
        A("## Where the runs that fail end")
        A("")
        A("From the daemon's own evaluations - %d runs across policy generations"
          % len(ev))
        A("%d to %d, each from the start of the map with one of the recorded"
          % (gens[0], gens[-1]))
        A("prestrafes.")
        A("")
        A("| | |")
        A("|---|---|")
        A("| finished | %d of %d (%.0f%%) |" % (len(wins), len(ev), 100.0 * len(wins) / len(ev)))
        if wins:
            A("| fastest | %.2f s |" % min(wins))
        A("")
        if fails:
            buckets = {}
            for f in fails:
                buckets[min(int(f // 5) * 5, 95)] = buckets.get(min(int(f // 5) * 5, 95), 0) + 1
            top = max(buckets.values())
            A("```")
            for b in sorted(buckets):
                A("  %3d-%3d%%  %4d  %s" % (b, b + 5, buckets[b], "#" * int(40.0 * buckets[b] / top)))
            A("```")
            A("")

    A("## Summary")
    A("")
    A("| | |")
    A("|---|---|")
    A("| generations | %d (gen %d .. %d) |" % (len(rows), gen[0], gen[-1]))
    A("| episodes | %d |" % sum(eps))
    A("| agent steps | %s |" % format(sum(steps), ","))
    A("| wall time | %s |" % fmt_dur(wall))
    A("| throughput | %.0f steps/s end-to-end |" % (sum(steps) / wall if wall else 0))
    A("| best gain seen | %.2f%% of track (%.0f units) |" % (max(best), max(best) / 100 * TRACK_UNITS))
    A("| current gain | %.2f%% |" % gain[-1])
    A("| entropy | %.3f (max %.3f = uniform over %d actions) |"
      % (ent[-1], math.log(N_ACTIONS), N_ACTIONS))
    A("| completions | %d |" % sum(fin))
    A("| trend | %s |" % trend)
    A("")

    A("## Progress")
    A("")
    A("```")
    A("gain %%   %s   %.2f%% -> %.2f%%" % (spark(gain), gain[0], gain[-1]))
    A("best %%   %s   %.2f%% -> %.2f%%" % (spark(best), best[0], best[-1]))
    A("entropy  %s   %.3f -> %.3f" % (spark(ent), ent[0], ent[-1]))
    A("```")
    A("")

    A("## Last 12 generations")
    A("")
    A("| gen | eps | steps | return | gain% | best% | dist | fell | fin | entropy |")
    A("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in rows[-12:]:
        A("| %s | %s | %s | %.1f | %.2f | %.2f | %.0f | %s | %s | %.3f |" % (
            r["gen"], r["episodes"], r["steps"], float(r["mean_return"]),
            float(r["mean_progress"]) * 100, float(r["best_progress"]) * 100,
            float(r["mean_progress"]) * TRACK_UNITS, r["fell"], r["finished"],
            float(r["entropy"])))
    A("")

    qa = replay_table()
    if qa:
        A("## Control quality")
        A("")
        A("`phi in window` is the share of strafing frames where the wish angle is")
        A("within (85,95) degrees of perpendicular - the only range where air")
        A("acceleration does anything at surf speed. A human sits at 98.1%.")
        A("")
        A("| replay | secs | flips/s | phi in window | med speed |")
        A("|---|---:|---:|---:|---:|")
        for a in qa:
            if a.get("error"):
                A("| %s | could not be read | | | %s |" % (a["name"], a["error"]))
                continue
            A("| %s%s | %.2f | %.2f | %.1f%% | %.0f |" % (
                a["name"], " *(human)*" if a.get("is_human") else "",
                a["secs"], a["flips_per_s"], a["phi_in_window"], a["speed_med"]))
        A("")

    A("## Other runs on record")
    A("")
    A("| log | generations | final gain |")
    A("|---|---:|---:|")
    for p in sorted(glob.glob(os.path.join(ROOT, "data", "train_log_*.csv"))):
        rr = load(p)
        if rr:
            A("| %s | %s | %.2f%% |" % (os.path.basename(p), rr[-1]["gen"],
                                        float(rr[-1]["mean_progress"]) * 100))
    A("")
    A("See `docs/experiments.md` for what each configuration changed and why.")

    return "\n".join(L)

MAP = current_map(ROOT)
HUMAN_TIME = human_reference(MAP)
TRACK_UNITS = track_units(MAP)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=os.path.join(ROOT, "data", "train_log.csv"))
    ap.add_argument("--out", default=os.path.join(ROOT, "reports"))
    ap.add_argument("--print", dest="do_print", action="store_true")
    ap.add_argument("--snapshot", action="store_true", help="also keep a timestamped copy")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    text = build(args.log, args.out)

    latest = os.path.join(args.out, "latest.md")
    with io.open(latest, "w", encoding="utf-8") as fh:
        fh.write(text)

    if args.snapshot:
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M")
        with io.open(os.path.join(args.out, "report_%s.md" % stamp), "w", encoding="utf-8") as fh:
            fh.write(text)
        # Keep the last two days of snapshots.
        for old in sorted(glob.glob(os.path.join(args.out, "report_*.md")))[:-576]:
            try:
                os.remove(old)
            except OSError:
                pass

    if args.do_print:
        print(text)
    else:
        print("wrote %s" % latest)
    return 0

if __name__ == "__main__":
    sys.exit(main())
