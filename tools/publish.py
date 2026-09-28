"""Write a checkpoint's policy out as the weights file the plugin reads."""

import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from ppo import Policy, write_weights, fit_inputs
from rollout import OBS_DIM

from game import CSTRIKE
WEIGHTS = os.path.join(CSTRIKE, r"addons\sourcemod\data\csai\weights.txt")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ckpt")
    ap.add_argument("--weights", default=WEIGHTS)
    args = ap.parse_args()

    if not os.path.isfile(args.ckpt):
        print("no such checkpoint: %s" % args.ckpt)
        return 1
    try:
        with np.load(args.ckpt) as f:
            z = {k: f[k] for k in f.files}
    except Exception as e:
        print("%s is not a readable checkpoint (%s)" % (args.ckpt, type(e).__name__))
        return 1
    policy = Policy(np.random.default_rng(0))
    n = 0
    for i, p in enumerate(policy.params()):
        key = "p%d" % i
        if key not in z:
            print("checkpoint has no %s - shape does not match this policy" % key)
            return 1
        w = z[key]
        if i == 0 and w.ndim == 2 and w.shape[1] != OBS_DIM:
            # An older checkpoint: the new inputs start at zero weight, as on resume.
            print("%s takes %d inputs, widening to %d" % (key, w.shape[1], OBS_DIM))
            w = fit_inputs(w, OBS_DIM)
        if w.shape != p.shape:
            print("%s is %s, this policy wants %s" % (key, w.shape, p.shape))
            return 1
        p[...] = w
        n += 1
    gen = int(z["gen"]) if "gen" in z else 0
    write_weights(args.weights, policy, gen)
    print("published %s (generation %d, %d tensors) -> %s"
          % (os.path.basename(args.ckpt), gen, n, args.weights))
    return 0

if __name__ == "__main__":
    sys.exit(main())
