# Experiment log

Target: `surf_demise`, 66 tick. Human reference **39.10 s**, 100% of track.
Metric: `gain %` — mean fraction of the track advanced from the episode's own
start, averaged over 64 episodes. (Not absolute position: episodes begin at
random checkpoints, so absolute progress flatters a policy that does nothing.)

| # | configuration | gen10 | gen20 | gen40 | gen100 | final |
|---|---|---:|---:|---:|---:|---:|
| 1 | obs22, centerline only, γ=0.99 | 1.70% | 2.78% | 4.43% | 6.34% | **6.67%** (gen 268) |
| 2 | obs28, + logarithmic lookahead, γ=0.997 | 0.81% | 2.34% | 4.49% | 6.99% | **6.86%** (gen 201) |
| 3 | obs36, + surface probe | 1.00% | 3.98% | — | — | 5.82% (gen 26, cut short) |
| 4 | obs38, + delta actions (wide ladder) | 0.66% | 0.90% | — | — | 1.31% (gen 28, abandoned) |
| 5 | obs38, tight ladder, ent 0.003 | — | — | — | — | running |

## What each change was, and whether it worked

### 1 → 2: longer horizons. **No effect.**

Measured that the policy could see **0.53 s** ahead at the map's median 3614 u/s
(0.40 s at peak) while episodes lasted **11.7 s**, and that γ=0.99 at frame-skip 2
gave a **3.0 s** reward horizon. Both indefensible. Fixed with logarithmic
lookahead offsets (256…16384 units, ~4.5 s) and γ=0.997 (~10 s).

Result: 6.86% vs 6.67%. Within noise. The horizons were genuinely too short, but
they were not what was binding — two independent configurations converging to
within 0.2% is the signature of a structural limit, not a tuning problem.

### 2 → 3: the surface probe. **Helped early; cut short.**

Dissecting a death rather than theorising: at termination the bot was 346 units
to the side but **502 units below** the centerline and accelerating downward. It
had genuinely fallen off the ramp — the deviation limit was not the culprit.

That indicted an early design decision. The observation was built from the
centerline alone, on the reasoning that "following the line *is* surfing, because
the line runs along the ramps". True while the agent is *on* the line; once it
drifts it has no idea where the ramp surface is, so it can follow but never
recover.

Added 5 collision rays (down and diagonal, velocity frame) plus the nearest
surface normal. The probe demonstrably reads real geometry: **`n.up` measures
0.506–0.571**, and a surf ramp's normal sits just below Source's 0.7 walkable
threshold. `n.fwd`/`n.left` span ±0.8 — ramp orientation relative to travel,
exactly the steering signal that was missing.

Cost: the probe needs engine collision, so it cannot be recomputed in Python.
Records grew 9 → 17 floats. That trade should have been made from the start.

gen20 3.98% against 2.78%/2.34% — the best early learning of any run, but
superseded before it could be judged at gen 100.

### 3 → 4: delta actions. **Regressed. Diagnosis right, implementation wrong.**

Prompted by watching a replay: the bot visibly snapped left and right. Measured
against a human on the same map:

| | median \|Δyaw\|/tick | p95 | reversals/s |
|---|---:|---:|---:|
| bot (absolute actions) | 0.00° | 57.6° | 4.0 |
| human | 0.36° | 2.24° | 0.4 |

Cause: the policy chose an **absolute** aim angle, re-picked 33×/s, so
consecutive choices were statistically independent. It had no way to express
"keep doing what I was doing" — smoothness had to be rediscovered every decision,
and the accelerations largely cancelled.

Switched to choosing a *change* in aim, integrating a held angle (and adding
sin/cos of that angle to the observation — with deltas the absolute angle is
internal state, and a policy that cannot see the variable it controls is blind).

It got worse: median 18.85°, p95 161°. The ladder was the fault. Under a
near-uniform policy the expected step is the mean |entry|, and rungs at ±115/±160
made that **36.8°/decision** — the aim random-walked as violently as before, just
with correlation.

### 4 → 5: tight ladder + less entropy pressure

Ladder capped at ±50 and densified near zero: expected step **12.8°** instead of
36.8°, while a 180° side-switch is still reachable in ~5 decisions (0.15 s).

Separately: after 4.66M steps entropy had only fallen 3.22 → 2.31 of a maximum
3.22. The policy never committed to anything. `ent_coef` 0.01 → 0.003: surf needs
sustained precise control, not sustained exploration.

### 5 → 6: the action space, derived from human play. **The real fix.**

Stopped guessing and measured the control law in the 39.10 s human run
(2525 strafing frames, phi = wish direction relative to velocity):

| statistic | value |
|---|---|
| phi p1 / p50 / p99 | **-90.54 / +89.43 / +90.73** |
| frames with abs(phi) in (85, 95) | **98.1%** |
| theoretical usable window at 3614 u/s | **abs(phi) >= 89.52** |

The player holds phi at +-90 essentially always, and the physics forces it:
`addspeed = wishspd - speed*cos(phi)`, so below 89.5 deg there is NO acceleration
at all, and near 180 deg the brake is up to 720 u/s in one tick.

Every earlier version offered the full 360 degrees. Measured on the best old
policy: it sat in the usable window **2.1% of the time** — 98% of every run spent
in a regime where its inputs did literally nothing. That was the plateau.

New space: 10 actions — hold, flip side, 8 trims — with `side` STICKY, so holding
a line is the cheap default. Immediately, at generation 38 and essentially
untrained: **77.8% in window**.

Also fixed the input decode. The old one held one strafe key and snapped the view
180 degrees to switch sides; a human switches the KEY and keeps the view on their
velocity (measured view-flip rate 0.00/s). Choosing the key from the sign of phi
collapses `viewyaw` to `velYaw +- trim` in both cases. Bot view-flip rate went
2.56/s -> **0.00/s**, matching the human, and the replay became watchable.

| | phi in window | view flips/s | med speed |
|---|---:|---:|---:|
| old absolute actions (gen116) | 2.1% | 2.56 | 1072 |
| human-derived (gen65) | **77.8%** | **0.00** | 409 |
| human | 98.1% | 0.00 | 3614 |

### 6 → 7: trim ladder straddling perpendicular

The action space was right in structure but wrong in range. Comparing the bot's
own wish angles against the human's, bucket by bucket:

| \|phi\| | bot | human |
|---|---:|---:|
| 90.0-90.6 | 0.0% | 10.5% |
| 91.5-93.0 | 9.5% | 0.2% |
| 93.0-96.0 | **34.9%** | 0.0% |
| > 95 (braking) | 11.1% | 0.0% |

Two faults. The bot lived at 93-96 degrees, where `accelspeed` saturates at
`sv_airaccelerate*maxspeed*frametime` and the player brakes — a region the human
never enters (their max is 93.18). And more basic: the ladder was
`side * (90 + trim)` with `trim >= 0`, so **nothing below 90 degrees was
reachable at all**, while the human's median is 89.43. Half their operating band
was inexpressible.

New ladder is signed and spans 89.0 .. 92.0, covering the measured human band
(89.4 .. 90.7, max 93.18) at 0.25-degree resolution. The 8/15/25-degree rungs are
gone.

Sharp turns do not need large |phi|: at 90 degrees the turn rate is already
30/speed rad/tick, about 114 deg/s at 1000 u/s. Big direction changes come from
flipping side.

Matched-generation comparison:

| gen | trim >= 90 | trim straddles 90 |
|---:|---:|---:|
| 5 | 0.76% | **2.51%** |
| 10 | 0.83% | **2.03%** |
| 20 | 1.01% | **2.13%** |
| 30 | 1.65% | **1.92%** |

Largest advantage early, which fits the mechanism: every action now lands in the
usable window, so even random exploration is productive instead of mostly inert.

### Reading `phi in window` correctly

