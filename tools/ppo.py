"""Policy and value networks, PPO update, checkpoint and weight IO."""

import os
import time

import numpy as np

from rollout import OBS_DIM, N_ACTIONS

H1 = 32
H2 = 32

POL_TOTAL = OBS_DIM * H1 + H1 + H1 * H2 + H2 + H2 * N_ACTIONS + N_ACTIONS

def relu(x):
    return np.maximum(x, 0.0)

def log_softmax(z):
    z = z - z.max(axis=-1, keepdims=True)
    return z - np.log(np.exp(z).sum(axis=-1, keepdims=True))

class Adam(object):
    def __init__(self, shapes, lr=3e-4, b1=0.9, b2=0.999, eps=1e-8):
        self.m = [np.zeros(s) for s in shapes]
        self.v = [np.zeros(s) for s in shapes]
        self.lr, self.b1, self.b2, self.eps = lr, b1, b2, eps
        self.t = 0

    def step(self, params, grads):
        self.t += 1
        for i, (p, g) in enumerate(zip(params, grads)):
            self.m[i] = self.b1 * self.m[i] + (1 - self.b1) * g
            self.v[i] = self.b2 * self.v[i] + (1 - self.b2) * (g * g)
            mhat = self.m[i] / (1 - self.b1 ** self.t)
            vhat = self.v[i] / (1 - self.b2 ** self.t)
            p -= self.lr * mhat / (np.sqrt(vhat) + self.eps)

class Policy(object):
    """obs -> logits. Mirrors csai_policy.inc (ReLU hidden, linear head)."""

    def __init__(self, rng=None):
        rng = rng or np.random.default_rng(0)
        self.W1 = rng.normal(0, np.sqrt(2.0 / OBS_DIM), (H1, OBS_DIM))
        self.b1 = np.zeros(H1)
        self.W2 = rng.normal(0, np.sqrt(2.0 / H1), (H2, H1))
        self.b2 = np.zeros(H2)
        self.W3 = rng.normal(0, 0.01, (N_ACTIONS, H2))
        self.b3 = np.zeros(N_ACTIONS)

    def params(self):
        return [self.W1, self.b1, self.W2, self.b2, self.W3, self.b3]

    def shapes(self):
        return [p.shape for p in self.params()]

    def forward(self, obs):
        z1 = obs @ self.W1.T + self.b1
        h1 = relu(z1)
        z2 = h1 @ self.W2.T + self.b2
        h2 = relu(z2)
        logits = h2 @ self.W3.T + self.b3
        return logits, (obs, z1, h1, z2, h2)

    def backward(self, cache, dlogits):
        """dlogits is dL/dlogits, already averaged over the minibatch."""
        obs, z1, h1, z2, h2 = cache

        gW3 = dlogits.T @ h2
        gb3 = dlogits.sum(axis=0)

        dh2 = dlogits @ self.W3
        dz2 = dh2 * (z2 > 0)
        gW2 = dz2.T @ h1
        gb2 = dz2.sum(axis=0)

        dh1 = dz2 @ self.W2
        dz1 = dh1 * (z1 > 0)
        gW1 = dz1.T @ obs
        gb1 = dz1.sum(axis=0)

        return [gW1, gb1, gW2, gb2, gW3, gb3]

    def flat(self):
        """Flatten in the exact order csai_policy.inc reads."""
        return np.concatenate([p.ravel() for p in self.params()])

