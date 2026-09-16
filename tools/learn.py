"""
The learner. Watches for trajectory batches from the plugin, runs PPO, and
publishes a new policy generation.

    plugin  ->  data/csai/out/batch_NNNN.bin + .done
    learner ->  data/csai/weights.txt   (generation N+1)

Run the actor in sync mode so it waits for each new policy:

    .\\tools\\train.ps1 -Batches 500 -Sync 1 -Timescale 80
    python tools/learn.py --batches 500

Usage notes:
  * GAE is computed per episode. Running it across the concatenation would
    bootstrap one episode's final value from the next episode's first state.
  * Timeout/stuck are cutoffs, not terminals, so they bootstrap; only falling and
    finishing are absorbing. See Episode.terminal in rollout.py.
"""

import argparse
import os
import shutil
import sys
import time

import numpy as np

from ppo import Policy, Value, Adam, compute_gae, ppo_update, write_weights, POL_TOTAL
from rollout import (Track, read_batch, episode_obs, load_state_arclengths,
                     OUTCOME_NAMES, OBS_DIM)

CSTRIKE = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrike"
DATA    = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai")
OUTDIR  = os.path.join(DATA, "out")
WEIGHTS = os.path.join(DATA, "weights.txt")


def find_next_batch(outdir, processed):
    """
    Oldest completed batch we have not consumed yet.

    Keyed by (name, mtime), not name alone. The actor restarts its batch counter
    at 0 every time it starts - which the daemon does on every power change - so a
    name-only key makes the learner skip the new batch_0000 as "already seen" and
    training silently stalls.

    Chosen by mtime rather than by name. Parallel actors write a0_batch_0001,
    a1_batch_0000, ... and "a0_batch_0001" sorts before "a1_batch_0000", so a
    name-ordered pick would always prefer actor 0 and let the others' batches
    age into staleness. Oldest-first is the order that keeps every actor's data
    equally fresh.
    """
    best = None       # (stem, key, mtime)
    try:
        names = os.listdir(outdir)
    except FileNotFoundError:
        return None
    for name in names:
        if not name.endswith(".done"):
            continue
        stem = name[:-5]
        binp = os.path.join(outdir, stem + ".bin")
        try:
            key = (stem, os.path.getmtime(os.path.join(outdir, name)))
        except OSError:
            continue
        if key in processed or not os.path.exists(binp):
            continue
        if best is None or (key[1], stem) < (best[2], best[0]):
            best = (stem, key, key[1])
    return best[:2] if best else None


def read_done(path):
    info = {}
    try:
        with open(path) as fh:
            for line in fh:
                p = line.split()
                if len(p) == 2:
                    info[p[0]] = p[1]
    except OSError:
        pass
    return info