The headline number understates the fix, because measured phi is not commanded
phi. At frame-skip 2 the angle is held for two ticks while velocity rotates, so
the measurement drifts by roughly twice the per-tick turn rate (30/speed rad):

| speed | phi in window | per-tick turn |
|---:|---:|---:|
| 50-300 | 0.0% | 9.8 deg |
| 300-600 | 74.2% | 3.8 deg |
| 600-1200 | 97.1% | 1.9 deg |
| **1200-5000** | **100.0%** | **0.55 deg** |

Commanded phi is always within [89, 92] by construction. At the speeds where
surfing actually happens the bot is at 100%; the shortfall is entirely a
low-speed artefact, and it shrinks as the policy gets faster. Against the
original action space's 2.1% overall, this part of the interface is now correct.

## Behaviour cloning

PPO with progress reward kept producing jitter: 19.79 side switches per second
against the human's ~1. That is what the reward asked for, so cloning the human's
inputs replaced it as the way technique enters the policy.

### Attempt 1 — transitional actions. Failed.

With actions as TRANSITIONS (hold / flip side / set trim), cloning could not fit
the data at all:

| | value |
|---|---|
| val accuracy | 0.166 |
| always-HOLD baseline | **0.724** |
| cloned flip rate | 6.07/s (human 1.10/s) |

The label was unlearnable by construction: the same situation is HOLD or FLIP
depending on a tracked side the observation does not contain. A probe settled it
— predicting the human's absolute side from the observation scored 0.959 against
a 0.613 majority baseline. The technique was in the data; the action space could
not express it.

### Attempt 2 — absolute actions. Fitted, then failed in the world.

Actions became absolute `(side, trim)`, 2 x 8 = 16 classes. Cloning fitted
immediately — and the policy still scored **0.1% of the map**, with **0.00 side
switches per second**. It welded itself to one strafe key.

The cause was in the observation. Its last two features were cos/sin of the
previous wish angle — the previous action. A human changes key ~1/s out of 33
decisions/s, so:

```
previous-action feature alone predicts the human's side: 0.967
```

Cloning learned the latch rather than the surfing. At capture time that feature
carries the human's real input; at eval it carries the policy's own last output,
so it closes a loop on itself. This is causal confusion (the "copycat" problem),
and `flips/s 0.00` is its signature.

Ablating the feature costs little and fixes the loop:

| | side acc | switches |
|---|---|---|
| with previous action | 0.972 | 1.10/s |
| **ablated** | **0.917** | **0.91/s** |
| human | — | 1.07/s |

0.917 against a 0.613 majority baseline: the geometry alone carries the
technique. `WISH_DIM` is now 0 and `OBS_DIM` is 36.

### Result

Removing it changed the behaviour completely, on identical data:

| | before | after |
|---|---|---|
| progress | 0.1% | **4.0%** |
| steering jitter (med abs dyaw) | 23.24 deg | **1.17 deg** |
| phi in window | 8.6% | **85.0%** |
| median speed | 204 u/s | **973 u/s** |
| outcome | stuck, motionless | falls while surfing |

The clone surfs properly and falls at 4% because it rides one side until the ramp
ends — the covariate-shift ceiling of cloning a *single* 1213-sample trajectory.
That is the case for RL on top, not for more cloning.

### RL from the clone

PPO is now anchored: `KL(pi || clone)` is priced into the loss (`--kl-ref`,
default 0.03) against a frozen `ckpt_bc.npz`. Without it, progress reward is free
to walk straight back to jitter, which tracks the centerline more tightly than
real technique does. The anchor gradient is checked by finite differences
(worst relative error 7.5e-06).

### A bug found on the way

The strafe-switch penalty had never once taken effect — it was charged and then
overwritten by the accumulator reset one line below it:

```
if (g_iEpSide != prevSide) g_fEpStepReward -= g_fSwitchCost;   // charged
...
g_fEpStepReward = 0.0;                                          // discarded
```

Every reported result that claimed to price side switches was in fact unpriced.

## Anchored RL from the clone

Resuming PPO from the cloned policy with `--kl-ref 0.03`:

| gen | mean progress | best | kl_ref |
|---:|---:|---:|---:|
| 3 | 6.31% | 8.03% | 0.002 |
| 22 | 8.01% | 11.37% | 0.119 |
| 40 | 9.62% | 14.58% | 0.317 |
| 58 | **18.56%** | **32.74%** | 0.294 |

Greedy eval went 4.0% (clone) to **15.8%** on one continuous run. The anchor held:
technique got *better* while speed nearly doubled, which is what it was for.

| | old PPO | clone | gen 57 | human |
|---|---|---|---|---|
| steering jitter | 23.24 deg | 1.17 deg | **0.55 deg** | - |
| phi in window | 8.6% | 85.0% | **93.6%** | 98.1% |
| median speed | 204 | 973 | **1941 u/s** | - |

### The remaining ceiling: it never switches

At gen 57 the bot made **0 strafe-key switches in 10.30 s**. Measured over the
same stretch of track, the human made **10, at 0.89/s**.

The bot is *faster* than the human there - 15.8% in 10.30 s against 11.26 s -
because holding one key and carrying 1941 u/s is locally optimal. It is a
degenerate strategy that works until the geometry requires a switch, which is
exactly where it dies.

### Switch cost: set to 0, then restored

First it was set to 0, on the strength of a measurement that turned out to be
garbage. Recording both halves, because the second half is the useful one.

`replayqa.py` counted a side switch as a **>90 deg jump in view yaw**. Switching
strafe key does not swing the view - the view turns smoothly and only the key
changes - so the metric reported **0.00/s for every replay ever measured,
including the human's**. That zero was read as "the policy never switches
sides", and the penalty was removed for taxing a behaviour believed absent.

The control that should have been run first: the same metric on the human's own
replay. It reads 0.95/s once fixed, and 0.00/s before - an obvious failure,
available at any point for the cost of one command.

With switches counted from the buttons instead:

| | flips/s | med abs dyaw | phi in window | med speed |
|---|---:|---:|---:|---:|
| clone (gen 1) | 3.94 | 1.17 deg | 85.0% | 973 |
| gen 57 | 6.11 | 0.55 deg | 93.6% | 1941 |
| gen 143 | **8.35** | 0.71 deg | 94.9% | 3298 |
| human | **0.95** | 0.36 deg | 98.1% | 3614 |

The opposite of the earlier reading: the bot switches 8.8x too often, and is
getting worse as it trains. The KL anchor alone does not hold it. Cost restored
to 0.15.

### Why a stochastic policy over-switches

Not a bug. Sampling a policy that is ~90% sure of its side flips that side on
roughly `2p(1-p)` of decisions - about 16%, or ~5/s at this cadence. The clone's
3.94/s is very close to what its own 0.906 side accuracy predicts. Any stochastic
policy choosing an absolute side every decision will jitter unless it is either
sharply confident or explicitly penalised for changing.

### Greedy is not the policy

At gen 143, from the same start, the same budget, the same deviation limit:

| | run 1 | 2 | 3 | 4 | 5 |
|---|---:|---:|---:|---:|---:|
| greedy (argmax) | 6.0% | 6.0% | 6.0% | 6.0% | 6.0% |
| sampled | 32.1% | 34.6% | **52.8%** | **62.7%** | 48.2% |

A discrete action space approximating a continuous steering angle can realise an
intermediate angle by dithering between adjacent trims. The argmax collapses that
mixture to one end and is a different, much worse controller. Every "eval"
number in this file before this point is a greedy number and understates the
policy badly.

`+csai_evalgreedy 0` runs the sampled policy; `tools/eval.ps1 -Greedy 0`.

The open tension: sampling is what makes it work (62.7%) and is also what makes
it jitter (8.35/s). The goal is a policy confident enough that its mode is its
behaviour - so greedy and sampled agree - which is what the switch cost and the
anchor are for.