class Value(object):
    """obs -> scalar. Python-only; tanh keeps it stable on wide-ranged inputs."""

    def __init__(self, rng=None):
        rng = rng or np.random.default_rng(1)
        self.W1 = rng.normal(0, np.sqrt(1.0 / OBS_DIM), (H1, OBS_DIM))
        self.b1 = np.zeros(H1)
        self.W2 = rng.normal(0, np.sqrt(1.0 / H1), (H2, H1))
        self.b2 = np.zeros(H2)
        self.W3 = rng.normal(0, 0.01, (1, H2))
        self.b3 = np.zeros(1)

        # running statistics of the raw return, for target normalisation
        self.ret_mean = 0.0
        self.ret_var = 1.0
        self.ret_count = 1e-4

    def params(self):
        return [self.W1, self.b1, self.W2, self.b2, self.W3, self.b3]

    def shapes(self):
        return [p.shape for p in self.params()]

    def update_ret_stats(self, returns):
        """Welford-style merge, so the scale tracks the policy as it improves."""
        n = len(returns)
        if n == 0:
            return
        m, v = float(np.mean(returns)), float(np.var(returns))
        delta = m - self.ret_mean
        tot = self.ret_count + n
        self.ret_mean += delta * n / tot
        m_a = self.ret_var * self.ret_count
        m_b = v * n
        self.ret_var = (m_a + m_b + delta * delta * self.ret_count * n / tot) / tot
        self.ret_count = tot

    @property
    def ret_std(self):
        return max(np.sqrt(self.ret_var), 1e-6)

    def normalize(self, raw):
        return (raw - self.ret_mean) / self.ret_std

    def denormalize(self, norm):
        return norm * self.ret_std + self.ret_mean

    def forward(self, obs):
        """Returns the NORMALISED value. Callers that need raw units denormalize."""
        z1 = obs @ self.W1.T + self.b1
        h1 = np.tanh(z1)
        z2 = h1 @ self.W2.T + self.b2
        h2 = np.tanh(z2)
        v = (h2 @ self.W3.T + self.b3)[:, 0]
        return v, (obs, h1, h2)

    def forward_raw(self, obs):
        v, cache = self.forward(obs)
        return self.denormalize(v), cache

    def backward(self, cache, dv):
        """dv is dL/dv, already averaged over the minibatch."""
        obs, h1, h2 = cache
        dv = dv[:, None]

        gW3 = dv.T @ h2
        gb3 = dv.sum(axis=0)

        dh2 = dv @ self.W3
        dz2 = dh2 * (1 - h2 * h2)
        gW2 = dz2.T @ h1
        gb2 = dz2.sum(axis=0)

        dh1 = dz2 @ self.W2
        dz1 = dh1 * (1 - h1 * h1)
        gW1 = dz1.T @ obs
        gb1 = dz1.sum(axis=0)

        return [gW1, gb1, gW2, gb2, gW3, gb3]

def compute_gae(rewards, values, dones, last_value=0.0, gamma=0.99, lam=0.95):
    """Generalised advantage estimation over a concatenation of episodes."""
    r = np.asarray(rewards, dtype=np.float64).tolist()
    v = np.asarray(values, dtype=np.float64).tolist()
    d = np.asarray(dones, dtype=np.float64).tolist()
    n = len(r)
    adv = [0.0] * n
    last_gae = 0.0
    next_v = float(last_value)
    for t in range(n - 1, -1, -1):
        nonterm = 1.0 - d[t]
        delta = r[t] + gamma * next_v * nonterm - v[t]
        last_gae = delta + gamma * lam * nonterm * last_gae
        adv[t] = last_gae
        next_v = v[t]
    adv = np.array(adv)
    return adv, adv + np.asarray(values, dtype=np.float64)

# A masked-out action gets this logit: zero probability, and still finite so
# entropy and gradients stay clean.
MASKED = -1.0e9

def _fwd_pol(Ws, o):
    W1, b1, W2, b2, W3, b3 = Ws
    z1 = o @ W1.T
    z1 += b1
    h1 = np.maximum(z1, 0.0)
    z2 = h1 @ W2.T
    z2 += b2
    h2 = np.maximum(z2, 0.0)
    logits = h2 @ W3.T
    logits += b3
    return logits, h1, h2

