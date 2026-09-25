"""Gradient and shape checks for ppo.py."""

import numpy as np

from ppo import (Policy, Value, Adam, log_softmax, POL_TOTAL, H1, H2, compute_gae,
                 ppo_update)
from rollout import OBS_DIM, N_ACTIONS

def ppo_scalar_loss(policy, o, a, olp, ad, clip=0.2, ent_coef=0.01):
    logits, _ = policy.forward(o)
    lsm = log_softmax(logits)
    logp = lsm[np.arange(len(a)), a]
    p = np.exp(lsm)
    ratio = np.exp(logp - olp)
    unclipped = ratio * ad
    clipped = np.clip(ratio, 1 - clip, 1 + clip) * ad
    entropy = -(p * lsm).sum(axis=1)
    return float(-np.minimum(unclipped, clipped).mean() - ent_coef * entropy.mean())

def analytic_policy_grads(policy, o, a, olp, ad, clip=0.2, ent_coef=0.01):
    n = len(a)
    logits, cache = policy.forward(o)
    lsm = log_softmax(logits)
    logp = lsm[np.arange(n), a]
    p = np.exp(lsm)

    ratio = np.exp(logp - olp)
    unclipped = ratio * ad
    clipped = np.clip(ratio, 1 - clip, 1 + clip) * ad
    use_unclipped = unclipped <= clipped

    dlogp = np.where(use_unclipped, -ad * ratio, 0.0) / n

    entropy = -(p * lsm).sum(axis=1)
    dent = -p * (lsm + entropy[:, None])

    dlogits = -p * dlogp[:, None]
    dlogits[np.arange(n), a] += dlogp
    dlogits += (-ent_coef) * dent / n

    return policy.backward(cache, dlogits)

def numeric_grads(fn, params, eps=1e-6):
    out = []
    for p in params:
        g = np.zeros_like(p)
        flat = p.ravel()
        gflat = g.ravel()
        for i in range(flat.size):
            old = flat[i]
            flat[i] = old + eps
            fp = fn()
            flat[i] = old - eps
            fm = fn()
            flat[i] = old
            gflat[i] = (fp - fm) / (2 * eps)
        out.append(g)
    return out

def rel_err(a, b):
    denom = np.maximum(np.abs(a) + np.abs(b), 1e-12)
    return float(np.max(np.abs(a - b) / denom))

def check_policy():
    rng = np.random.default_rng(7)
    n = 24
    o = rng.normal(0, 1, (n, OBS_DIM))
    a = rng.integers(0, N_ACTIONS, n)
    olp = rng.normal(-3.2, 0.1, n)
    ad = rng.normal(0, 1, n)

    pol = Policy(np.random.default_rng(3))
    # make the head non-trivial so gradients are not all ~0
    pol.W3 = rng.normal(0, 0.3, pol.W3.shape)

    ana = analytic_policy_grads(pol, o, a, olp, ad)
    num = numeric_grads(lambda: ppo_scalar_loss(pol, o, a, olp, ad), pol.params())

    worst = 0.0
    for nm, g1, g2 in zip(["W1", "b1", "W2", "b2", "W3", "b3"], ana, num):
        e = rel_err(g1, g2)
        worst = max(worst, e)
        print("   policy %-3s max rel err %.3e" % (nm, e))
    assert worst < 2e-4, "policy gradient mismatch: %.3e" % worst
    return worst

def check_value():
    rng = np.random.default_rng(11)
    n = 24
    o = rng.normal(0, 1, (n, OBS_DIM))
    ret = rng.normal(0, 1, n)
    vf = 0.5

    val = Value(np.random.default_rng(5))
    val.W3 = rng.normal(0, 0.3, val.W3.shape)

    def loss():
        v, _ = val.forward(o)
        return float(vf * ((v - ret) ** 2).mean())

    v, cache = val.forward(o)
    dv = vf * 2.0 * (v - ret) / n
    ana = val.backward(cache, dv)
    num = numeric_grads(loss, val.params())

    worst = 0.0
    for nm, g1, g2 in zip(["W1", "b1", "W2", "b2", "W3", "b3"], ana, num):
        e = rel_err(g1, g2)
        worst = max(worst, e)
        print("   value  %-3s max rel err %.3e" % (nm, e))
    assert worst < 2e-4, "value gradient mismatch: %.3e" % worst
    return worst