## Throughput

One srcds instance is single-threaded and saturates around 4500 ticks/s. Parallel
instances are the only way past it.

| actors | steps/s | gen/min | scaling |
|---:|---:|---:|---:|
| 1 | 2245 | 2.4 | - |
| 4 | 8739 | 8.6 | 3.89x (97%) |
| 6 | 12005 | 11.8 | 5.35x (89%) |

Needed four fixes: per-actor batch filenames (every actor wrote batch_0000 over
the others), separate UDP ports, per-actor orphan cleanup (a restarting actor
deleted its siblings' in-progress batches), and oldest-first batch selection in
the learner - the old name-ordered pick always preferred actor 0 and let the rest
go stale. Off-policy lag costs nothing measurable: KL stays at 0.004-0.006.

## Frameskip 6

Reasoning: gamma 0.997 at frameskip 2 gives a 10 s horizon against 27 s episodes,
so the value function could not see the last two thirds of a run.

It did not improve progress. 180 generations settled at 40% mean against the 46%
baseline. It did improve technique sharply:

| | fs 2 | fs 6 | human |
|---|---:|---:|---:|
| strafe switches/s | 8.35 | 4.31 | 0.95 |
| p95 abs dyaw | 35.24 deg | 3.48 deg | 2.24 deg |
| phi in window | 94.9% | 97.6% | 98.1% |
| median speed | 3298 | 3276 | 3614 |

Kept, because the wall turned out to be elsewhere and the technique gain is real.

## The 60% wall was a reward cliff

Best progress sat at 60-63% through every hyperparameter change. Five of six eval
runs ended at 59-60% of the map, all within a unit or two of **600** - the
deviation limit. They were not falling. They were being ruled out of bounds.

The reward was `dS/100` and nothing else, so drifting 590 units off the route
cost exactly zero and then at 600 the episode died with no warning. A cliff with
no slope leading to it cannot be learned from.

Control run first, this time: the human's own deviation from the centerline built
from that run is 0 for the entire route, exceeding 600 only after the finish. So
the limit was not killing legitimate play.

Added `cost * (dist/maxDeviation)^2` per decision, quadratic so ordinary wobble
is nearly free (`+csai_devcost`, default 0.5):

| | mean | best |
|---|---:|---:|
| fs 6, no deviation cost | 40% | 61% |
| fs 6 + deviation cost | **49.7%** | 63.4% |

Better than the frameskip-2 baseline of 46% as well. Max deviation across a run
dropped from ~607 to ~278.

## What is left: one missed input at 59%

The wall did not move much, and the reason is specific.

| | human | bot |
|---|---|---|
| switches to D at | **59.0%** | **60.5%** |
| z, 57% -> 60% | 4701 -> 4204 | 4677 -> 3661 |
| speed through the section | 3810 | 3220 |

The bot matches the human's height within +-80 units for the first 57% of the
map. Then it holds A about 0.7 s too long through the transition at 59%, sinks
twice as fast as the human, and leaves the corridor.

It is also entering that section ~590 u/s slower, which is the likelier root
cause: the same line may simply not be holdable at 3220.

Not a tuning problem, and not a sampling problem either - at 12000 steps/s the
bot attempts this transition thousands of times per minute. The two candidates
are more human runs covering it, and letting episodes start near it so the
payoff is not 40 s of successful surfing away.

## Eight human runs, and why cloning them failed

Added an in-game recorder (`csai_record.inc`) because the timer cannot supply
extra runs - shavit keeps one replay per map and replaces it only on a
faster time, so slower runs are discarded. Seven new runs were recorded, times
39.0-40.3 s, plus the original.

Offline, the extra data looks like exactly the fix the bot needed:

| trained on | n | side acc across ALL runs | switches/s |
|---|---:|---:|---:|
| run 1 only | 406 | 0.621 | **0.18** |
| all 8 runs | 3090 | **0.800** | **1.26** |
| human | - | - | 1.10 |

The single-run clone scores 0.850 on its own validation split and 0.621 across
the other runs: it memorised one line. It also switches strafe key 0.18 times a
second against the human's 1.10, which is the exact pathology seen in the bot.

Closed loop, the ranking inverts:

| policy | best of 5 |
|---|---:|
| gen 789 RL | **59.2%** |
| gen 789 + BC fine-tune on 8 runs | 32.2% |
| clone, 1 run | 6.8% |
| clone, 8 runs | **1.1%** |

More demonstrations made cloning worse, and fine-tuning a good policy on them
destroyed half its performance. This is mode averaging: where one run went left
and another went right through the same place, a single-headed policy learns the
average of the two and follows neither. One narrow but decisive demonstrator beats
eight that disagree.

The standing lesson repeats. Imitation metrics measured on the demonstrator's own
states said the opposite of what happened once the policy was driving.

## A third measurement bug

`eval.ps1` hardcoded `+csai_frameskip 2` while training ran at 6, so from
generation 254 every eval replay drove the policy at three times its decision
rate. The 59% analysis in the previous section was performed on one of those
replays and had to be redone. `eval.ps1` now takes `-FrameSkip`, `-DevCost` and
`-SwitchCost`, and the daemon passes its own settings through.

Three measurement bugs in one day - replayqa counting view jumps instead of key
changes, bc.py hardcoding the decision rate, eval.ps1 hardcoding frameskip. Each
produced confident, wrong numbers that drove a decision. The tooling deserves the
same suspicion as the policy.

## The wall, measured correctly

| | human | bot (gen 789) |
|---|---:|---:|
| speed at 51.0% | 3586 | 3516 |
| speed at 51.5% | 3587 | **1450** |
| speed 52-59% | 3587-3811 | 3225-3254 |
| z at 57% | 4771 | 4625 |
| z at 59.5% | 4443 | 3812 |

The failure does not start at 59%. It starts at **51.5%**, where the bot loses
2000 u/s in a single tick - it clips something. It never recovers the human's
speed afterwards, running the rest of the section ~580 u/s slow, which is why it
sinks through the 58-60% descent and leaves the corridor at 59.5%.

Chasing the switch timing at 59% was chasing a symptom.

## Vertical aim

The bot held pitch at exactly 0 for every run - staring dead level, which no
player does and which makes a replay look wrong however good the line is.

Source zeroes the z component of the forward and right vectors and renormalises
before building wishdir, so pitch never enters the movement maths. Verified
rather than assumed: a fixed 45 degree pitch and the fitted natural pitch produce
**bit-identical** runs (72.6 / 72.7 / 53.3 in both). Pitch is free to set for
looks alone.

(The first attempt at that test ran while six actors were training and gave
different results for identical configs. Physics depends on frame timing and the
machine was loaded. Determinism tests need a quiet machine.)

Fitted to the 8 recorded runs: the human tracks their own velocity direction at
0.575 gain with a 6.06 degree downward bias, correlation 0.69. More important
than the angle is how it MOVES - 0.08 deg/tick median, 0.30 at p95 - so a
first-order lag of 0.05 with a 0.30 deg/tick cap was fitted to reproduce that.

| | bot gen 1143 | human |
|---|---:|---:|
| median pitch | 10.7 deg | 8.5 deg |
| pitch change p95 | 0.30 deg/tick | 0.31 deg/tick |
| strafe switches/s | 1.50 | 1.05 |
| median abs dyaw | 0.45 deg | 0.33 deg |
| phi in window | 97.9% | 97.8% |
| median speed | 3356 | 3589 |

Greedy evaluation reaches 72.6% at generation 1143, against 6.0% at generation
143.

## Prestrafe

The replayed prestrafe was 67 ticks against the human's real 63-113, so the bot
appeared at the start line already moving instead of winding up.

Two bugs surfaced on the way to fixing that.

### Teleports inside the recordings

The recorder keeps a rolling buffer before the timer starts, so it captures the
player respawning - a jump of up to 21772 units in one tick, from wherever the
previous attempt ended. `Demo_Begin` teleports the bot to tick 0 and replays from
there, so those runs began in the middle of the map and every captured
observation was paired with a human action taken somewhere else.

This is what produced the earlier result that cloning 8 runs (1.1%) was far worse
than cloning 1 (6.8%), read at the time as mode averaging. The data was simply
corrupt. Both the recorder and the loader now cut everything before the last
teleport.

With clean data the offline numbers improve a lot:

| trained on | side acc across all runs | switches/s |
|---|---:|---:|
| run 1, corrupt capture | 0.621 | 0.18 |
| run 1, clean | 0.671 | 0.67 |
| all 8, corrupt capture | 0.800 | 1.26 |
| **all 8, clean** | **0.867** | **1.14** |
| human | - | 1.10 |

Closed loop it is still only 2.3% against the single run's 6.8%, so the original
conclusion survives in weaker form: cloning is a poor policy here either way, and
the trained policy at 72.6% is what matters. But the stated reason was wrong.

### The policy is brittle to its opening state

Every recorded run carries its own prestrafe, so all of them are now kept and one
is sampled per episode. Switching evaluation from the 67-tick prestrafe to the
113-tick one, changing nothing else, took the same policy from **72.4% to 11.3%**.

It had only ever started from one handover position, speed and angle, and could
not handle a different one. Training now samples all 8 sets; evaluation uses the
longest, which both looks like a real approach and stays deterministic. Numbers
before and after this change are not comparable.

## The 72% drop

Training plateaued with mean 72.2% and best 73.0% - nearly every run ending in
the same place. It is a drop with a hard right turn, and the bot flies straight
past it.

First reading was wrong. The human's turn rate there reaches 305 deg/s, far
beyond what air acceleration can deliver (about 28 deg/s at 4000 u/s), so it
looked like a ramp redirecting them. But their vertical velocity through the
whole section changes by exactly -12 u/s per tick - clean gravity, no vertical
clipping at all. The surface is a near-vertical wall: steep enough to redirect
horizontal velocity hard while leaving the fall untouched.

| | turn rate |
|---|---:|
| human, 72.5-73.3% | -180 to -305 deg/s |
| bot, 71.3-72.2% | -95 deg/s |
| bot, 72.4% on | -12 deg/s (no contact) |

Dumping the observation through the drop showed why. The bot is hugging geometry
at 70.3% and has separated half a second later:

| progress | down | fwd-dn | back-dn | left-dn | right-dn |
|---|---:|---:|---:|---:|---:|
| 70.3% | 0.07 | 0.12 | 0.09 | 0.05 | 1.00 |
| **70.8%** | **1.00** | **1.00** | **1.00** | **0.65** | **1.00** |
| 71.3% | 0.97 | 1.00 | 1.00 | 0.57 | 1.00 |

Four of five rays see nothing within 512 units; the wall it has to ride is a
single oblique reading at ~330 units. By then it is already too late - closing
300 units sideways needs about 0.55 s and it has 0.3 s.

All five rays pointed down, which is right for a ramp underfoot and wrong for a
wall alongside. Three level rays were added (left, right, forward) at 1024 units
rather than 512, since 512 is under two decisions of warning at surf speed.

Rays 0-4 keep their exact directions and range, and the surface normal is still
taken from those five alone, so every pre-existing observation value is
unchanged. The trained policy was carried across by copying its first-layer
columns into their new positions and zeroing the three new ones - it evaluates at
72.5% after the migration, the same as before, and can now learn to use the new
inputs rather than starting over.

## The 72% drop, continued

### Mixed starts ruled exploration out

30% of episodes were started from replay checkpoints at 61.3%, 66.5% and 71.7%
of the track - the last sitting right at the drop - so the bot entered it
repeatedly from varied approaches instead of once per full run.

It made no difference. Every episode still ended there, including the ~20 per
batch that began at 71.7% carrying the human's own recorded velocity. Mid-map
episodes gained essentially nothing. So the bot is not failing for want of
practice or of a lucky success to reinforce; it cannot clear the drop even when
handed the state the human clears it from.

Two bugs found while establishing that, both mine:

- The setting was not reaching the running actors at all, and the plateau was
  being watched as if it were under treatment. A `mid N` counter in the batch
  line settled it in one look. Instrument the thing being changed.
- The learner had a 75-batch backlog, oldest 45 minutes stale, and consumes
  oldest-first - so it was training on pre-change data while the actors produced
  post-change data, and the two disagreed by 25 points of progress. With sync on,
  one consumed batch unblocks every actor at once, so the queue grows
  structurally. The learner now drops anything more than `--max-lag` generations
  behind, which PPO needed anyway: its trust region is meaningless on data that
  far off-policy.

### Not the deviation limit either

The obvious next suspect, since the corridor is measured against the human's line
through a 305 deg/s turn. Raising the limit from 600 to 1500 moved best progress
from 73.0% to 73.2%. It is genuinely falling, not being ruled out of bounds.

### What the action space cannot do

The bot must press a strafe key on every decision, at phi ~ +-90. It has no way
to press nothing. Across the 8 recorded runs the human does exactly that far more
at this spot than anywhere else:

| | one key | no strafe key | both |
|---|---:|---:|---:|
| whole run | 92.8% | 5.6% | 1.5% |
| 68-76% (the drop) | 86.5% | **12.2%** | 1.3% |

With no strafe key there is no wish direction and no air acceleration at all -
coasting stops pushing the player off the surface being ridden. Forced to
accelerate every tick, the bot cannot hold a delicate contact.

Added action 16, POL_COAST: presses nothing, keeps the held side so resuming does
not register as a strafe switch. The trained policy was carried over by copying
the 16 existing head rows and giving the new one zero weights and a -0.5 bias, so
it starts at about 3.7% and PPO can find it rather than being forced into it.

### A configuration slip worth recording

Training silently reverted from frameskip 6 to 2 on a restart, because the
daemon's `-FrameSkip` default was 2 and the restart did not pass it. Nothing
looked wrong: evaluation defaulted the same way, so actor and eval agreed with
each other and reported a plausible 73%. It surfaced only when a manual eval at
frameskip 6 returned 8.5% for a policy that measures 73% at 2.

A policy is not portable across decision rates, and a default that silently
disagrees with the checkpoint is a trap. The default is now pinned to the
checkpoint's rate with that written next to it.

## The 72% drop: solved as a diagnosis

The user's observation was that the bot "slightly looks down and stops steering"
and dives, at an identical point every time. That turned out to be literally
true, and measurable.

### Turning and losing speed are the same act

Source applies `addspeed = 30 - speed*cos(phi)` per tick, capped by
`sv_airaccelerate * wishspeed * frametime` (1012 here, never the binding limit).
Past 90 degrees `cos(phi)` is negative, so `addspeed` GROWS with phi - and the
same acceleration has a negative component along velocity. Turn rate at 4147 u/s:

| phi | turn rate |
|---:|---:|
| 89.75 | 11 deg/s |
| 91 | 94 deg/s |
| 92 | 161 deg/s |

The bot going into the drop turns at 94 deg/s, then settles on a trim that can
only manage 11. At that angle and that speed it has, almost exactly, stopped
steering. (An earlier note here claimed the 94 deg/s came from wall contact. It
does not - it is plain air acceleration at phi 91.)

### What actually works

Forcing a single trim for a whole episode, starting from the checkpoint at the
drop (71.7%), and from the one before it (66.5%):

| phi | from 71.7% | from 66.5% |
|---:|---:|---:|
| 90.0 | 1.1% | **2.8%** |
| 90.5 | 1.3% | - |
| 91.0 | 1.6% | - |
| **92.0** | **3.8%** | 0.9% |
| 95 | 0.7% | - |
| 100 | 0.4% | - |
| 110 | 0.3% | - |

phi 92 clears the drop, reaching 75.5% where the policy reaches 72.8%. The same
phi 92 is bad on the approach. So the correct angle is sharply state-dependent,
the useful amount of braking is small, and heavy braking is worse than none.

Trims of 5, 10 and 20 degrees were added on the theory that the bot could not
turn hard enough, then measured, found strictly worse, and reverted. The action
that works - trim 7, phi 92 - had been in the action space the whole time.

### The real cause: the policy had collapsed

At the drop the policy chose the D side on **100% of decisions across 40
episodes**, with p(coast) = 0.001 and effectively zero mass anywhere else. A
saturated policy cannot discover phi 92 by sampling, and the mixed starts
therefore could not help however many attempts they provided. Entropy raised from
0.003 to 0.02.

### Two diagnostic bugs found on the way

`+csai_forceside -1` parsed as 0, because the command-line reader does not take
negative values. Every "forced D side" measurement was silently the unforced
policy, which is why a sweep of five very different trims returned identical
results - the tell that something was not connected. The encoding is now 1 = A,
2 = D.

The lesson repeats from earlier today: when a deliberately varied input produces
identical output, suspect the wiring before the physics.

## Hardening pass, and what a second map exposed

Three parallel audits (python tools, the plugin, the PowerShell orchestration)
plus the first attempt to run a map the project had never seen.

### Training was dead and everything said it was fine

A checkpoint migration wrote `range(4)` for the value network, which has SIX
tensors, so the critic's output layer was dropped. `learn.py --resume` died with
`KeyError: v4` on every restart - 59 times over 90 minutes - while the panel
reported RUNNING and the reports refreshed on schedule at a frozen generation.
Six cores produced nothing.

It was invisible because the learner's stdout and stderr went to a hidden console
and were discarded, and because the daemon logged `learner started` from
`Start-Process`, which returns before Python has parsed anything. A variable that
looked like a stall watchdog, `$lastGen`, was assigned and never read.

Fixed: learner output captured to `logs/learner.log`, the resume path validates
the critic as well as the policy, and the watchdog is real. The class of lesson:
**a log line that asserts success without checking it is worse than no log line.**

### The centerline was drawn through teleports

`clean_frames` concatenates the kept segments of a run; `centerline` then
resampled that concatenation by raw inter-frame distance. On a staged map the
gaps between segments became straight lines through nothing:

| map | track length before | after | real path |
|---|---:|---:|---:|
| surf_demise | 135,242 | 135,247 | one segment, unaffected |
| surf_dune | 110,277 | **61,216** | ~61,498 |

44% of surf_dune's "track" was void, and since progress along this polyline IS
the reward, crossing one teleport tick paid about 23,000 units at once - which
would have dominated everything the policy learned. surf_demise is a single
continuous segment, which is exactly why a year of work on it never showed this.

Carrying the resampling overshoot (rather than zeroing it) also made spacing
actually 64 units instead of a speed-dependent 90; surf_demise went to 2078
points, past the old `MAX_TRACK` of 2048 - which used to `break` silently and set
the track length to wherever it stopped, so the finish test fired mid-map and
paid the completion bonus.

### Recordings were never checked against the server tickrate

Playback feeds one recorded tick per server tick. surf_dune's replay is 100 tick
and this server is 66.67, so every input would be held 1.5x too long and every
velocity reconstructed 1.5x wrong, while demo capture paired those observations
with the human's actions. `replay.py` now re-times recordings to the server rate
and both loaders refuse a file that disagrees.

### Weights were committed before they were validated

`Pol_Load` wrote each parsed float straight into the live arrays and only then
checked the header and the count - returning false *after* corrupting them, while
still reporting the OLD generation. The learner rewrites that file every
generation and the plugin polls it four times a second. Now parsed into a scratch
buffer and committed only when complete.

### Smaller, same shape

- `report.py` carried `TRACK_UNITS = 135547.0` (already 305 units stale for its
  own map) and a reference time of 39.10 as constants; both now come from the
  map's own files. Reported max entropy was ln(10) for a 17-action space.
- `bc.py` bucketed the coast action as a left strafe, corrupting the side
  accuracy and switch rate that cloning is judged on.
- `unstick.py` carried a windowed search hint across checkpoints and across
  episode boundaries in a dump: 81% of rows had wrong track positions, by up to
  98 percentage points.
- An eval flag was cleared in exactly one place, so stopping an eval part-way
  made the *next* training run finish after one episode and report success.
- `eval.ps1` ran on the default port, which is actor 0's.
- The panel's "make a replay" drove a frameskip-2 policy at frameskip 6, with
  argmax - two different controllers writing into one replay list.
- `Viewer.bat` had never worked: `web
un.ps1` was stored as `web<CR>un.ps1`.

### Speed and smoothness became objectives

Until now the reward was distance only. A full run earns about 1352 from
progress and the finish bonus was 10 - 0.7% of the return - so two runs that both
finished scored the same whether they took 39 s or 60 s. Nothing asked for speed,
and nothing asked for technique beyond the switch cost. Left alone it would have
plateaued at "completes the map" and stopped wanting anything.

Added, all as flags:

| term | value | why |
|---|---|---|
| time cost | 0.08 per decision | a tick spent is a tick paid. Kept well under the ~0.99 a decision earns from progress: a living cost larger than the progress it interrupts makes dying early the better move |
| time bonus | 30 per second under the reference | paid only on runs that start at the beginning - a mid-map start has no comparable clock. Upside only, since the time cost already prices slowness |
| trim cost | 0.05 per aim change | nothing priced fidgeting with the angle; that is most of what separated the bot's 0.45 deg median view movement from the human's 0.33 |
| switch cost | 0.15 -> 0.40 | the human changes strafe key 0.95/s |

The reference time is read from the map's own states file rather than a constant.

## All eleven actors were running the same episodes

Generations 2674-2677 were byte-identical - same steps, same return, same
fell/finished/stuck - from four different actors. Every actor shares the policy's
default RNG seed (0x1234567), and `+csai_seed` exists but the daemon never passed
it, so any actors sitting on the same policy generation ran **exactly the same
episodes**. Not only the first batch: a5, a6 and a7 produced identical
batch_0002 as well. The learner then spent a separate PPO update on each copy.

Each actor now gets its own seed, re-randomised per restart. With fewer actors
and more timing jitter this was mostly hidden; at 11 actors it was most of the
machine's output.

## Watching for the next plateau without a human

`tools/autofix.py` runs the earlier diagnosis on a timer: if best progress has not
improved by 0.75 points over 150 generations, it stops training, runs
`unstick.py`, and restarts. Validated against this project's own history rather
than assumed - replaying the log, it reports PLATEAU at every check through the
73% wall (gens 1600-2300) and "ok" during healthy progress (+12.5 points over the
last 150). It backs up the checkpoint before each attempt, waits 45 minutes
between them, stops after six, and halts if training does not come back up.

## The wall moved to 88%

| progress | human | bot |
|---|---|---|
| 80% | 4341 u/s | 3704 |
| 87% | key D | key **A** |
| 88% | z -961 | z -1495 |

Same shape as the drop at 72%: a specific stretch where the policy commits to the
wrong side, sinks below the line, and leaves the corridor at the 600-unit limit.
The speed deficit is local, not general - median speed over a whole run is 3522
against the human's 3589, about 2%.

Technique is now close to human and better on the measure that matters most for
speed: phi inside the acceleration window 98.5% against the human's 97.8%, p95
view movement 2.44 deg against 2.21.

## Evaluation was measuring one opening state, not the policy

An automatic eval reported 6.9%, 6.0%, 7.1% while training reported mean gain
55-60% and a best of 88%. That gap is not improvement or regression, it is two
different measurements.

Evaluation always used the LONGEST recorded prestrafe - one of the eight the
policy trains against, chosen because it looks best in a replay. Training samples
all eight. Running the same policy at the same moment, cycling the prestrafes by
run index instead:

| eval method | runs |
|---|---|
| always the longest | 6.9, 6.0, 7.1 |
| cycling all eight | **88.4, 40.0, 40.0, 87.6, 87.9, 52.5, 88.4, 87.9** |

So the number that has been used to judge every change today was describing a
single opening state the policy happens to be bad at. The warning was already in
this file: changing only which prestrafe an eval used once took the same policy
from 72.4% to 11.3%, and that was recorded as evidence of brittleness rather than
acted on as a flaw in the measurement.

Evaluation now cycles the sets by run index - deterministic and reproducible, and
covering the spread training actually faces.

It also shows something real: five of the eight openings reach ~88%, three reach
40-52%. That brittleness is worth attacking, and it was invisible while the eval
reported a single number from a single state.

**The lesson is the day's recurring one.** A metric that silently narrows its
input reports confidently about something other than what you asked.

## Standing lesson, reinforced

Every one of these produced a plausible number or a green status rather than an
error. The audits were worth more than the fixes: three of the worst were in code
that had been read many times and looked right.

## Standing lesson

Every real defect was in the agent's **interface to the game** — what
it can perceive, and how it expresses intent — not in the learning algorithm,
the reward, or throughput. The algorithm was verified correct early (gradient
checks, observation parity, KL ≈ 0) and none of that verification would ever have
caught either bug.

The decisive defect was found by *watching a replay* and then *measuring the
human's own inputs* — not by reading training metrics. The plateau was visible in
the numbers across four runs and diagnosable in none of them.

Two attempts at the fix were wrong before the right one: delta actions made the
thrashing worse (the ladder averaged a 37 deg step under a near-uniform policy),
and a height-based scripted controller scored 0.0% because its sign convention
was guessed rather than measured. The reference data was available the whole
time.

A hand-coded controller is a cheap and decisive test of whether an interface is
adequate at all: it reached 8.5%, barely past the learner's 7%, which ruled out
the algorithm before any redesign started.

The cloning failures repeat the same pattern one level up. Both were interface
defects — an action space that could not express the label, then an observation
that leaked it — and both produced *better* offline metrics than the fix does
(0.972 side accuracy latched, 0.917 honest). An imitation metric measured on the
expert's own trajectory cannot see the failure that matters, because the failure
only exists once the policy is the one driving. The number that exposed it was
behavioural, not statistical: side switches per second.

## The bot finishes the map, and two metrics hid it

At gen 2691 an episode starting at the beginning of surf_demise reached the end.
By gen 2852 that was happening in 14 generations out of 52. The completed runs
take 39.75 to 40.38 seconds against the human's 39.05 - a median of 2.2% slower.

Nothing in the training log said so, because both of its progress columns are
blind to it in opposite ways.

`best_progress` is the share of the track an episode *gained*, so it stops at
1.0 the moment the bot can run the map end to end. From then on it is pinned,
and a watchdog reading it sees a permanent plateau. That is exactly what
happened: autofix stopped training at gen 2842 reporting "best stuck at 99.97%
for 150 generations" during the steepest improvement the run has ever had.

`finished` counts every episode that reached the end, and 30% of episodes are
spawned at a checkpoint three quarters of the way along. Those need ten seconds
of track, not forty. The column read 4-5 finishes per generation for hundreds of
generations before any episode ran the whole map.

So the headline number had to be computed from the batch records instead, by
filtering on `start_state == 0` - which is what tools/finishes.py now does, and
what autofix now switches to watching once anything finishes.

### Where the other 99% die

Of 1037 runs from the start, 895 - 86% of every failure - end between 85% and
90% of the track, centred on 87.8%. Everything else is noise. But the cause is
not there. Comparing the 8 completed runs against 250 failures band by band:

    band      speed win / fail     height vs the line, win / fail
    60-65%      3858 / 3835
    70-75%      3947 / 3909
    75.0-75.5%  3859 / 3811          +16 / -101
    76.0-76.5%  4114 / 3777           -8 /  -72
    77.0-77.5%  4326 / 3766           -2 / +165
    78.0-78.5%  4401 / 3763           +9 / +158
    80-85%      4295 / 3701
    87-88%      4294 / 3686          +67 / -481

The two populations are identical to within 5 u/s until 60%, and differ by 620
u/s by 78%. The whole gap opens in one 2.5% stretch: between 75.5% and 78% the
winners gain 540 u/s and the failures gain nothing at all. They enter that ramp
about 100 units below the line, catch it in the wrong place, and come out slow.

After that the runs are already decided. Both populations hold a flat speed from
79% onward - 4294 against 3686 - and the failures sink steadily below the line
(-153, -259, -382, -481) until they leave the corridor at 87.8%. Nothing goes
wrong at 87.8%; that is just where a run that was 600 u/s short since 78% runs
out of height.

This is the second time a fix has been aimed at the place a run visibly ends
rather than the place it was lost. The first was the 72% drop, where the
observable failure was real but three confident explanations of it were wrong.
The lesson is the same both times: the band where runs end is the *last* place
to look for the cause, because by then every run in the failing population
already shares the same state.

## Two missing braces stopped training for half an hour

A guard added the same evening to stop a failed batch being announced as
complete:

    if (g_hBatchFile == null)
        PrintToServer("[CsAI] could not open batch file %s", path);
        // Train_EndOfBatch would still write a .done marker claiming N episodes
        g_bBatchBroken = true;

SourcePawn binds only the first statement to a braceless `if`, so
`g_bBatchBroken = true` ran on *every* batch. `Train_WriteDoneMarker` returns
early when that flag is set, so from the moment the rebuilt plugin loaded, no
actor ever wrote a `.done` marker. The learner discovers work by scanning for
`.done` files. It found none and waited.

What that looked like from outside: eleven actors alive, each pinned at 100% of
a core, writing well-formed `.bin` files. A learner process alive with an empty
stderr. A daemon log showing eleven healthy actors. Nothing had crashed, so
every liveness check in the system passed for twenty-eight minutes while the
policy did not move a single generation.

Three things were supposed to catch it and none did:

  * the daemon's stall watchdog fired once at four minutes, wrote one line, and
    never acted again. It now restarts everything after twelve minutes, at most
    three times, and says so.
  * the health probe allowed ten minutes without a generation. A generation takes
    fifteen seconds. It now allows five.
  * neither knew the failure's fingerprint. Both now count `.bin` files that have
    waited over five minutes for a marker, which is the one signal that separates
    "the learner has nothing to do" from "the learner cannot see the work".

The 1440 episodes already written were not lost: the markers were reconstructed
from the batch contents and the learner consumed them.

A scan of every braceless `if`/`for`/`while` in the plugin found no other site
with a second statement indented under it. That check is worth keeping in mind
whenever a one-line guard grows a comment - the comment is what makes the extra
statement look like it belongs.

## The recorded runs disagree with the line the reward enforces

An episode is killed the moment it is more than 600 units from the centerline.
The centerline is built from exactly one recorded run. Nothing ever checked the
other recorded runs against it - and checking them says this, for surf_demise:

    demo    (the one cloning reads)  253 of 2734 ticks beyond 600, worst 2196
    demo_2                           156 of 2940 ticks beyond 600, worst 1408
    demo_3                           inside the corridor throughout, worst 470
    demo_4  (the track's own source) inside by construction, 39.04 s
    demo_6                           188 of 2875 ticks beyond 600, worst 1543
    demo_8                           107 of 2892 ticks beyond 600, worst  880

Four of the human's own runs spend one to three seconds outside the corridor
their own reward enforces. The file cloning reads is the worst of them: through
84-92% of the map it runs 800 to 1150 units from the track, which is a line the
reward would terminate on sight. Cloning was being taught technique that the
reward then punished, and both numbers looked fine on their own.

Two things were checked before believing this. The nearest-point search agrees
tick for tick with a full argmin, so it is not the windowed search drifting. And
the track's own source run measures 0 units off across every band, so the
centerline is faithful - it is the other runs that differ.

Three of the eight contain a map teleport, a single tick that moves the player
about 20,000 units. The windowed search looks 64 points ahead, roughly 4000
units, so it cannot follow that jump and everything it reports afterwards is
meaningless. A naive reading of those three gives a mean deviation of 9000
units, which is not a line at all - it is the measurement failing. The check now
says so instead of printing the number.

This does not explain the runs that end at 87.8%: at that point the track's own
source is on the line, the bot's completed runs are 71 units off it, and the
ones that fail are 505 to 562 - under the limit, and already 600 u/s slow since
78%. The corridor is killing runs that were lost long before. But it does mean
the corridor is roughly half the width of the human's own run-to-run spread, so
the bot is confined to reproducing one particular run rather than finding its
own line.

`setup_map.py --check` now reports all of this per run. It does not refuse to
train, because the map trains perfectly well with these files on disk - they
only matter when one is chosen to clone from, and then they matter a lot.

## Most of the machine was being thrown away

Eleven actors on twelve logical cores, all at AboveNormal priority, with the
learner left at Normal. A finished batch arrived every 5 seconds; the learner
took 14 to 15 seconds to consume one. Everything older than 12 generations is
dropped as stale, so **55% of every episode the machine produced was discarded
before it could be used.**

The learner was slow because it was losing every scheduling contest it entered.
Its OpenBLAS threads had no cores to run on, because eleven game servers at a
higher priority had all of them. Matching the learner's priority to the actors'
is a one-line change and it is most of the fix:

    config                                     steps/hour   gens/hour   wasted
    11 actors, batch 96, learner Normal            19.8M         235       55%
     6 actors, batch 64, learner AboveNormal       44.3M         715        0%

2.2x the learning throughput on half the machine, and nothing thrown away at
all - six actors produce very close to what the learner can take. Both windows
are single-configuration samples, 200 generations and 65 generations.

So the maximum power level is not the fastest setting - it is less than half
the speed of one that uses six actors instead of eleven. The actors were never the
bottleneck; they were already producing more than twice what could be used, and
adding more of them only took cores away from the one process that was.

Six actors also lands near the balance point: they produce about 37,000 episodes
an hour and the learner now consumes about 39,000, so almost nothing is wasted
and half the machine is free.

The remaining bottleneck is the PPO update itself - a pure numpy implementation,
about 11 seconds per 100,000 steps. That is now the thing to make faster, and
nothing about actor count or game settings will move it.

Worth noting what this looked like before it was measured: the run was set to
MAX because more actors obviously means more data, CPU usage sat at 55-60% and
looked like headroom, and every dashboard read healthy. The waste was only
visible in a line the learner printed and nothing aggregated - "skip
a3_batch_0047: policy gen 2835 is 13 behind" - once for every other batch, all
night.

## Measuring the finish rate off the disk understates it

`finishes.py` answers the headline question - how often does a run that started
at the beginning reach the end - by reading the batch files present in the out
directory. Three readings taken within twenty minutes gave 0.8%, 20.0% and
67.9%, on a policy that was improving but nothing like that fast.

The sample is biased, and biased downward. The learner deletes a batch as soon
as it has used one, so a batch sitting on disk is either still being written or
was passed over. The ones passed over are the ones dropped as stale - produced
by a policy twelve or more generations old. At 55% stale that was most of what
was ever there to read, and all of it came from older, worse policies.

The count now comes from the learner, which sees every episode exactly once,
and is written to `train_log.csv` as `runs_from_start`, `finished_from_start`
and `best_full_run_s`. Over gens 3018-3024, on 308 runs from the start of the
map: **69% finished**, fastest 39.66 s against the human's 39.05 s.

That is the fourth measurement error in this project with the same shape. The
eval that always used the longest prestrafe, the eval that ran at a different
frameskip than training, `best_progress` pinning at 1.0, and now this: in every
case the number was computed over a population that was not the one being asked
about, and in every case it looked entirely plausible. None of them produced an
error, a warning, or an implausible value - which is why each survived for days.

## The time bonus has never paid out once

Now that runs finish, the remaining objective is the clock: 39.66 s against the
human's 39.045 s. But the reward that was built for exactly this pays nothing:

    float under = g_fRefTime - secs;
    if (under > 0.0)
        terminal += g_fTimeBonus * under;      // g_fTimeBonus = 30.0

It is one-sided. It rewards beating the human and is silent about everything
else, and the bot has never beaten the human, so this term has contributed
exactly zero to every episode it has ever run. Two finishes, one at 39.7 s and
one at 45 s, receive an identical terminal reward of 10.0.

The only pressure toward speed is `g_fTimeCost`, charged at 0.08 per decision -
about 2.67 per second of episode. The bonus, if it were two-sided, would be 30
per second. So the gradient toward a faster run is roughly eleven times weaker
than intended, and has been the whole time.

Not changed yet, deliberately. Making it two-sided is an eleven-fold change to
the dominant terminal reward, and the obvious failure mode is a policy that
trades finishing for speed - which would undo the thing that just started
working. It wants a controlled comparison against a held checkpoint, not an
unattended overnight switch.

## Making the time bonus two-sided (gen 3625 onward)

Six hundred generations of evidence that the clock was not being optimised:

    gens        runs   finished   rate    fastest in the block
    3000-3099   3689    2628      71%     39.568s
    3100-3199   4430    3343      75%     39.568s
    3200-3299   4428    3468      78%     39.568s
    3300-3399   4415    3544      80%     39.478s
    3400-3499   4436    3663      83%     39.508s
    3500-3599   4437    3626      82%     39.478s

The finish rate climbed eleven points. The fastest run moved 0.09 s and then
stopped: 39.478 s at gen 3324, not beaten in the 300 generations after it. The
policy did exactly what it was asked - the reward paid for finishing and said
nothing whatsoever about the clock until the bot was already beating a human,
which it never was.

The change is not just removing the `if (under > 0.0)`. At 30 per second, a
finish one second slow would score 10 - 30 = -20 against a fall's -1, and the
policy would be right to conclude that falling is better than finishing slowly.
The original comment anticipated this ("charging for it twice would push toward
giving up") and drew the wrong conclusion from it - it dropped the downside
rather than bounding it.

So: base 50 rather than 10, two-sided at 30 a second, floored at 5.

    finish at 39.045 s (the reference)   50.0
    finish at 39.478 s (the best so far) 37.0
    finish at 39.700 s (typical)         30.3
    finish at 40.500 s                    6.3
    finish at 41.000 s or slower          5.0   (the floor)
    fell or stuck                        -1.0

Every finish outscores every fall no matter how slow, and the gradient stays
live from 39.045 s out to 40.545 s, which covers every time any run has ever
recorded. The plugin now prints this table at startup against its own loaded
reference time, because a reward you cannot read back is one you are guessing
at - and the first version of that very print was wrong (SourcePawn's
PrintToServer does not take the `%+.1f` flag; it printed the spec literally and
shifted every argument after it, reporting the floor as 30 and the time cost as
5).

Two other things changed with it. The KL anchor pointed at the behaviour clone -
gen 1, cloned from the one recorded run that spends 253 ticks outside the
deviation limit - and had been pulling an 82% policy back towards it for
thousands of generations. It now points at the gen 3625 policy: the term exists
to prevent collapse, not to preserve the clone. And `tools/compare.sh` reports
the run against the 225 settled generations before the change, with the watch
loop restoring the baseline checkpoint on its own if the finish rate drops under
65% across 60 generations.

## Learning the opening (gen 8858 onward)

By gen 8800 the run had converged and stopped improving: the fastest run was
39.178 s at gen 6217 and 2400 generations after it nothing had beaten it. Three
measurements say why, and none of them is the route.

**The policy is deterministic and the map is deterministic.** Forty greedy runs,
five per recorded opening:

    opening   runs   finished   time
    set 0        5       5      39.55s
    set 1        5       5      40.20s
    set 2        5       5      39.52s
    set 3        5       0      fell at 28.7%, every time
    set 4        5       5      39.52s
    set 5        5       5      39.71s
    set 6        5       5      39.64s
    set 7        5       5      39.62s

Within an opening all five runs agree tick for tick. Every bit of variation in
finish time comes from which of the eight recorded wind-ups was drawn - 0.68 s
between the best and the worst that finishes, and one that fails outright.

**The difference between a fast run and a slow one is spread evenly.** Comparing
the fastest quartile of completed runs against the slowest, tenth by tenth: 20 to
40 u/s everywhere, no section where the fast ones win. That is execution noise,
not route knowledge.

**The route constraint is not binding.** The deviation cost is 16.5 against an
episode return of 1196 - 1.4%. Completed runs sit 64 to 70 units from a 600 unit
corridor and never exceed 486. And the correlation between finish time and
distance from the centerline is +0.15: the faster runs are marginally *closer* to
the human's line, not further from it. Removing the route to let the policy
invent its own would not release anything - and the centerline is also the
progress reward and 21 of the 39 numbers the observation carries, so removing it
takes away the dense learning signal and most of what the bot can see.

So the opening is the thing nothing was optimising, and it is now the policy's.

`+csai_prelearn N` hands it the last N ticks of the wind-up. That phase needs a
different interpretation of the same 17 actions, because the ground is not the
air:

  * forward is held with the strafe key, so wishdir is the diagonal between them
    and sits 45 degrees off the view, not the 90 that sidemove alone gives
  * no jump - bunnyhopping through a wind-up throws away the ground speed it
    exists to build
  * the trim ladder spans 0 to 2 degrees, right at 4000 u/s and far too fine at
    280, so it is multiplied by 20 here and spans 70 to 130 degrees instead

No observation change was needed: speed alone separates the two phases, 0.28
against 4.0, and the checkpoint stays loadable.

Three things had to be kept honest. The wind-up makes no track progress by
design, so the no-progress cutoff and the stuck test both had to be suppressed
during it or the episode dies before the run starts. And the reported time
subtracts the wind-up ticks - left in, every time would have quietly grown by the
length of the wind-up and stopped being comparable to the human's 39.045 s.

Rolled out as a curriculum, because the states at the start of a wind-up are ones
the policy has never seen in 8800 generations. It gets 8 ticks first, near the
handover it already knows, and `tools/curriculum.sh` gives it 8 more each time
the finish rate comes back above 88%, up to 64. Below 55% it restores the gen
8858 checkpoint and stops.

One thing this cannot do much about: at gamma 0.997 and frameskip 2 the horizon
is 333 decisions, about ten seconds. The finish is roughly 1300 decisions after
the opening, so the time bonus reaches back to it at 2% strength. What actually
shapes the wind-up is the progress reward over the following ten seconds - a
reasonable proxy for a good entry, but it is a proxy.

### The baseline to beat, measured properly

Sampled and deterministic times are not comparable, and "fastest ever" is a tail
statistic that the entropy anneal shrinks on purpose. So the reference point for
everything after gen 8858 is a deterministic eval of that checkpoint, eight runs,
one per recorded opening:

    39.43  39.47  39.52  39.59  39.67  39.71  39.75  40.25      8/8 finished
    median 39.63 s, best 39.43 s, against the human's 39.045 s

`median_full_run_s` is now logged per generation alongside the best, because
judged on the best alone a policy that gets sharper and more consistent reads as
a regression.

## The checkpoint window meant something other than it said

`-StateLo` / `-StateHi` select which checkpoints a mixed start can spawn at, as a
fraction. Of what was never checked. The states file writes

    "frac": i / float(n - 1)

where `i` is the frame index - so it is how far through the run in TIME, and it
was being compared against numbers everyone read as a position on the track. The
run is much slower at the start than the end, so the two diverge by up to eleven
points:

    stored frac (time)   true track fraction
        0.6956                 0.6121
        0.7390                 0.6632
        0.7825                 0.7154
        0.8259                 0.7691

Every window ever passed therefore sat about seven points earlier on the map than
whoever typed it intended - including the one chosen specifically to straddle the
75.5-78% stretch where completed runs and failed ones diverge, which actually
covered 61-72% and only just reached it.

The plugin now resolves each checkpoint against the track at load and prints the
range, so the parameter means what it says. The defaults and the curriculum's
arguments were converted at the same time (0.68-0.79 becomes 0.61-0.72, the
explicit 0.73-0.83 becomes 0.66-0.77) so exactly the same three checkpoints are
selected as before: a labelling fix, not a behavioural one, deliberately kept
separate from the experiment running over it.

It has to be a full scan rather than the windowed search. These are isolated
points with no prior hint, and surf_demise teleports 2.5 s in - roughly 19,000
units, far past the 64 points the window looks ahead. Chained from the previous
checkpoint the last five read 1,120 to 14,616 units off a line they are sitting
exactly on, which is how the teleport first showed up at all.

## Learning the opening did not work

The wind-up experiment ran to PreLearn=40 and the answer is no. Every saved
checkpoint, evaluated the same way - eight deterministic runs, one per recorded
opening, at the setting it was trained with:

    checkpoint            wind-up ticks   finished   median
    gen 8858 (baseline)         0            8/8     39.63 s
    gen 9034                    8            7/8     39.67 s
    gen 9311                   16            7/8     39.67 s
    gen 9453                   24            8/8     40.21 s
    gen 9893                   32            8/8     42.22 s

Handing the policy the last 8 or 16 ticks was neutral and cost one opening.
Beyond that it got steadily worse. The best policy is still the one that replays
the recorded wind-up whole, so that is what has been restored.

The reason is the discount. At gamma 0.997 and frameskip 2 the horizon is 333
decisions, about ten seconds; the finish is roughly 1300 decisions after the
opening, so the time bonus reaches it at 2% strength. Nothing was asking the
wind-up to be fast. It was only being asked not to fall over, and it obliged.

### The reward stopped being able to see the problem

Worse, and the part worth remembering: the run degraded for eight hundred
generations and nothing objected.

    gens 9000-9099   median 39.80 s   finishing 91%
    gens 9200-9299   median 40.30 s   finishing 90%
    gens 9400-9499   median 41.84 s   finishing 87%
    gens 9700-9799   median 44.37 s   finishing 88%
    gens 9800-9899   median 44.47 s   finishing 90%

Four and a half seconds slower with the finish rate untouched. The two-sided time
bonus was a straight line, `50 - 30 * (seconds over the reference)`, floored at 5
so that a slow finish still beat a fall. Past about 40.5 s that floor is what
every finish gets - so the reward could not tell a 41 second run from a 44 second
one, and the policy drifted through the flat part freely. The cliff that the
two-sided bonus was built to remove had simply been moved somewhere else on the
scale, out of sight of the range anyone was checking.

It is now a ratio, `50 * (reference / time) ^ 8`, which is always positive so a
finish always outscores a fall without a floor doing the work, falls off fastest
where the runs actually are, and never flattens:

    39.05 s -> 50.0    40.5 s -> 37.3    44.4 s -> 17.9    60 s -> 1.6

`+csai_timebonus` is gone rather than left parsed and ignored.

And the curriculum could not see it either: it promoted five times on finish rate
alone while the runs got four seconds slower, because the finish rate was the
only thing it watched. It now measures median run time against a baseline taken
before the first step and rolls back if it drifts more than 0.4 s. Checked
against the run that went wrong, that would have stopped it around gen 9450
instead of gen 9893.
