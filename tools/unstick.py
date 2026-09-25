"""Find the action that clears an obstacle, teach it, verify, roll back on failure."""

import argparse
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from ppo import Policy, Value, Adam, log_softmax, write_weights
from rollout import Track, OBS_DIM, N_ACTIONS

from game import GAME
CSTRIKE = os.path.join(GAME, "cstrike")
DATA = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai")
OUT = os.path.join(DATA, "out")
HIDDEN = os.path.join(HERE, "hidden.ps1")
CREATE_NO_WINDOW = 0x08000000

N_TRIMS = (N_ACTIONS - 1) // 2          # the last action is coast
PROBE_PORT = "26800"
PROBE_ACTOR = "9"

# --------------------------------------------------------------- running ----

def run_srcds(extra, timeout=600):
    """Launch one windowless run and return its console output."""
    args = ["+csai_actor", PROBE_ACTOR,
            "+csai_bench_timescale", "20", "+csai_bench_quit", "1",
            "+csai_bench_delay", "8"] + extra
    quoted = ",".join("'%s'" % a.replace("'", "''") for a in args)
    ps = (". '%s'; Invoke-Srcds 'csai_unstick' %s @(%s) %d"
          % (HIDDEN, PROBE_PORT, quoted, timeout))
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", ps],
                       timeout=timeout + 60, capture_output=True, text=True,
                       creationflags=CREATE_NO_WINDOW)
    for junk in os.listdir(OUT):
        if junk.startswith("a%s_batch_" % PROBE_ACTOR):
            try:
                os.remove(os.path.join(OUT, junk))
            except OSError:
                pass
    return r.stdout or ""

def batch_run(map_name, frameskip, lo, hi, episodes=16, side=0, trim=-1,
              obsdump=0, budget=6000, deviation=600, devcost=0.5):
    """One batch of episodes started inside [lo, hi] of the reference run."""
    extra = ["+map", map_name,
             "+csai_train_batches", "1", "+csai_train_sync", "0",
             "+csai_batch", str(episodes), "+csai_frameskip", str(frameskip),
             "+csai_states", "1", "+csai_statemix", "1.0",
             "+csai_statelo", "%.4f" % lo, "+csai_statehi", "%.4f" % hi,
             "+csai_forceside", str(side), "+csai_forcetrim", str(trim),
             "+csai_budget", str(budget), "+csai_deviation", str(deviation),
             "+csai_devcost", str(devcost), "+csai_prestrafe", "1"]
    if obsdump:
        extra += ["+csai_obsdump", str(obsdump)]
    log = run_srcds(extra)
    m = re.search(r"batch 0 gen \d+ \| eps \d+ \(mid \d+\) \| return [\d.-]+ \| progress ([\d.]+)%", log)
    return float(m.group(1)) if m else None

def eval_runs(map_name, frameskip, runs=5, devcost=0.5):
    """Full runs from the start. Returns the progress each one reached."""
    log = run_srcds(["+map", map_name, "+csai_eval", str(runs),
                     "+csai_evalgreedy", "0", "+csai_frameskip", str(frameskip),
                     "+csai_devcost", str(devcost), "+csai_prestrafe", "1",
                     "+csai_budget", "6000", "+csai_deviation", "600"])
    return [float(x) for x in re.findall(r"eval run \d+/\d+: \w+ at ([\d.]+)%", log)]

# ------------------------------------------------------------ checkpoints ----

def checkpoints(map_name):
    """(time fraction, track fraction) for each recorded start state."""
    tr = Track(os.path.join(DATA, "%s_track.txt" % map_name))
    rows = []
    with open(os.path.join(DATA, "%s_states.txt" % map_name)) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append([float(x) for x in line.split()])
    out = []
    for r in rows:
        idx = tr.nearest(r[3:6], -1)
        s, _, _ = tr.project(r[3:6], idx)
        out.append((r[1], 100.0 * s / tr.length))
    return out, tr

def load_dump(path, tr):
    rows = [[float(x) for x in l.split()] for l in open(path) if len(l.split()) > 7]
    if not rows:
        return np.zeros((0, OBS_DIM)), np.zeros(0)
    a = np.array(rows)
    prog = []
    hint = -1
    prev = None
    for p in a[:, 0:3]:
        if prev is None or float(np.linalg.norm(p - prev)) > 200.0:
            hint = -1
        prev = p
        hint = tr.nearest(p, hint)
        s, _, _ = tr.project(p, hint)
        prog.append(100.0 * s / tr.length)
    return a[:, 6:], np.array(prog)

