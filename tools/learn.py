"""PPO learner. Consumes trajectory batches, publishes weights each generation."""

import argparse
import os
import shutil
import sys
import time

# The networks are small: one thread beats a thread pool at this size.
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")

import numpy as np

from ppo import (Policy, Value, Adam, compute_gae, ppo_update, write_weights, value_raw,
                 fit_inputs, POL_TOTAL)
from rollout import (Track, read_batch, episode_obs, episode_mask, mask_bool,
                     load_state_arclengths, OUTCOME_NAMES, OBS_DIM, N_ACTIONS, EP_FINISHED)

CSTRIKE = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrike"
DATA    = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai")
OUTDIR  = os.path.join(DATA, "out")
WEIGHTS = os.path.join(DATA, "weights.txt")
TICK    = 0.015

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
        with open(path, encoding="utf-8-sig") as fh:
            for line in fh:
                p = line.split()
                if len(p) == 2:
                    info[p[0]] = p[1]
    except OSError:
        pass
    return info

def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--batches", type=int, default=1000, help="batches to consume before stopping")
    ap.add_argument("--track", default=os.path.join(DATA, "surf_demise_track.txt"))
    ap.add_argument("--states", default=os.path.join(DATA, "surf_demise_states.txt"))
    ap.add_argument("--data", default=DATA, help="where each map's track and states live")
    ap.add_argument("--outdir", default=OUTDIR)
    ap.add_argument("--weights", default=WEIGHTS)
    ap.add_argument("--ckpt", default=os.path.join(os.path.dirname(__file__), "..", "data", "ckpt.npz"))
    ap.add_argument("--log", default=os.path.join(os.path.dirname(__file__), "..", "data", "train_log.csv"))
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--vlr", type=float, default=1e-3)
    ap.add_argument("--gamma", type=float, default=0.997, help="discount; 0.997 is ~10s at frameskip 2")
    ap.add_argument("--lam", type=float, default=0.95)
    ap.add_argument("--clip", type=float, default=0.2)
    ap.add_argument("--ent-final", dest="ent_final", type=float, default=None,
                    help="anneal the entropy bonus linearly to this")
    ap.add_argument("--ent-anneal", dest="ent_anneal", type=int, default=2000,
                    help="generations over which --ent reaches --ent-final")
    ap.add_argument("--ent", type=float, default=0.003, help="entropy bonus")
    ap.add_argument("--epochs", type=int, default=4)
    ap.add_argument("--minibatch", type=int, default=4096)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--frameskip", type=int, default=2, help="ticks per decision, for run times")
    ap.add_argument("--max-lag", dest="max_lag", type=int, default=12,
                    help="drop batches this many generations old; 0 keeps all")
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--kl-ref", dest="kl_ref", type=float, default=0.0,
                    help="penalty on moving away from the anchor policy; 0 disables")
    ap.add_argument("--ref-ckpt", dest="ref_ckpt",
                    default=os.path.join(os.path.dirname(__file__), "..", "data", "ckpt_anchor.npz"))
    ap.add_argument("--keep", action="store_true", help="keep consumed batch files")
    ap.add_argument("--timeout", type=float, default=600.0, help="seconds to wait for a batch")
    ap.add_argument("--out", help="write all output to this file instead of the console")
    args = ap.parse_args()

    if args.out:
        fh = open(args.out, "a", encoding="utf-8", buffering=1)
        sys.stdout = sys.stderr = fh

    rng = np.random.default_rng(args.seed)
    # One track per map, loaded the first time a batch from that map arrives.
    # Batches name their map; older ones without a name use --track.
    maps = {}

    def map_data(name):
        if name not in maps:
            if name:
                tpath = os.path.join(args.data, "%s_track.txt" % name)
                spath = os.path.join(args.data, "%s_states.txt" % name)
            else:
                tpath, spath = args.track, args.states
            if not os.path.isfile(tpath):
                maps[name] = None
                print("no track for map '%s' at %s" % (name, tpath))
            else:
                t = Track(tpath)
                s = load_state_arclengths(t, spath) if os.path.isfile(spath) else np.zeros(0)
                maps[name] = (t, s)
                print("map %s: %d track points, %.0f units, %d start states"
                      % (name or "(default)", t.n, t.length, len(s)))
        return maps[name]

    map_data("")

    policy = Policy(np.random.default_rng(args.seed))
    value = Value(np.random.default_rng(args.seed + 1))
    pol_opt = Adam(policy.shapes(), lr=args.lr)
    val_opt = Adam(value.shapes(), lr=args.vlr)
    gen = 0
    ent_gen0 = None            # generation the entropy anneal counts from

    if args.resume and os.path.exists(args.ckpt):
        # Read and closed at once: an open handle stops Windows replacing the file.
        with np.load(args.ckpt) as f:
            z = {k: f[k] for k in f.files}
        # A checkpoint from before the observation grew gets zero weights on the
        # new inputs, so it acts exactly as before until it learns to use them.
        for key in ("p0", "v0"):
            if key in z and z[key].ndim == 2 and z[key].shape[1] != OBS_DIM:
                print("checkpoint %s: %s takes %d inputs, widening to %d"
                      % (args.ckpt, key, z[key].shape[1], OBS_DIM))
                z[key] = fit_inputs(z[key], OBS_DIM)
        for i, p in enumerate(policy.params()):
            if z["p%d" % i].shape != p.shape:
                print("checkpoint %s does not fit this build: policy tensor %d is "
                      "%s, expected %s" % (args.ckpt, i, z["p%d" % i].shape, p.shape))
                print("obs %d, actions %d. Migrate the checkpoint or start fresh."
                      % (OBS_DIM, N_ACTIONS))
                return 1
        for i, p in enumerate(value.params()):
            key = "v%d" % i
            if key not in z:
                print("checkpoint %s is missing %s: it has %d value tensors, this "
                      "build needs %d."
                      % (args.ckpt, key, sum(k[0] == 'v' for k in z),
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
        if "ent_gen0" in z:
            ent_gen0 = int(z["ent_gen0"])
        if "ret_mean" in z:
            value.ret_mean = float(z["ret_mean"])
            value.ret_var = float(z["ret_var"])
            value.ret_count = float(z["ret_count"])
        print("resumed from %s at generation %d" % (args.ckpt, gen))

    if ent_gen0 is None:
        ent_gen0 = gen

    ref_policy = None
    if args.kl_ref > 0.0 and os.path.exists(args.ref_ckpt):
        ref_policy = Policy(np.random.default_rng(args.seed))
        with np.load(args.ref_ckpt) as z:
            for i, p in enumerate(ref_policy.params()):
                w = z["p%d" % i]
                p[...] = fit_inputs(w, OBS_DIM) if i == 0 else w
        print("anchored to %s (kl_ref %.4f)" % (args.ref_ckpt, args.kl_ref))
    elif args.kl_ref > 0.0:
        print("no anchor at %s - running unanchored" % args.ref_ckpt)

    HEADER = ("gen,batch,episodes,steps,mean_return,mean_progress,best_progress,"
              "fell,finished,timeout,stuck,entropy,kl,clipfrac,val_loss,kl_ref,wall,"
              "runs_from_start,finished_from_start,best_full_run_s,median_full_run_s,map")

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

        batch_map = info.get("map", "")
        md = map_data(batch_map)
        if md is None:
            processed.add(batch_key)
            continue
        track, state_s = md

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

        ent_now = args.ent
        if args.ent_final is not None and args.ent_anneal > 0:
            frac = min(max(gen - ent_gen0, 0) / float(args.ent_anneal), 1.0)
            ent_now = args.ent + (args.ent_final - args.ent) * frac

        t0 = time.time()

        obs_l, act_l, olp_l, mask_l, used = [], [], [], [], []
        counts = {"fell": 0, "finished": 0, "timeout": 0, "stuck": 0}
        progs = []

        run0 = fin0 = 0
        fin0_steps = []

        for ep in eps:
            if ep.n == 0:
                continue
            counts[OUTCOME_NAMES.get(ep.outcome, "stuck")] = \
                counts.get(OUTCOME_NAMES.get(ep.outcome, "stuck"), 0) + 1

            obs_l.append(episode_obs(track, ep))
            act_l.append(ep.steps[:, 6].astype(np.int64))
            olp_l.append(ep.steps[:, 7])
            mask_l.append(episode_mask(ep))
            used.append(ep)
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
        mask = mask_bool(np.concatenate(mask_l))

        # One value pass for the whole batch, then advantages per episode.
        v_all = value_raw(value, obs)
        adv_l, ret_l = [], []
        off = 0
        for ep in used:
            v = v_all[off:off + ep.n]
            off += ep.n
            done = np.zeros(ep.n)
            done[-1] = 1.0 if ep.terminal else 0.0
            last_v = 0.0 if ep.terminal else float(v[-1])
            a_, r_ = compute_gae(ep.steps[:, 8], v, done, last_value=last_v,
                                 gamma=args.gamma, lam=args.lam)
            adv_l.append(a_)
            ret_l.append(r_)
        adv = np.concatenate(adv_l)
        ret = np.concatenate(ret_l)

        value.update_ret_stats(ret)

        stats = ppo_update(policy, value, pol_opt, val_opt, obs, act, olp, adv, ret,
                           epochs=args.epochs, minibatch=args.minibatch,
                           clip=args.clip, ent_coef=ent_now, rng=rng,
                           ref_policy=ref_policy, kl_ref_coef=args.kl_ref, mask=mask)

        gen += 1
        write_weights(args.weights, policy, gen)

        mean_ret = float(np.mean([ep.steps[:, 8].sum() for ep in eps if ep.n]))
        mean_prog = float(np.mean(progs)) if progs else 0.0
        best_prog = float(np.max(progs)) if progs else 0.0
        wall = time.time() - t_start

        best_full = (min(fin0_steps) * args.frameskip * TICK) if fin0_steps else 0.0
        med_full = (float(np.median(fin0_steps)) * args.frameskip * TICK) if fin0_steps else 0.0

        print("gen %-4d %s %s | eps %3d steps %6d | ret %7.2f | gain %5.2f%% (best %5.2f%%) | "
              "fell %3d fin %2d to %2d stuck %3d | H %.3f kl %+.4f clip %.3f vl %.3f | full %d/%d %.2fs | H* %.4f | %.1fs upd %.2fs"
              % (gen, batch_map or "-", stem, len(eps), obs.shape[0], mean_ret, mean_prog * 100, best_prog * 100,
                 counts["fell"], counts["finished"], counts["timeout"], counts["stuck"],
                 stats["entropy"], stats["kl"], stats["clipfrac"], stats["val_loss"],
                 fin0, run0, med_full, ent_now, wall, time.time() - t0))

        print("%d,%s,%d,%d,%.4f,%.5f,%.5f,%d,%d,%d,%d,%.4f,%.5f,%.4f,%.4f,%.5f,%.1f,%d,%d,%.3f,%.3f,%s"
              % (gen, stem, len(eps), obs.shape[0], mean_ret, mean_prog, best_prog,
                 counts["fell"], counts["finished"], counts["timeout"], counts["stuck"],
                 stats["entropy"], stats["kl"], stats["clipfrac"], stats["val_loss"],
                 stats["kl_ref"], wall, run0, fin0, best_full, med_full, batch_map),
              file=logf)
        logf.flush()

        tmp = args.ckpt + ".tmp"
        np.savez(tmp, gen=gen, ent_gen0=ent_gen0,
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
