"""
Read trajectory batches written by the plugin, and rebuild the observations the
policy actually saw.

The plugin logs only (pos, vel, action, logp, reward) - 9 floats per step. The
observation is recomputed here. That keeps records small and lets the observation
definition change without re-collecting data, but it carries one hard
requirement: **this must reproduce csai_track.inc exactly**. If the observations
diverge, PPO computes its ratio against a distribution the actor never used, and
the gradient is wrong in a way nothing will report.

So the nearest-point search here is the plugin's *windowed* search, hint and all -
not a full argmin. On a track that never approaches itself they agree, but
mirroring it removes the question. tools/check_obs.py verifies the two against
each other numerically.

Batch file format (little-endian):
    per episode:
        int32   n_steps
        int32   outcome        (1 fell, 2 finished, 3 timeout, 4 stuck)
        int32   start_state
        float32 best_s
        n_steps * 9 * float32  px py pz vx vy vz action logp reward
"""

import math
import os
import struct

import numpy as np

# must match csai_track.inc
LOOK_OFFSETS   = (4, 8, 16, 32, 64, 128, 256)   # track points; 64 units apart
LOOKAHEAD      = len(LOOK_OFFSETS)
PROBE_RAYS     = 5
PROBE_DIM      = PROBE_RAYS + 3
WISH_DIM       = 0          # previous action; removed - BC latched onto it
OBS_DIM        = 7 + 3 * LOOKAHEAD + PROBE_DIM + WISH_DIM

# must match csai_policy.inc
N_ACTIONS = 16   # absolute: 2 sides x 8 trims

EP_FELL, EP_FINISHED, EP_TIMEOUT, EP_STUCK = 1, 2, 3, 4
OUTCOME_NAMES = {1: "fell", 2: "finished", 3: "timeout", 4: "stuck"}

# 9 core fields + the probe, which needs engine collision and so is logged
# rather than recomputed here.
REC_FLOATS = 9 + PROBE_DIM + 1   # +1: held wish angle


class Track(object):
    """Centerline, mirroring csai_track.inc."""

    def __init__(self, path):
        xs, ys, zs, ss = [], [], [], []
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                p = line.split()
                if len(p) < 5:
                    continue
                xs.append(float(p[1])); ys.append(float(p[2]))
                zs.append(float(p[3])); ss.append(float(p[4]))
        self.x = np.array(xs, dtype=np.float64)
        self.y = np.array(ys, dtype=np.float64)
        self.z = np.array(zs, dtype=np.float64)
        self.s = np.array(ss, dtype=np.float64)
        self.n = len(xs)
        self.length = self.s[-1] if self.n else 0.0
        self.pts = np.stack([self.x, self.y, self.z], axis=1)

    def nearest(self, pos, hint):
        """Windowed search, exactly as the plugin does it. hint < 0 = full scan."""
        if self.n == 0:
            return -1
        if hint < 0:
            lo, hi = 0, self.n - 1
        else:
            lo = max(hint - 16, 0)
            hi = min(hint + 64, self.n - 1)      # asymmetric, forward-biased
        d = self.pts[lo:hi + 1] - pos
        return lo + int(np.argmin(np.einsum("ij,ij->i", d, d)))

    def project(self, pos, idx):
        """Project onto the two segments adjacent to idx. Returns (s, closest, dist)."""
        best_s = self.s[idx]
        best_d = 1.0e18
        closest = self.pts[idx].copy()

        for k in (-1, 0):
            a = idx + k
            b = a + 1
            if a < 0 or b > self.n - 1:
                continue
            pa = self.pts[a]
            e = self.pts[b] - pa
            len2 = float(e @ e)
            if len2 < 1e-4:
                continue
            t = float((pos - pa) @ e) / len2
            t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
            p = pa + e * t
            dv = pos - p
            d = float(dv @ dv)
            if d < best_d:
                best_d = d
                best_s = self.s[a] + (self.s[b] - self.s[a]) * t
                closest = p
        return best_s, closest, math.sqrt(best_d)

    def tangent_yaw(self, i):
        a, b = i, i + 1
        if b > self.n - 1:
            a, b = self.n - 2, self.n - 1
        if a < 0:
            return 0.0
        return math.degrees(math.atan2(self.y[b] - self.y[a], self.x[b] - self.x[a]))

    def build_obs(self, pos, vel, idx):
        obs = np.zeros(OBS_DIM, dtype=np.float64)
        if idx < 0 or self.n == 0:
            return obs

        s, closest, _dist = self.project(pos, idx)

        speed = math.hypot(vel[0], vel[1])
        tan_yaw = self.tangent_yaw(idx)
        vel_yaw = math.degrees(math.atan2(vel[1], vel[0])) if speed > 1.0 else tan_yaw

        d_yaw = math.radians(vel_yaw - tan_yaw)

        tr = math.radians(tan_yaw)
        tx, ty = math.cos(tr), math.sin(tr)
        ox, oy = pos[0] - closest[0], pos[1] - closest[1]
        lateral = tx * oy - ty * ox

        obs[0] = speed / 1000.0
        obs[1] = vel[2] / 1000.0
        obs[2] = math.cos(d_yaw)
        obs[3] = math.sin(d_yaw)
        obs[4] = lateral / 256.0
        obs[5] = (pos[2] - closest[2]) / 256.0
        obs[6] = (s / self.length) if self.length > 0 else 0.0

        vr = math.radians(vel_yaw)
        cy, sy = math.cos(vr), math.sin(vr)
        for k, off in enumerate(LOOK_OFFSETS):
            j = min(idx + off, self.n - 1)
            rx = self.x[j] - pos[0]
            ry = self.y[j] - pos[1]
            rz = self.z[j] - pos[2]
            scale = off * 64.0                  # nominal arc distance
            o = 7 + k * 3
            obs[o + 0] = ( cy * rx + sy * ry) / scale
            obs[o + 1] = (-sy * rx + cy * ry) / scale
            obs[o + 2] = rz / scale
        return obs


