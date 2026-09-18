"""
Watch an unattended run, and act when it stops getting anywhere.

The failure this exists for: best progress sat between 73.02% and 73.07% for a
hundred generations - about 9600 episodes, not one of which got past a drop at
72% of surf_demise - while the policy put a probability of 0.0014 on the action
that actually clears it and chose it 0% of the time. A saturated policy cannot
sample its way out, so the run would have burned the whole night going nowhere
and looked busy the entire time.

So: if best progress has not improved by --improve over --window generations,
stop training, run unstick.py (which finds what clears the obstacle and teaches
it, rolling back on its own if a full run does not improve), and start training
again.

    python tools/autofix.py                 # watch and act
    python tools/autofix.py --dry-run       # report what it would do

Safety, because this edits a checkpoint with nobody watching:

  * the checkpoint is copied before every attempt, to data/ckpt_autofix_<gen>.npz
  * unstick.py verifies against full runs and restores its own backup if the
    frontier did not move
  * at most one attempt per --cooldown minutes, and --max-attempts in total
  * if training does not come back up afterwards, it stops trying and says so
  * everything, including every decision not to act, goes to logs/autofix.log
"""

import argparse
import datetime
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TRAIN_LOG = os.path.join(ROOT, "data", "train_log.csv")
CKPT = os.path.join(ROOT, "data", "ckpt.npz")
LOG = os.path.join(ROOT, "logs", "autofix.log")


def say(msg):
    line = "%s  %s" % (datetime.datetime.now().strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    try:
        os.makedirs(os.path.dirname(LOG), exist_ok=True)
        with open(LOG, "a", encoding="utf-8") as fh:
            fh.write(line + "\n")
    except OSError:
        pass


FINISHING = 0.95   # an episode that gains this much of the track ran it end to end


def rows():
    """(gen, best_progress, mean_progress) from the training log."""
    out = []
    try:
        with open(TRAIN_LOG, encoding="utf-8-sig") as fh:
            head = fh.readline().strip().split(",")
            try:
                gi = head.index("gen")
                bi = head.index("best_progress")
                mi = head.index("mean_progress")
            except ValueError:
                return out
            for line in fh:
                p = line.strip().split(",")
                if len(p) <= max(gi, bi, mi):
                    continue
                try:
                    out.append((int(p[gi]), float(p[bi]), float(p[mi])))
                except ValueError:
                    continue
    except OSError:
        pass
    return out


def phase(data, window):
    """Which number still has room to move?

    best_progress is the share of the track an episode gained, so it stops dead
    at 1.0 the moment the bot can run the map start to finish. From then on it
    is pinned and reads as a permanent plateau. That is not a guess: at gen 2842
    this fired with "best stuck at 99.97% for 150 generations" while the finish
    rate was climbing from 1 generation in 97 to 14 in 52.

    So while nothing finishes, watch the furthest anything got - that is the
    wall this tool was built for. Once runs do finish, watch the average
    instead, because what is left is doing it every time and doing it faster.
    """
    recent = data[-window:] if len(data) > window else data
    if any(b >= FINISHING for _, b, _ in recent):
        return "finishing", "average progress", [(g, m) for g, _, m in data]
    return "reaching", "best progress", [(g, b) for g, b, _ in data]


def plateaued(series, window, improve, label):
    """Has `series` failed to improve over the last `window` generations?"""
    if len(series) < window + 5:
        return False, "only %d generations logged" % len(series)
    recent = series[-window:]
    earlier = series[:-window]
    best_recent = max(b for _, b in recent)
    best_before = max(b for _, b in earlier)
    gain = (best_recent - best_before) * 100.0
    if gain >= improve:
        return False, "%s improved %+.2f points over the last %d gens" % (label, gain, window)
    return True, ("%s stuck at %.2f%% for %d generations (%+.2f points)"
                  % (label, best_recent * 100, window, gain))


def ps(args, timeout=900):
    return subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass"] + args,
                          cwd=ROOT, timeout=timeout,
                          capture_output=True, text=True)


def training_alive():
    r = subprocess.run(["tasklist"], capture_output=True, text=True)
    return r.stdout.lower().count("srcds_win64") > 0


def stop_training():
    ps(["-File", os.path.join(HERE, "daemon.ps1"), "-Stop"], timeout=300)
    for _ in range(40):
        if not training_alive():
            return True
        time.sleep(5)
    return not training_alive()