# ----------------------------------------------------------------- policy ----

def load_policy(ckpt):
    z = np.load(ckpt)
    pol = Policy(np.random.default_rng(0))
    for i, p in enumerate(pol.params()):
        p[...] = z["p%d" % i]
    return pol, z

def teach(pol, Xteach, action, Xanchor, epochs=150, lr=1e-4, anchor_weight=3.0):
    """Push one action at the stuck states; hold everything else where it is."""
    y_anchor = pol.forward(Xanchor)[0].argmax(1)
    X = np.vstack([Xteach, Xanchor])
    y = np.concatenate([np.full(len(Xteach), action), y_anchor])
    w = np.concatenate([np.ones(len(Xteach)), np.full(len(Xanchor), anchor_weight)])
    opt = Adam(pol.shapes(), lr=lr)
    rng = np.random.default_rng(0)
    for _ in range(epochs):
        order = rng.permutation(len(X))
        for s0 in range(0, len(X), 256):
            mb = order[s0:s0 + 256]
            o, a, ww = X[mb], y[mb], w[mb]
            lg, cache = pol.forward(o)
            p = np.exp(log_softmax(lg))
            d = p * ww[:, None]
            d[np.arange(len(mb)), a] -= ww
            d /= ww.sum()
            opt.step(pol.params(), pol.backward(cache, d))
    kept = float((pol.forward(Xanchor)[0].argmax(1) == y_anchor).mean())
    P = np.exp(log_softmax(pol.forward(Xteach)[0]))
    return P[:, action].mean(), kept

def save(ckpt, weights, pol, z):
    out = {"gen": int(z["gen"])}
    for k in ("ret_mean", "ret_var", "ret_count"):
        if k in z:
            out[k] = float(z[k])
    out.update({"p%d" % i: p for i, p in enumerate(pol.params())})
    nval = len(Value(np.random.default_rng(0)).params())
    out.update({"v%d" % i: z["v%d" % i] for i in range(nval)})
    np.savez(ckpt, **out)
    write_weights(weights, pol, int(z["gen"]))