class Episode(object):
    __slots__ = ("outcome", "start_state", "best_s", "steps")

    def __init__(self, outcome, start_state, best_s, steps):
        self.outcome = outcome
        self.start_state = start_state
        self.best_s = best_s
        self.steps = steps            # (n, 9) float32

    @property
    def n(self):
        return self.steps.shape[0]

    @property
    def terminal(self):
        """True when the episode ended in a real terminal state, not a cutoff.

        Timeout and stuck are time limits, not terminals: bootstrapping through
        them is correct, and treating them as absorbing would teach the agent
        that running out of clock is as bad as falling.
        """
        return self.outcome in (EP_FELL, EP_FINISHED)


def read_batch(path):
    with open(path, "rb") as fh:
        blob = fh.read()

    episodes = []
    off = 0
    total = len(blob)
    while off + 16 <= total:
        n, outcome, start_state = struct.unpack_from("<iii", blob, off)
        best_s = struct.unpack_from("<f", blob, off + 12)[0]
        off += 16
        need = n * REC_FLOATS * 4
        if n < 0 or off + need > total:
            raise ValueError("truncated batch %s at offset %d (episode claims %d steps)"
                             % (os.path.basename(path), off, n))
        steps = np.frombuffer(blob, dtype="<f4", count=n * REC_FLOATS, offset=off)
        steps = steps.reshape(n, REC_FLOATS).astype(np.float64)
        off += need
        episodes.append(Episode(outcome, start_state, best_s, steps))

    if off != total:
        raise ValueError("trailing %d bytes in %s" % (total - off, os.path.basename(path)))
    return episodes


def episode_obs(track, ep):
    """Rebuild the observation sequence, reproducing the plugin's hint evolution."""
    n = ep.n
    obs = np.zeros((n, OBS_DIM), dtype=np.float64)
    if n == 0:
        return obs

    # Ep_Begin does a full scan; every tick after that is windowed. The plugin
    # advances the hint every physics tick, we only see decision steps, so the
    # window is widened by the frame-skip factor implicitly via hint drift.
    hint = -1
    for i in range(n):
        pos = ep.steps[i, 0:3]
        vel = ep.steps[i, 3:6]
        hint = track.nearest(pos, hint)
        obs[i] = track.build_obs(pos, vel, hint)
        # splice in what the actor actually saw but we cannot recompute:
        # the collision probe, and the wish angle it was holding
        base = 7 + 3 * LOOKAHEAD
        obs[i, base:base + PROBE_DIM] = ep.steps[i, 9:9 + PROBE_DIM]
        if WISH_DIM:
            wish = math.radians(float(ep.steps[i, 9 + PROBE_DIM]))
            obs[i, base + PROBE_DIM + 0] = math.cos(wish)
            obs[i, base + PROBE_DIM + 1] = math.sin(wish)
    return obs


def load_batch(track, path):
    """Returns (obs, actions, old_logp, rewards, dones, episode list)."""
    eps = read_batch(path)
    obs_l, act_l, logp_l, rew_l, done_l = [], [], [], [], []
    for ep in eps:
        if ep.n == 0:
            continue
        o = episode_obs(track, ep)
        obs_l.append(o)
        act_l.append(ep.steps[:, 6].astype(np.int64))
        logp_l.append(ep.steps[:, 7])
        rew_l.append(ep.steps[:, 8])
        d = np.zeros(ep.n, dtype=np.float64)
        d[-1] = 1.0 if ep.terminal else 0.0
        done_l.append(d)

    if not obs_l:
        return (np.zeros((0, OBS_DIM)), np.zeros(0, dtype=np.int64),
                np.zeros(0), np.zeros(0), np.zeros(0), eps)

    return (np.concatenate(obs_l), np.concatenate(act_l), np.concatenate(logp_l),
            np.concatenate(rew_l), np.concatenate(done_l), eps)


def batch_stats(eps):
    if not eps:
        return {}
    outcomes = {}
    for e in eps:
        outcomes[OUTCOME_NAMES.get(e.outcome, "?")] = outcomes.get(OUTCOME_NAMES.get(e.outcome, "?"), 0) + 1
    returns = [float(e.steps[:, 8].sum()) for e in eps if e.n]
    lens = [e.n for e in eps]
    return {
        "episodes": len(eps),
        "steps": int(sum(lens)),
        "mean_len": float(np.mean(lens)) if lens else 0.0,
        "mean_return": float(np.mean(returns)) if returns else 0.0,
        "outcomes": outcomes,
    }


def load_state_arclengths(track, states_path):
    """
    Arc length of each replay start state.

    Needed because an episode's `best_s` is an ABSOLUTE position on the track. An
    episode that starts at the 80% checkpoint and immediately falls still reports
    best_s = 0.80, so reporting that as "progress" makes a policy that does
    nothing look 80% successful. What matters is best_s minus the start.
    """
    out = []
    with open(states_path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if len(p) < 13:
                continue
            pos = np.array([float(p[3]), float(p[4]), float(p[5])])
            idx = track.nearest(pos, -1)
            s, _c, _d = track.project(pos, idx)
            out.append(s)
    return np.array(out)
