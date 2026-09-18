"""PPO learner. Consumes trajectory batches, publishes weights each generation."""

import argparse
import os
import shutil
import sys
import time

import numpy as np

from ppo import Policy, Value, Adam, compute_gae, ppo_update, write_weights, POL_TOTAL
from rollout import (Track, read_batch, episode_obs, load_state_arclengths,
                     OUTCOME_NAMES, OBS_DIM, N_ACTIONS, EP_FINISHED)

CSTRIKE = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrike"
DATA    = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai")
OUTDIR  = os.path.join(DATA, "out")
WEIGHTS = os.path.join(DATA, "weights.txt")

def find_next_batch(outdir, processed):
    """Oldest completed batch we have not consumed yet."""
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
    ap.add_argument("--ent-final", dest="ent_final", type=float, default=None,
                    help="anneal the entropy bonus linearly to this over --ent-anneal "
                         "generations. Exploration is worth paying for while the policy "
                         "still has somewhere to go; once it has converged on a route "
                         "the same bonus is just noise in the execution. Measured at "
                         "gen 8800: every run-to-run difference in finish time came "
                         "from the opening, not from the sampling - the deterministic "
                         "policy repeats a run tick for tick.")
    ap.add_argument("--ent-anneal", dest="ent_anneal", type=int, default=2000,
                    help="generations over which --ent reaches --ent-final")
    ap.add_argument("--ent", type=float, default=0.003,
                    help="0.01 kept entropy at 2.31 of a max 3.22 after 4.7M steps - the "
                         "policy never committed to a line. Surf needs sustained precise "
                         "control, so it needs less exploration pressure than the default.")
    ap.add_argument("--epochs", type=int, default=4)
    ap.add_argument("--minibatch", type=int, default=4096)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--frameskip", type=int, default=2,
                    help="the actors' decision rate, only used to turn a finishing "
                         "episode's step count into seconds")
    ap.add_argument("--max-lag", dest="max_lag", type=int, default=12,
                    help="drop batches produced by a policy this many generations "
                         "behind. With sync on, one consumed batch unblocks every "
                         "actor at once, so the queue grows and the oldest entries "
                         "become far too off-policy for PPO's trust region to mean "
                         "anything. 0 disables the check.")
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--kl-ref", dest="kl_ref", type=float, default=0.03,
                    help="penalty on moving away from the anchor policy; 0 disables")
    # Anchor against the best known policy, not the behaviour clone.
    ap.add_argument("--ref-ckpt", dest="ref_ckpt",
                    default=os.path.join(os.path.dirname(__file__), "..", "data", "ckpt_anchor.npz"))
    ap.add_argument("--keep", action="store_true", help="keep consumed batch files")
    ap.add_argument("--timeout", type=float, default=600.0, help="seconds to wait for a batch")
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    track = Track(args.track)
    print("track: %d points, %.0f units" % (track.n, track.length))

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
            if z["p%d" % i].shape != p.shape:
                print("checkpoint %s does not fit this build: policy tensor %d is "
                      "%s, expected %s" % (args.ckpt, i, z["p%d" % i].shape, p.shape))
                print("obs %d, actions %d. Migrate the checkpoint or start fresh."
                      % (OBS_DIM, N_ACTIONS))
                return 1
        for i, p in enumerate(value.params()):
            key = "v%d" % i
            if key not in z.files:
                print("checkpoint %s is missing %s: it has %d value tensors, this "
                      "build needs %d. The critic was written incompletely."
                      % (args.ckpt, key, sum(k[0] == 'v' for k in z.files),
                         len(value.params())))
                return 1
            if z[key].shape != p.shape:
                print("checkpoint %s: value tensor %d is %s, expected %s"
                      % (args.ckpt, i, z[key].shape, p.shape))
                return 1
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

    ent_gen0 = gen

    ref_policy = None
    if args.kl_ref > 0.0 and os.path.exists(args.ref_ckpt):
        z = np.load(args.ref_ckpt)
        ref_policy = Policy(np.random.default_rng(args.seed))
        for i, p in enumerate(ref_policy.params()):
            p[...] = z["p%d" % i]
        print("anchored to %s (kl_ref %.4f)" % (args.ref_ckpt, args.kl_ref))
    elif args.kl_ref > 0.0:
        print("no anchor at %s - running unanchored" % args.ref_ckpt)

    HEADER = ("gen,batch,episodes,steps,mean_return,mean_progress,best_progress,"
              "fell,finished,timeout,stuck,entropy,kl,clipfrac,val_loss,kl_ref,wall,"
              "runs_from_start,finished_from_start,best_full_run_s,median_full_run_s")

    os.makedirs(os.path.dirname(os.path.abspath(args.log)), exist_ok=True)
    new_log = not os.path.exists(args.log)

    if not new_log:
        try:
            with open(args.log, encoding="utf-8-sig") as fh:
                lines = fh.read().splitlines()
            if lines and lines[0] != HEADER:
                want = HEADER.count(",") + 1
                have = lines[0].count(",") + 1
                if have < want:
                    pad = "," * (want - have)
                    fixed = [HEADER] + [l + pad for l in lines[1:] if l.strip()]
                    with open(args.log, "w", encoding="utf-8", newline="\n") as fh:
                        fh.write("\n".join(fixed) + "\n")
                    print("train log: added %d column(s), padded %d existing rows"
                          % (want - have, len(fixed) - 1))
        except OSError as e:
            print("could not check the train log header (%s)" % e)

    logf = open(args.log, "a")
    if new_log:
        print(HEADER, file=logf)
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

        # Produced against a different centerline than the learner is using.
        batch_track = float(info.get("track_length", 0.0) or 0.0)
        if batch_track > 0.0 and abs(batch_track - track.length) > 1.0:
            print("skip %s: built against a track of %.1f units, this learner has "
                  "%.1f - regenerate the track or restart the actors so they agree"
                  % (stem, batch_track, track.length))
            processed.add(batch_key)
            if not args.keep:
                for pth in (binp, donep):
                    try:
                        os.remove(pth)
                    except OSError:
                        pass
            continue

        # Too far behind the current policy to be worth an update.
        batch_gen = int(info.get("gen", gen))
        if args.max_lag > 0 and gen - batch_gen > args.max_lag:
            print("skip %s: policy gen %d is %d behind" % (stem, batch_gen, gen - batch_gen))
            processed.add(batch_key)
            if not args.keep:
                for pth in (binp, donep):
                    try:
                        os.remove(pth)
                    except OSError:
                        pass
            continue

        # Linear anneal from --ent to --ent-final, measured from the generation
        # this learner started at, so a restart does not begin the schedule again.
        ent_now = args.ent
        if args.ent_final is not None and args.ent_anneal > 0:
            frac = min(max(gen - ent_gen0, 0) / float(args.ent_anneal), 1.0)
            ent_now = args.ent + (args.ent_final - args.ent) * frac

        t0 = time.time()

        obs_l, act_l, olp_l, adv_l, ret_l = [], [], [], [], []
        counts = {"fell": 0, "finished": 0, "timeout": 0, "stuck": 0}
        progs = []

        run0 = fin0 = 0
        fin0_steps = []

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

            # cutoffs bootstrap from the last observed state; true
            # terminals contribute nothing beyond the episode
            last_v = 0.0 if ep.terminal else float(v[-1])
            adv, ret = compute_gae(rew, v, done, last_value=last_v,
                                   gamma=args.gamma, lam=args.lam)

            obs_l.append(o)
            act_l.append(ep.steps[:, 6].astype(np.int64))
            olp_l.append(ep.steps[:, 7])
            adv_l.append(adv)
            ret_l.append(ret)
            if ep.start_state == 0:
                run0 += 1
                if ep.outcome == EP_FINISHED:
                    fin0 += 1
                    fin0_steps.append(ep.n)

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
                           clip=args.clip, ent_coef=ent_now, rng=rng,
                           ref_policy=ref_policy, kl_ref_coef=args.kl_ref)

        gen += 1
        write_weights(args.weights, policy, gen)

        mean_ret = float(np.mean([ep.steps[:, 8].sum() for ep in eps if ep.n]))
        mean_prog = float(np.mean(progs)) if progs else 0.0
        best_prog = float(np.max(progs)) if progs else 0.0
        wall = time.time() - t_start

        best_full = (min(fin0_steps) * args.frameskip / 66.67) if fin0_steps else 0.0
        med_full = (float(np.median(fin0_steps)) * args.frameskip / 66.67) if fin0_steps else 0.0

        print("gen %-4d %s | eps %3d steps %6d | ret %7.2f | gain %5.2f%% (best %5.2f%%) | "
              "fell %3d fin %2d to %2d stuck %3d | H %.3f kl %+.4f clip %.3f vl %.3f | full %d/%d %.2fs | H* %.4f | %.1fs upd %.2fs"
              % (gen, stem, len(eps), obs.shape[0], mean_ret, mean_prog * 100, best_prog * 100,
                 counts["fell"], counts["finished"], counts["timeout"], counts["stuck"],
                 stats["entropy"], stats["kl"], stats["clipfrac"], stats["val_loss"],
                 fin0, run0, med_full, ent_now, wall, time.time() - t0))

        print("%d,%s,%d,%d,%.4f,%.5f,%.5f,%d,%d,%d,%d,%.4f,%.5f,%.4f,%.4f,%.5f,%.1f,%d,%d,%.3f,%.3f"
              % (gen, stem, len(eps), obs.shape[0], mean_ret, mean_prog, best_prog,
                 counts["fell"], counts["finished"], counts["timeout"], counts["stuck"],
                 stats["entropy"], stats["kl"], stats["clipfrac"], stats["val_loss"],
                 stats["kl_ref"], wall, run0, fin0, best_full, med_full),
              file=logf)
        logf.flush()

        tmp = args.ckpt + ".tmp"
        np.savez(tmp, gen=gen,
                 ret_mean=value.ret_mean, ret_var=value.ret_var, ret_count=value.ret_count,
                 **{"p%d" % i: p for i, p in enumerate(policy.params())},
                 **{"v%d" % i: p for i, p in enumerate(value.params())})
        if not tmp.endswith(".npz"):
            tmp += ".npz"          # numpy appends the extension when it is absent
        try:
            if os.path.exists(args.ckpt):
                shutil.copyfile(args.ckpt, args.ckpt + ".prev")
        except OSError:
            pass
        for attempt in range(20):
            try:
                os.replace(tmp, args.ckpt)
                break
            except PermissionError:
                # Windows: something else has the file open for a moment.
                time.sleep(0.1)
        else:
            print("could not move the checkpoint into place - it is still at %s" % tmp)

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