def start_training(daemon_args):
    subprocess.Popen(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                      "-File", os.path.join(HERE, "daemon.ps1")] + daemon_args,
                     cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        time.sleep(5)
        if training_alive():
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--window", type=int, default=150,
                    help="generations of no improvement before acting")
    ap.add_argument("--improve", type=float, default=0.75,
                    help="percentage points of best progress that counts as progress")
    ap.add_argument("--interval", type=int, default=120, help="seconds between checks")
    ap.add_argument("--cooldown", type=int, default=45, help="minutes between attempts")
    ap.add_argument("--max-attempts", dest="max_attempts", type=int, default=6)
    ap.add_argument("--hours", type=float, default=12.0, help="how long to keep watching")
    ap.add_argument("--map", default="surf_demise")
    ap.add_argument("--frameskip", type=int, default=2)
    ap.add_argument("--dry-run", dest="dry", action="store_true")
    # -Power high, not max. Eleven actors produce more than twice what the
    # learner can consume, and take the cores it needs to consume them: six
    # actors measured 38.5M steps/hour against eleven actors' 32.4M. Restarting
    # into max would quietly halve the speed of whatever it was trying to fix.
    ap.add_argument("--daemon-args", dest="daemon_args", default=
                    "-Power high -FrameSkip 2 -StateMix 0.3 -StateLo 0.73 -StateHi 0.83 "
                    "-Entropy 0.01 -TimeCost 0.08 -TimeBonus 30 -TrimCost 0.05 -SwitchCost 0.40")
    args = ap.parse_args()

    dargs = ["-Map", args.map] + args.daemon_args.split()
    say("watching: act after %d generations without %+.2f points of best progress"
        % (args.window, args.improve))

    deadline = time.time() + args.hours * 3600
    attempts = 0
    last_attempt = 0.0
    last_note = 0.0

    while time.time() < deadline:
        time.sleep(args.interval)
        data = rows()
        ph, label, series = phase(data, args.window)
        stuck, why = plateaued(series, args.window, args.improve, label)
        if not stuck:
            continue

        if ph == "finishing":
            # unstick.py finds the one action that clears an obstacle. There is
            # no obstacle left - runs are reaching the end - so there is nothing
            # for it to find, and stopping training to let it look costs four
            # minutes of eleven actors for nothing.
            if time.time() - last_note >= args.cooldown * 60:
                say("%s - but runs are finishing the map, so what is left is "
                    "consistency and time, not a wall. Training left alone." % why)
                last_note = time.time()
            continue

        if attempts >= args.max_attempts:
            say("PLATEAU: %s - but %d attempts already made, leaving it alone" % (why, attempts))
            time.sleep(args.cooldown * 60)
            continue
        if time.time() - last_attempt < args.cooldown * 60:
            continue

        say("PLATEAU: %s" % why)
        if args.dry:
            say("  dry run: would stop training, run unstick.py, and restart")
            last_attempt = time.time()
            attempts += 1
            continue

        gen = data[-1][0]
        backup = os.path.join(ROOT, "data", "ckpt_autofix_%d.npz" % gen)
        try:
            shutil.copyfile(CKPT, backup)
            say("  checkpoint backed up to %s" % os.path.basename(backup))
        except OSError as e:
            say("  could not back up the checkpoint (%s) - not touching anything" % e)
            last_attempt = time.time()
            continue

        if not stop_training():
            say("  training would not stop - not running unstick, leaving it as it is")
            last_attempt = time.time()
            attempts += 1
            continue

        say("  running unstick.py")
        r = subprocess.run([sys.executable, os.path.join(HERE, "unstick.py"),
                            "--map", args.map, "--frameskip", str(args.frameskip),
                            "--episodes", "14", "--eval-runs", "5", "--verify-runs", "12"],
                           cwd=ROOT, capture_output=True, text=True, timeout=5400)
        for line in (r.stdout or "").splitlines():
            if line.strip():
                say("    " + line.rstrip())
        if r.returncode != 0 and (r.stderr or "").strip():
            say("    unstick failed: " + (r.stderr or "").strip()[-400:])

        if start_training(dargs):
            say("  training restarted")
        else:
            say("  TRAINING DID NOT COME BACK UP - stopping, needs a human")
            return 1

        attempts += 1
        last_attempt = time.time()

    say("watch finished after %.1f hours, %d attempt(s)" % (args.hours, attempts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
