"""Write a checkpoint's policy as a weights file the plugin can load.

Evaluations run on a frozen copy, so the checkpoint kept as the best is the one
that was measured, not whatever the learner published while the server started.

    python tools/freeze.py <checkpoint.npz> <weights.txt>
"""

import sys

import numpy as np

from ppo import Policy, write_weights, fit_inputs
from rollout import OBS_DIM

def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    with np.load(sys.argv[1]) as z:
        d = {k: z[k] for k in z.files}
    pol = Policy()
    for i, p in enumerate(pol.params()):
        w = d["p%d" % i]
        p[...] = fit_inputs(w, OBS_DIM) if i == 0 else w
    gen = int(d["gen"])
    write_weights(sys.argv[2], pol, gen)
    print("froze generation %d into %s" % (gen, sys.argv[2]))
    return 0

if __name__ == "__main__":
    sys.exit(main())
