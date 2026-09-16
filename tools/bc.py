"""
Behaviour cloning: train the policy to imitate the human's actions.

Why this exists. Progress reward alone taught the policy to micro-oscillate
+-90 degrees every couple of ticks - 19.79 side switches per second against a
human's 0.95 - because jitter tracks the reference line more tightly than real
technique does. That is not a training-time problem; it is what the reward asked
for, and more PPO entrenches it.

Cloning asks the other question: given this situation, what did the human do?
Their inputs carry the technique the reward cannot express - holding a key
through a curve and steering with the view, not strafing through tight ramp
sections, strafing on long airborne stretches.

Input is produced in-engine by csai_demo.inc, which replays the human's recorded
inputs through real physics and records (observation, action) at the policy's own
decision cadence. The observations therefore lie on the human's trajectory, which
is the only place these actions are valid.

    python tools/bc.py                       # train and publish
    python tools/bc.py --epochs 400 --print  # more passes, verbose

Output is a weights.txt the plugin loads and a ckpt.npz that `learn.py --resume`
continues from, so PPO carries on from cloned technique rather than scratch.
"""

import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from ppo import Policy, Value, Adam, log_softmax, write_weights, POL_TOTAL
from rollout import OBS_DIM, N_ACTIONS

N_TRIMS = N_ACTIONS // 2      # actions 0..N_TRIMS-1 are side +1, the rest side -1

CSTRIKE = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrike"
DATA = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai")
DEFAULT_IN = os.path.join(DATA, r"out\surf_demise_bc.txt")
DEFAULT_WEIGHTS = os.path.join(DATA, "weights.txt")


