"""
Generate a training report from the logs.

Written for the case where training ran unattended for hours and you want to know
what happened without reading a CSV. Produces `reports/latest.md` plus a
timestamped copy, and is safe to run while training is in flight.

    python tools/report.py                 # write reports/latest.md
    python tools/report.py --print         # also dump it to the console
"""

import argparse
import csv
import datetime
import glob
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

BLOCKS = " .:-=+*#%@"
HUMAN_TIME = 39.10
TRACK_UNITS = 135547.0


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
    cs = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrike"
    bots = sorted(glob.glob(os.path.join(cs, r"addons\sourcemod\data\csai\replays\*.replay")),
                  key=os.path.getmtime, reverse=True)[:3]
    human = os.path.join(cs, r"addons\sourcemod\data\replaybot\0\surf_demise.replay")
    rows = []
    for p in bots + ([human] if os.path.exists(human) else []):
        try:
            a = analyse(p)
            if a:
                a["is_human"] = (p == human)
                rows.append(a)
        except Exception:
            pass
    return rows


def build(log_path, out_dir):
    rows = load(log_path)
    now = datetime.datetime.now()
    L = []
    A = L.append

    A("# CsAI training report")
    A("")
    A("generated %s" % now.strftime("%Y-%m-%d %H:%M:%S"))
    A("")
    A("Target: **surf_demise**, 66 tick. Human reference **%.2f s**, 100%% of track." % HUMAN_TIME)
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
    fell = [int(r["fell"]) for r in rows]
    fin = [int(r["finished"]) for r in rows]
    wall = float(rows[-1]["wall"])

    # recent window vs the one before it: is it still improving?
    w = max(len(gain) // 4, 5)
    recent = sum(gain[-w:]) / min(w, len(gain))
    prior = sum(gain[-2 * w:-w]) / max(min(w, len(gain) - w), 1) if len(gain) > w else recent
    if recent > prior * 1.05:
        trend = "**improving** (%.2f%% -> %.2f%% over the last two windows)" % (prior, recent)
    elif recent < prior * 0.95:
        trend = "**regressing** (%.2f%% -> %.2f%%)" % (prior, recent)
    else:
        trend = "**flat** (%.2f%% -> %.2f%%)" % (prior, recent)

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
    A("| entropy | %.3f (max %.3f = uniform) |" % (ent[-1], 2.303))
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

    if args.do_print:
        print(text)
    else:
        print("wrote %s" % latest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
