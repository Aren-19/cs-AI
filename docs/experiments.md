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
extra runs - shavit keeps one replay per map and only replaces it when you beat
your time, so slower runs are discarded. Seven new runs were recorded, times
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