# ------------------------------------------------------------------- main ----

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default="surf_demise")
    ap.add_argument("--frameskip", type=int, default=2)
    ap.add_argument("--ckpt", default=os.path.join(HERE, "..", "data", "ckpt.npz"))
    ap.add_argument("--weights", default=os.path.join(DATA, "weights.txt"))
    ap.add_argument("--episodes", type=int, default=16, help="episodes per forced action")
    ap.add_argument("--eval-runs", dest="eval_runs", type=int, default=5)
    ap.add_argument("--verify-runs", dest="verify_runs", type=int, default=12,
                    help="full runs used to decide whether to keep the change. "
                         "More than the diagnosis needs: teaching buys the new "
                         "skill by costing some approach quality, so the result is "
                         "high variance and a handful of runs will often miss the "
                         "one that gets through. Five runs of a policy that does "
                         "reach 81%% returned 34/40/52/81/34.")
    ap.add_argument("--probe-ticks", dest="probe_ticks", type=int, default=250,
                    help="tick budget per forced action. Short on purpose: the "
                         "question is which action gets through the obstacle just "
                         "ahead, not which single action could drive the rest of "
                         "the map - no constant action does that well, and a long "
                         "budget makes every one of them look bad.")
    ap.add_argument("--margin", type=float, default=1.5,
                    help="how many times the policy's own progress a forced action "
                         "must reach before it is worth teaching")
    ap.add_argument("--dry-run", dest="dry", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.ckpt):
        print("no checkpoint at %s" % args.ckpt)
        return 1

    cps, tr = checkpoints(args.map)
    print("== where does it stop? ==")
    before = eval_runs(args.map, args.frameskip, args.eval_runs)
    if not before:
        print("no evaluation runs completed - is the map or the build wrong?")
        return 1
    stuck = float(max(before))
    print("  full runs reached: %s" % ", ".join("%.1f%%" % v for v in before))
    print("  furthest any run got: %.1f%% of the track" % stuck)

    # the last checkpoint before the failure, so the bot arrives carrying speed
    usable = [(tf, tp) for tf, tp in cps if tp < stuck - 0.5]
    if not usable:
        print("no checkpoint sits before that point; nothing to practise from")
        return 1
    tf, tp = usable[-1]
    lo, hi = tf - 0.005, tf + 0.005
    print("  practising from the checkpoint at %.1f%% of the track" % tp)

    print()
    print("== what gets past it? ==")
    baseline = batch_run(args.map, args.frameskip, lo, hi, args.episodes,
                         budget=args.probe_ticks)
    if baseline is None:
        print("  could not measure the policy's own progress from there")
        return 1
    print("  policy's own choice: %.1f%%" % baseline)

    best = (None, baseline)
    for side in (1, 2):
        for trim in range(N_TRIMS):
            got = batch_run(args.map, args.frameskip, lo, hi, args.episodes,
                            side=side, trim=trim, budget=args.probe_ticks)
            if got is None:
                continue
            tag = "A" if side == 1 else "D"
            flag = ""
            if got > best[1]:
                best = ((side, trim), got)
                flag = "  <- best so far"
            print("  %s side, trim %-2d : %5.1f%%%s" % (tag, trim, got, flag))

    if best[0] is None or best[1] < baseline * args.margin:
        print()
        print("nothing beats the policy by %.1fx; this is not a case of one missing"
              % args.margin)
        print("action, so leave it to training rather than teaching it something.")
        return 0

    side, trim = best[0]
    action = (trim if side == 1 else N_TRIMS + trim)
    print()
    print("== teaching %s side trim %d (%.1f%% against %.1f%%) ==" %
          ("A" if side == 1 else "D", trim, best[1], baseline))
    if args.dry:
        print("dry run: stopping here")
        return 0

    # states where it is stuck, driven with the action that works, plus states
    # from the rest of the map to hold the policy still everywhere else
    batch_run(args.map, args.frameskip, lo, hi, 60, side=side, trim=trim,
              obsdump=4000, budget=args.probe_ticks)
    Xd, dp = load_dump(os.path.join(OUT, "obsdump.txt"), tr)
    batch_run(args.map, args.frameskip, 0.0, 0.0, 20, obsdump=4000)
    Xr, rp = load_dump(os.path.join(OUT, "obsdump.txt"), tr)

    lo_p, hi_p = tp - 0.3, stuck + 1.5
    Xteach = Xd[(dp >= lo_p) & (dp <= hi_p)]
    Xanchor = Xr[(rp < lo_p - 0.6) | (rp > hi_p + 0.6)]
    if len(Xteach) < 100 or len(Xanchor) < 100:
        print("not enough states captured (%d teach, %d anchor)" % (len(Xteach), len(Xanchor)))
        return 1
    print("  %d states in %.1f-%.1f%%, %d anchor states elsewhere"
          % (len(Xteach), lo_p, hi_p, len(Xanchor)))

    backup = args.ckpt.replace(".npz", "_before_unstick.npz")
    import shutil
    shutil.copyfile(args.ckpt, backup)
    pol, z = load_policy(args.ckpt)
    p_after, kept = teach(pol, Xteach, action, Xanchor)
    print("  p(action) at the stuck states: %.3f, rest of the map unchanged on %.1f%%"
          % (p_after, 100 * kept))
    save(args.ckpt, args.weights, pol, z)

    print()
    print("== did it help? ==")
    after = eval_runs(args.map, args.frameskip, args.verify_runs)
    print("  before: %s" % ", ".join("%.1f%%" % v for v in before))
    print("  after : %s" % ", ".join("%.1f%%" % v for v in after))
    if after and max(after) > max(before) + 0.5:
        print("  kept: furthest run %.1f%% -> %.1f%% (mean %.1f%% -> %.1f%%)"
              % (max(before), max(after),
                 float(np.mean(before)), float(np.mean(after))))
        print("  the approach is rougher now; training repairs that, and can only")
        print("  do so because there is finally something past the wall to reward.")
        print("  backup of the previous policy at %s" % backup)
        return 0

    print("  no improvement on a full run; rolling back")
    shutil.copyfile(backup, args.ckpt)
    pol, z = load_policy(args.ckpt)
    write_weights(args.weights, pol, int(z["gen"]))
    return 0

if __name__ == "__main__":
    sys.exit(main())