def main():
    # stdout is block-buffered when redirected, which makes a backgrounded
    # learner look hung for minutes at a time. Line-buffer it.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--batches", type=int, default=1000, help="batches to consume before stopping")
    ap.add_argument("--track", default=os.path.join(DATA, "surf_demise_track.txt"))
    ap.add_argument("--states", default=os.path.join(DATA, "surf_demise_states.txt"))
    ap.add_argument("--outdir", default=OUTDIR)
    ap.add_argument("--weights", default=WEIGHTS)
    ap.add_argument("--ckpt", default=os.path.join(os.path.dirname(__file__), "..", "data", "ckpt.npz"))
    ap.add_argument("--log", default=os.path.join(os.path.dirname(__file__), "..", "data", "train_log.csv"))
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--vlr", type=float, default=1e-3)
    ap.add_argument("--gamma", type=float, default=0.997,
                    help="0.99 gives a 3.0s horizon at frameskip 2/66 tick, "
                         "shorter than an episode; 0.997 gives ~10s")
    ap.add_argument("--lam", type=float, default=0.95)
    ap.add_argument("--clip", type=float, default=0.2)
    ap.add_argument("--ent", type=float, default=0.003,
                    help="0.01 kept entropy at 2.31 of a max 3.22 after 4.7M steps - the "
                         "policy never committed to a line. Surf needs sustained precise "
                         "control, so it needs less exploration pressure than the default.")
    ap.add_argument("--epochs", type=int, default=4)
    ap.add_argument("--minibatch", type=int, default=4096)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--kl-ref", dest="kl_ref", type=float, default=0.03,
                    help="penalty on moving away from the cloned policy; 0 disables")
    ap.add_argument("--ref-ckpt", dest="ref_ckpt",
                    default=os.path.join(os.path.dirname(__file__), "..", "data", "ckpt_bc.npz"))
    ap.add_argument("--keep", action="store_true", help="keep consumed batch files")
    ap.add_argument("--timeout", type=float, default=600.0, help="seconds to wait for a batch")
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    track = Track(args.track)
    print("track: %d points, %.0f units" % (track.n, track.length))

    # best_s is an absolute position on the track, so progress has to be measured
    # against where the episode actually started - otherwise an episode spawned at
    # the 80% checkpoint that instantly falls reports 80% "progress".
    state_s = load_state_arclengths(track, args.states)
    print("start states: %d, arc lengths %.0f .. %.0f" %
          (len(state_s), state_s.min() if len(state_s) else 0,
           state_s.max() if len(state_s) else 0))

    policy = Policy(np.random.default_rng(args.seed))
    value = Value(np.random.default_rng(args.seed + 1))
    pol_opt = Adam(policy.shapes(), lr=args.lr)
    val_opt = Adam(value.shapes(), lr=args.vlr)
    gen = 0

    if args.resume and os.path.exists(args.ckpt):
        z = np.load(args.ckpt)
        for i, p in enumerate(policy.params()):
            p[...] = z["p%d" % i]
        for i, p in enumerate(value.params()):
            p[...] = z["v%d" % i]
        gen = int(z["gen"])
        if "ret_mean" in z:
            value.ret_mean = float(z["ret_mean"])
            value.ret_var = float(z["ret_var"])
            value.ret_count = float(z["ret_count"])
        print("resumed from %s at generation %d" % (args.ckpt, gen))

    ref_policy = None
    if args.kl_ref > 0.0 and os.path.exists(args.ref_ckpt):
        z = np.load(args.ref_ckpt)
        ref_policy = Policy(np.random.default_rng(args.seed))
        for i, p in enumerate(ref_policy.params()):
            p[...] = z["p%d" % i]
        print("anchored to %s (kl_ref %.4f)" % (args.ref_ckpt, args.kl_ref))
    elif args.kl_ref > 0.0:
        print("no anchor at %s - running unanchored" % args.ref_ckpt)

    os.makedirs(os.path.dirname(os.path.abspath(args.log)), exist_ok=True)
    new_log = not os.path.exists(args.log)
    logf = open(args.log, "a")
    if new_log:
        print("gen,batch,episodes,steps,mean_return,mean_progress,best_progress,"
              "fell,finished,timeout,stuck,entropy,kl,clipfrac,val_loss,kl_ref,wall", file=logf)
        logf.flush()

    # publish generation 1 so the actor starts from a real policy rather than its
    # own cold-start random init
    gen += 1
    write_weights(args.weights, policy, gen)
    print("published generation %d (%d floats) -> %s" % (gen, POL_TOTAL, args.weights))

    processed = set()
    consumed = 0
    t_start = time.time()

    while consumed < args.batches:
        found = find_next_batch(args.outdir, processed)
        if found is None:
            waited = 0.0
            while found is None and waited < args.timeout:
                time.sleep(0.25)
                waited += 0.25
                found = find_next_batch(args.outdir, processed)
            if found is None:
                print("no batch within %.0fs, stopping" % args.timeout)
                break
        stem, batch_key = found

        binp = os.path.join(args.outdir, stem + ".bin")
        donep = os.path.join(args.outdir, stem + ".done")

        # the plugin closes the .bin before writing .done, but be defensive about
        # a partially flushed file on a slow disk
        for _ in range(20):
            try:
                eps = read_batch(binp)
                break
            except (ValueError, OSError):
                time.sleep(0.1)
        else:
            print("could not read %s, skipping" % stem)
            processed.add(batch_key)
            continue

        info = read_done(donep)
        t0 = time.time()

        obs_l, act_l, olp_l, adv_l, ret_l = [], [], [], [], []
        counts = {"fell": 0, "finished": 0, "timeout": 0, "stuck": 0}
        progs = []

        for ep in eps:
            if ep.n == 0:
                continue
            counts[OUTCOME_NAMES.get(ep.outcome, "stuck")] = \
                counts.get(OUTCOME_NAMES.get(ep.outcome, "stuck"), 0) + 1

            o = episode_obs(track, ep)
            v, _ = value.forward_raw(o)
            rew = ep.steps[:, 8]
            done = np.zeros(ep.n)
            done[-1] = 1.0 if ep.terminal else 0.0

            # cutoffs bootstrap from the last state we actually saw; true
            # terminals contribute nothing beyond the episode
            last_v = 0.0 if ep.terminal else float(v[-1])
            adv, ret = compute_gae(rew, v, done, last_value=last_v,
                                   gamma=args.gamma, lam=args.lam)

            obs_l.append(o)
            act_l.append(ep.steps[:, 6].astype(np.int64))
            olp_l.append(ep.steps[:, 7])
            adv_l.append(adv)
            ret_l.append(ret)
            start_s = float(state_s[ep.start_state]) if ep.start_state < len(state_s) else 0.0
            gained = max(float(ep.best_s) - start_s, 0.0)
            progs.append(gained / track.length if track.length else 0.0)

        if not obs_l:
            processed.add(batch_key)
            continue

        obs = np.concatenate(obs_l)
        act = np.concatenate(act_l)
        olp = np.concatenate(olp_l)
        adv = np.concatenate(adv_l)
        ret = np.concatenate(ret_l)

        value.update_ret_stats(ret)

        stats = ppo_update(policy, value, pol_opt, val_opt, obs, act, olp, adv, ret,
                           epochs=args.epochs, minibatch=args.minibatch,
                           clip=args.clip, ent_coef=args.ent, rng=rng,
                           ref_policy=ref_policy, kl_ref_coef=args.kl_ref)

        gen += 1
        write_weights(args.weights, policy, gen)

        mean_ret = float(np.mean([ep.steps[:, 8].sum() for ep in eps if ep.n]))
        mean_prog = float(np.mean(progs)) if progs else 0.0
        best_prog = float(np.max(progs)) if progs else 0.0
        wall = time.time() - t_start

        print("gen %-4d %s | eps %3d steps %6d | ret %7.2f | gain %5.2f%% (best %5.2f%%) | "
              "fell %3d fin %2d to %2d stuck %3d | H %.3f kl %+.4f clip %.3f vl %.3f | %.1fs upd %.2fs"
              % (gen, stem, len(eps), obs.shape[0], mean_ret, mean_prog * 100, best_prog * 100,
                 counts["fell"], counts["finished"], counts["timeout"], counts["stuck"],
                 stats["entropy"], stats["kl"], stats["clipfrac"], stats["val_loss"],
                 wall, time.time() - t0))

        print("%d,%s,%d,%d,%.4f,%.5f,%.5f,%d,%d,%d,%d,%.4f,%.5f,%.4f,%.4f,%.5f,%.1f"
              % (gen, stem, len(eps), obs.shape[0], mean_ret, mean_prog, best_prog,
                 counts["fell"], counts["finished"], counts["timeout"], counts["stuck"],
                 stats["entropy"], stats["kl"], stats["clipfrac"], stats["val_loss"],
                 stats["kl_ref"], wall),
              file=logf)
        logf.flush()

        np.savez(args.ckpt, gen=gen,
                 ret_mean=value.ret_mean, ret_var=value.ret_var, ret_count=value.ret_count,
                 **{"p%d" % i: p for i, p in enumerate(policy.params())},
                 **{"v%d" % i: p for i, p in enumerate(value.params())})

        processed.add(batch_key)
        consumed += 1
        if not args.keep:
            for f in (binp, donep):
                try:
                    os.remove(f)
                except OSError:
                    pass

    logf.close()
    print("consumed %d batches, final generation %d" % (consumed, gen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