def load(path):
    acts, obs = [], []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if len(p) != 1 + OBS_DIM:
                continue
            acts.append(int(float(p[0])))
            obs.append([float(v) for v in p[1:]])
    return np.array(obs, dtype=np.float64), np.array(acts, dtype=np.int64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=DEFAULT_IN)
    ap.add_argument("--weights", default=DEFAULT_WEIGHTS)
    ap.add_argument("--ckpt", default=os.path.join(HERE, "..", "data", "ckpt.npz"))
    ap.add_argument("--epochs", type=int, default=300)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--minibatch", type=int, default=256)
    ap.add_argument("--val", type=float, default=0.15, help="held-out fraction")
    ap.add_argument("--balance", action="store_true",
                    help="weight classes inversely to frequency; off by default, "
                         "because upweighting the rare trims cost more in spurious "
                         "side changes than it bought in coverage")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--print", dest="do_print", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.data):
        print("no capture at %s" % args.data)
        print("produce it with:  srcds ... +csai_democapture 1")
        return 1

    X, y = load(args.data)
    if len(X) == 0:
        print("capture is empty")
        return 1

    # keep the capture in trajectory order: the switch-rate check below is only
    # meaningful as a sequence, and shuffling destroys that.
    Xseq, yseq = X.copy(), y.copy()

    rng = np.random.default_rng(args.seed)
    idx = rng.permutation(len(X))
    X, y = X[idx], y[idx]
    nval = max(int(len(X) * args.val), 1)
    Xv, yv, Xt, yt = X[:nval], y[:nval], X[nval:], y[nval:]

    print("samples %d  (train %d, val %d)   obs %d   actions %d"
          % (len(X), len(Xt), len(Xv), OBS_DIM, N_ACTIONS))

    counts = np.bincount(y, minlength=N_ACTIONS).astype(np.float64)
    if args.balance:
        w = np.where(counts > 0, len(y) / (N_ACTIONS * np.maximum(counts, 1)), 0.0)
    else:
        w = np.ones(N_ACTIONS)
    print("class weights: " + " ".join("%.2f" % v for v in w))

    policy = Policy(np.random.default_rng(args.seed))
    opt = Adam(policy.shapes(), lr=args.lr)

    def evaluate(Xe, ye):
        logits, _ = policy.forward(Xe)
        pred = logits.argmax(axis=1)
        acc = float((pred == ye).mean())
        # balanced accuracy: mean per-class recall, so the rare trims count as
        # much as the common ones and a fit cannot hide in the majority class.
        recs = []
        for a in range(N_ACTIONS):
            m = ye == a
            if m.sum():
                recs.append(float((pred[m] == a).mean()))
        return acc, (float(np.mean(recs)) if recs else 0.0)

    n = len(Xt)
    for ep in range(args.epochs):
        order = rng.permutation(n)
        tot = 0.0
        for s in range(0, n, args.minibatch):
            mb = order[s:s + args.minibatch]
            o, a = Xt[mb], yt[mb]
            logits, cache = policy.forward(o)
            lsm = log_softmax(logits)
            sw = w[a]
            loss = -(lsm[np.arange(len(mb)), a] * sw).sum() / max(sw.sum(), 1e-8)
            tot += float(loss)

            p = np.exp(lsm)
            dlogits = p * sw[:, None]
            dlogits[np.arange(len(mb)), a] -= sw
            dlogits /= max(sw.sum(), 1e-8)
            opt.step(policy.params(), policy.backward(cache, dlogits))

        if args.do_print and (ep % 25 == 0 or ep == args.epochs - 1):
            ta, tb = evaluate(Xt, yt)
            va, vb = evaluate(Xv, yv)
            print("  epoch %4d  loss %.4f   train acc %.3f (bal %.3f)   val acc %.3f (bal %.3f)"
                  % (ep, tot / max(1, (n + args.minibatch - 1) // args.minibatch), ta, tb, va, vb))

    ta, tb = evaluate(Xt, yt)
    va, vb = evaluate(Xv, yv)
    print()
    print("final   train acc %.3f (balanced %.3f)    val acc %.3f (balanced %.3f)"
          % (ta, tb, va, vb))
    base = float((y == np.bincount(y).argmax()).mean())
    print("majority-class baseline: %.3f  -> cloning %s"
          % (base, "beats it" if va > base + 0.01 else "does NOT beat it"))

    # Actions are absolute (side, trim), so the two halves of the space are the
    # two strafe keys. Getting the SIDE right is the technique; the trim is a
    # few degrees of steering on top of it.
    logits, _ = policy.forward(Xv)
    side_acc = float(((logits.argmax(axis=1) < N_TRIMS) == (yv < N_TRIMS)).mean())
    print("side accuracy (val): %.3f" % side_acc)

    # Switch rate along the human's own trajectory: how often would the cloned
    # policy change strafe key, against how often the human did? This is the
    # number the jitter problem was about.
    pred_seq = policy.forward(Xseq)[0].argmax(axis=1) < N_TRIMS
    human_seq = yseq < N_TRIMS
    hz = 66.67 / 2.0
    sw_h = float((human_seq[1:] != human_seq[:-1]).mean()) * hz
    sw_p = float((pred_seq[1:] != pred_seq[:-1]).mean()) * hz
    print("strafe-key switches: human %.2f/s, cloned %.2f/s  (pre-BC policy was 19.79/s)"
          % (sw_h, sw_p))

    write_weights(args.weights, policy, 1)
    print("published %s (gen 1, %d floats)" % (args.weights, POL_TOTAL))

    value = Value(np.random.default_rng(args.seed + 1))
    np.savez(args.ckpt, gen=1,
             ret_mean=0.0, ret_var=1.0, ret_count=1e-4,
             **{"p%d" % i: p for i, p in enumerate(policy.params())},
             **{"v%d" % i: p for i, p in enumerate(value.params())})
    print("wrote %s - learn.py --resume continues from cloned technique" % args.ckpt)

    # A second, frozen copy. args.ckpt is overwritten every generation once PPO
    # starts, so it cannot also serve as the thing PPO is anchored to.
    ref = os.path.join(os.path.dirname(os.path.abspath(args.ckpt)), "ckpt_bc.npz")
    np.savez(ref, gen=1, **{"p%d" % i: p for i, p in enumerate(policy.params())})
    print("wrote %s - frozen anchor for learn.py --kl-ref" % ref)
    return 0


if __name__ == "__main__":
    sys.exit(main())