def check_gae():
    # two episodes: first ends terminal, second is a cutoff
    rew = np.array([1.0, 1.0, 1.0, 2.0, 2.0])
    val = np.array([0.5, 0.5, 0.5, 1.0, 1.0])
    done = np.array([0.0, 0.0, 1.0, 0.0, 0.0])
    adv, ret = compute_gae(rew, val, done, last_value=3.0, gamma=0.99, lam=0.95)

    # at t=2 the episode is terminal, so no bootstrap: delta = r - v
    assert abs(adv[2] - (1.0 - 0.5)) < 1e-12, adv[2]
    # at t=4 it is a cutoff, so it bootstraps from last_value
    expected = 2.0 + 0.99 * 3.0 - 1.0
    assert abs(adv[4] - expected) < 1e-12, (adv[4], expected)
    print("   gae terminal vs cutoff handling OK")
    return 0.0

def check_flat_layout():
    pol = Policy(np.random.default_rng(1))
    flat = pol.flat()
    assert flat.size == POL_TOTAL, (flat.size, POL_TOTAL)

    # rebuild the way csai_policy.inc slices the stream, and compare
    o = 0
    W1 = flat[o:o + H1 * OBS_DIM].reshape(H1, OBS_DIM); o += H1 * OBS_DIM
    b1 = flat[o:o + H1]; o += H1
    W2 = flat[o:o + H2 * H1].reshape(H2, H1); o += H2 * H1
    b2 = flat[o:o + H2]; o += H2
    W3 = flat[o:o + N_ACTIONS * H2].reshape(N_ACTIONS, H2); o += N_ACTIONS * H2
    b3 = flat[o:o + N_ACTIONS]; o += N_ACTIONS
    assert o == POL_TOTAL

    for nm, x, y in (("W1", W1, pol.W1), ("b1", b1, pol.b1), ("W2", W2, pol.W2),
                     ("b2", b2, pol.b2), ("W3", W3, pol.W3), ("b3", b3, pol.b3)):
        assert np.array_equal(x, y), "layout mismatch in " + nm
    print("   weight layout round-trips (%d floats)" % POL_TOTAL)
    return 0.0

def _one_update(mask, seed=4):
    rng = np.random.default_rng(seed)
    n = 64
    o = rng.normal(0, 1, (n, OBS_DIM))
    a = rng.integers(0, 4, n)                     # only the first four actions are ever taken
    olp = rng.normal(-1.5, 0.1, n)
    ad = rng.normal(0, 1, n)
    ret = rng.normal(0, 1, n)
    pol = Policy(np.random.default_rng(3))
    pol.W3 = rng.normal(0, 0.3, pol.W3.shape)
    val = Value(np.random.default_rng(5))
    before = [p.copy() for p in pol.params()]
    ppo_update(pol, val, Adam(pol.shapes()), Adam(val.shapes()), o, a, olp, ad, ret,
               epochs=1, minibatch=n, mask=mask, dtype=np.float64,
               rng=np.random.default_rng(0))
    return before, pol

def check_mask():
    n = 64
    everything = np.ones((n, N_ACTIONS), dtype=bool)
    _b, p_all = _one_update(everything)
    _b, p_none = _one_update(None)
    for x, y in zip(p_all.params(), p_none.params()):
        assert np.allclose(x, y), "an all-allowed mask changed the update"

    few = np.zeros((n, N_ACTIONS), dtype=bool)
    few[:, :4] = True
    before, pol = _one_update(few)
    moved = np.abs(pol.b3 - before[5])
    assert np.all(moved[4:] == 0.0), "a masked action's logit moved"
    assert np.any(moved[:4] > 0.0)
    print("   masked actions get no gradient, a full mask changes nothing")
    return 0.0

if __name__ == "__main__":
    print("checking policy gradients...")
    check_policy()
    print("checking value gradients...")
    check_value()
    print("checking GAE...")
    check_gae()
    print("checking action masks...")
    check_mask()
    print("checking weight layout...")
    check_flat_layout()
    print("\nall gradient checks passed")