def _bwd_pol(Ws, o, h1, h2, dlogits):
    W1, b1, W2, b2, W3, b3 = Ws
    gW3 = dlogits.T @ h2
    gb3 = dlogits.sum(axis=0)
    dz2 = dlogits @ W3
    dz2 *= (h2 > 0)
    gW2 = dz2.T @ h1
    gb2 = dz2.sum(axis=0)
    dz1 = dz2 @ W2
    dz1 *= (h1 > 0)
    gW1 = dz1.T @ o
    gb1 = dz1.sum(axis=0)
    return [gW1, gb1, gW2, gb2, gW3, gb3]

def _fwd_val(Ws, o):
    W1, b1, W2, b2, W3, b3 = Ws
    z1 = o @ W1.T
    z1 += b1
    h1 = np.tanh(z1)
    z2 = h1 @ W2.T
    z2 += b2
    h2 = np.tanh(z2)
    v = h2 @ W3[0] + b3[0]
    return v, h1, h2

def _bwd_val(Ws, o, h1, h2, dv):
    W1, b1, W2, b2, W3, b3 = Ws
    gW3 = (dv @ h2)[None, :]
    gb3 = np.array([dv.sum()])
    dz2 = np.outer(dv, W3[0])
    dz2 *= 1 - h2 * h2
    gW2 = dz2.T @ h1
    gb2 = dz2.sum(axis=0)
    dz1 = dz2 @ W2
    dz1 *= 1 - h1 * h1
    gW1 = dz1.T @ o
    gb1 = dz1.sum(axis=0)
    return [gW1, gb1, gW2, gb2, gW3, gb3]

def value_raw(value, obs, dtype=np.float32):
    """Raw-unit values for a whole batch in one pass."""
    Wv = [p.astype(dtype) for p in value.params()]
    v = _fwd_val(Wv, np.ascontiguousarray(obs, dtype=dtype))[0]
    return value.denormalize(v.astype(np.float64))

