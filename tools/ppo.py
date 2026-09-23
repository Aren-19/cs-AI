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
    n = len(rewards)
    adv = np.zeros(n)
    last_gae = 0.0
    for t in range(n - 1, -1, -1):
        if t == n - 1:
            next_v = last_value
        else:
            next_v = values[t + 1]
        nonterm = 1.0 - dones[t]
        delta = rewards[t] + gamma * next_v * nonterm - values[t]
        last_gae = delta + gamma * lam * nonterm * last_gae
        adv[t] = last_gae
    return adv, adv + values

def ppo_update(policy, value, pol_opt, val_opt, obs, actions, old_logp, adv, returns,
               epochs=4, minibatch=4096, clip=0.2, ent_coef=0.01, vf_coef=0.5,
               max_grad_norm=1.0, rng=None, ref_policy=None, kl_ref_coef=0.0):
    """PPO, optionally anchored to a frozen reference policy."""
    rng = rng or np.random.default_rng(0)
    n = obs.shape[0]

    adv_n = (adv - adv.mean()) / (adv.std() + 1e-8)

    stats = {"pol_loss": 0.0, "val_loss": 0.0, "entropy": 0.0, "kl": 0.0,
             "clipfrac": 0.0, "kl_ref": 0.0, "nupd": 0}
    anchored = ref_policy is not None and kl_ref_coef > 0.0

    for _ in range(epochs):
        idx = rng.permutation(n)
        for start in range(0, n, minibatch):
            mb = idx[start:start + minibatch]
            if len(mb) < 2:
                continue
            o, a = obs[mb], actions[mb]
            olp, ad, ret = old_logp[mb], adv_n[mb], returns[mb]

            logits, cache = policy.forward(o)
            logp_all = log_softmax(logits)
            logp = logp_all[np.arange(len(mb)), a]
            p_all = np.exp(logp_all)

            ratio = np.exp(logp - olp)
            unclipped = ratio * ad
            clipped = np.clip(ratio, 1 - clip, 1 + clip) * ad
            use_unclipped = unclipped <= clipped        # PPO takes the min

            # d/dlogp of -mean(min(...)) ; the clipped branch has zero gradient
            # wherever the ratio is outside the trust region
            dlogp = np.where(use_unclipped, -ad * ratio, 0.0) / len(mb)

            entropy = -(p_all * logp_all).sum(axis=1)
            # dH/dz_j = -p_j (log p_j + H)
            dent = -p_all * (logp_all + entropy[:, None])

            # dlogp_i/dlogits_j = delta_{j,a_i} - p_j
            dlogits = -p_all * dlogp[:, None]
            dlogits[np.arange(len(mb)), a] += dlogp
            dlogits += (-ent_coef) * dent / len(mb)

            if anchored:
                # KL(pi || ref) = sum_a p_a (log p_a - log ref_a), so
                # dKL/dz_j = p_j * ((log p_j - log ref_j) - KL)
                ref_logp_all = log_softmax(ref_policy.forward(o)[0])
                d = logp_all - ref_logp_all
                kl_ref = (p_all * d).sum(axis=1)
                dlogits += kl_ref_coef * (p_all * (d - kl_ref[:, None])) / len(mb)
                stats["kl_ref"] += float(kl_ref.mean())

            gp = policy.backward(cache, dlogits)
            clip_grads(gp, max_grad_norm)
            pol_opt.step(policy.params(), gp)

            v, vcache = value.forward(o)
            ret_n = value.normalize(ret)
            dv = vf_coef * 2.0 * (v - ret_n) / len(mb)
            gv = value.backward(vcache, dv)
            clip_grads(gv, max_grad_norm)
            val_opt.step(value.params(), gv)

            stats["pol_loss"] += float(-np.minimum(unclipped, clipped).mean())
            stats["val_loss"] += float(((v - ret_n) ** 2).mean())
            stats["entropy"] += float(entropy.mean())
            stats["kl"] += float((olp - logp).mean())
            stats["clipfrac"] += float((np.abs(ratio - 1.0) > clip).mean())
            stats["nupd"] += 1

    k = max(stats["nupd"], 1)
    for key in ("pol_loss", "val_loss", "entropy", "kl", "clipfrac", "kl_ref"):
        stats[key] /= k
    return stats

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
