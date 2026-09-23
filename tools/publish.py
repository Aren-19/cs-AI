"""Write a checkpoint's policy out as the weights file the plugin reads."""

import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from ppo import Policy, write_weights

CSTRIKE = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrike"
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
        if z[key].shape != p.shape:
            print("%s is %s, this policy wants %s" % (key, z[key].shape, p.shape))
            return 1
        p[...] = z[key]
        n += 1
    gen = int(z["gen"]) if "gen" in z else 0
    write_weights(args.weights, policy, gen)
    print("published %s (generation %d, %d tensors) -> %s"
          % (os.path.basename(args.ckpt), gen, n, args.weights))
    return 0

if __name__ == "__main__":
    sys.exit(main())