def ppo_update(policy, value, pol_opt, val_opt, obs, actions, old_logp, adv, returns,
               epochs=4, minibatch=4096, clip=0.2, ent_coef=0.01, vf_coef=0.5,
               max_grad_norm=1.0, rng=None, ref_policy=None, kl_ref_coef=0.0,
               mask=None, dtype=np.float32):
    """PPO over the allowed actions, optionally anchored to a frozen reference policy.

    mask is (n, N_ACTIONS) bool: the actions the actor could pick at each step.
    The math runs in float32; the weights and Adam state stay float64.
    """
    rng = rng or np.random.default_rng(0)
    n = obs.shape[0]
    f = dtype
    obs = np.ascontiguousarray(obs, dtype=f)
    adv_n = ((adv - adv.mean()) / (adv.std() + 1e-8)).astype(f)
    old_logp = np.asarray(old_logp).astype(f)
    ret_n_all = value.normalize(returns).astype(f)     # the scale is fixed during the update
    bias = None
    if mask is not None:
        bias = np.where(mask, 0.0, MASKED).astype(f)
        bias[np.arange(n), actions] = 0.0              # the action taken was allowed

    stats = {"pol_loss": 0.0, "val_loss": 0.0, "entropy": 0.0, "kl": 0.0,
             "clipfrac": 0.0, "kl_ref": 0.0, "nupd": 0}
    anchored = ref_policy is not None and kl_ref_coef > 0.0
    if anchored:
        Wr = [p.astype(f) for p in ref_policy.params()]
        rl = _fwd_pol(Wr, obs)[0]
        if bias is not None:
            rl += bias
        ref_lp = log_softmax(rl)                       # once per batch

    acc = np.zeros(6)
    for _ in range(epochs):
        idx = rng.permutation(n)
        O = obs[idx]
        A = actions[idx]
        OLP = old_logp[idx]
        AD = adv_n[idx]
        RN = ret_n_all[idx]
        B = bias[idx] if bias is not None else None
        RL = ref_lp[idx] if anchored else None
        for start in range(0, n, minibatch):
            end = min(start + minibatch, n)
            m = end - start
            if m < 2:
                continue
            o = O[start:end]
            a = A[start:end]
            olp = OLP[start:end]
            ad = AD[start:end]
            rows = np.arange(m)

            Wp = [p.astype(f) for p in policy.params()]
            logits, h1, h2 = _fwd_pol(Wp, o)
            if B is not None:
                logits += B[start:end]
            logp_all = log_softmax(logits)
            logp = logp_all[rows, a]
            p_all = np.exp(logp_all)

            ratio = np.exp(logp - olp)
            unclipped = ratio * ad
            clipped = np.clip(ratio, 1 - clip, 1 + clip) * ad
            use_unclipped = unclipped <= clipped        # PPO takes the min

            # d/dlogp of -mean(min(...)); the clipped branch has zero gradient
            # wherever the ratio is outside the trust region
            dlogp = np.where(use_unclipped, -ad * ratio, 0.0) / m

            # masked actions have p = 0, so they add nothing to either term
            plogp = p_all * logp_all
            entropy = -plogp.sum(axis=1)
            # dlogp_i/dlogits_j = delta_{j,a_i} - p_j
            dlogits = -p_all * dlogp[:, None]
            dlogits[rows, a] += dlogp
            # entropy bonus: -ent_coef * dH/dz / m, with dH/dz = -p (log p + H)
            dlogits += (ent_coef / m) * (plogp + p_all * entropy[:, None])

            if anchored:
                # KL(pi || ref) = sum_a p_a (log p_a - log ref_a), so
                # dKL/dz_j = p_j * ((log p_j - log ref_j) - KL)
                dref = logp_all - RL[start:end]
                pd = p_all * dref
                kl_ref = pd.sum(axis=1)
                dlogits += (kl_ref_coef / m) * (pd - p_all * kl_ref[:, None])
                acc[5] += float(kl_ref.mean())

            gp = [g.astype(np.float64) for g in _bwd_pol(Wp, o, h1, h2, dlogits)]
            clip_grads(gp, max_grad_norm)
            pol_opt.step(policy.params(), gp)

            Wv = [p.astype(f) for p in value.params()]
            v, vh1, vh2 = _fwd_val(Wv, o)
            err = v - RN[start:end]
            dv = (vf_coef * 2.0 / m) * err
            gv = [g.astype(np.float64) for g in _bwd_val(Wv, o, vh1, vh2, dv)]
            clip_grads(gv, max_grad_norm)
            val_opt.step(value.params(), gv)

            acc[0] += float(-np.minimum(unclipped, clipped).mean())
            acc[1] += float((err * err).mean())
            acc[2] += float(entropy.mean())
            acc[3] += float((olp - logp).mean())
            acc[4] += float((np.abs(ratio - 1.0) > clip).mean())
            stats["nupd"] += 1

    k = max(stats["nupd"], 1)
    for i, key in enumerate(("pol_loss", "val_loss", "entropy", "kl", "clipfrac", "kl_ref")):
        stats[key] = acc[i] / k
    return stats

def fit_inputs(W, dim):
    """A first-layer weight matrix widened (new inputs start at zero weight) or cut to dim inputs."""
    if W.shape[1] == dim:
        return W
    out = np.zeros((W.shape[0], dim))
    k = min(dim, W.shape[1])
    out[:, :k] = W[:, :k]
    return out

def clip_grads(grads, max_norm):
    total = np.sqrt(sum(float((g * g).sum()) for g in grads))
    if total > max_norm and total > 0:
        scale = max_norm / total
        for g in grads:
            g *= scale
    return total

def write_weights(path, policy, gen):
    """Flat text, one float per line, in the plugin's expected order."""
    flat = policy.flat()
    assert flat.size == POL_TOTAL, "weights %d != plugin total %d" % (flat.size, POL_TOTAL)
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        print("# gen %d dim %d actions %d total %d" % (gen, OBS_DIM, N_ACTIONS, POL_TOTAL), file=fh)
        for v in flat:
            print("%.7g" % v, file=fh)
    for attempt in range(50):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            time.sleep(0.05)
    raise PermissionError("could not replace %s after 2.5s of retries" % path)
