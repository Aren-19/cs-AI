"""Copy one checkpoint's networks over another, keeping the target's generation count."""

import argparse
import os
import sys

import numpy as np

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    args = ap.parse_args()

    with np.load(args.src) as f:
        z = {k: f[k] for k in f.files}
    gen = int(z["gen"])
    if os.path.exists(args.dst):
        with np.load(args.dst) as f:
            gen = max(gen, int(f["gen"]))
    z["gen"] = np.array(gen)

    tmp = args.dst + ".tmp.npz"
    np.savez(tmp, **z)
    os.replace(tmp, args.dst)
    print("restored %s into %s at generation %d" % (os.path.basename(args.src), os.path.basename(args.dst), gen))
    return 0

if __name__ == "__main__":
    sys.exit(main())
