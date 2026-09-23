# Experiment log

What was changed, and whether it helped. Failures are kept because most of the
useful information is in them.

Target: `surf_demise`, 66 tick. Reference run 39.045 s.

## Observation and action space

| change | result |
|---|---|
| Longer lookahead and horizon (gamma 0.99 -> 0.997) | no effect, 6.86% vs 6.67% |
| Surface probe: 5 collision rays plus surface normal | best early learning of any run |
| Delta actions (choose a change in aim, not an angle) | worse: p95 yaw step 161 deg |
| Tight action ladder, lower entropy | recovered, still limited |
| Action = strafe angle relative to velocity | the fix |

The centerline-only observation could follow a line but never recover from
leaving it: at one death the bot was 346 units to the side and 502 units below
the line. The probe reads real geometry (`n.up` 0.506-0.571, just under Source's
0.7 walkable threshold), which is the steering signal that was missing. It needs
engine collision, so it is logged rather than recomputed in Python.

Delta actions were the right diagnosis and the wrong implementation. The bot
snapped left and right because an absolute angle re-picked 33 times a second
makes consecutive choices independent. Integrating a held angle fixed that, but
the ladder rungs at +-115/+-160 gave an expected step of 36.8 deg per decision,
so the aim random-walked anyway.

The action space that worked came from measuring the reference run rather than
guessing: 98.1% of strafing frames hold the wish direction within 5 degrees of
perpendicular to velocity. The physics forces it - `addspeed = wishspd -
speed*cos(phi)` gives no acceleration at all below 89.5 degrees at 3614 u/s, and
near 180 degrees brakes up to 720 u/s in a single tick. So the policy picks that
one angle and everything else is derived from it.

## Behaviour cloning

Progress reward alone produced 19.79 strafe-key switches per second against the
reference run's 0.95. Jitter tracks a centerline more tightly than real technique
does, so that is what the reward asked for, and more training entrenched it.

Two cloning attempts failed before one worked, and both produced *better* offline
scores than the fix:

- an action space that could not express the label (0.972 side accuracy, latched)
- an observation carrying the previous action, which the clone read instead of the
  state: "repeat the last action" predicted the side 96.7% of the time

The number that exposed both was behavioural, not statistical: switches per
second. An imitation metric measured on the expert's own trajectory cannot see a
failure that only exists once the policy is driving.

Cloning is a one-off bootstrap, not a phase. PPO continues from the cloned weights
with a KL anchor.

## Reward

| term | effect |
|---|---|
| Progress along the centerline | the main signal |
| Deviation cost, quadratic | 1.4% of episode return; keeps runs near the line |
| Switch cost 0.40 per side change | reduced thrash; 0.15 was too weak |
| Trim cost 0.05 per aim change | closed most of the remaining view jitter |
| Time cost 0.08 per decision | weak but constant pressure on the clock |
| Finish bonus | see below |

The finish bonus went through three shapes:

1. **Flat 10.** Two runs that both finished scored the same whether they took
   39 s or 60 s. Nothing asked for speed.
2. **Upside-only time bonus**, `+30 per second under the reference`. It paid
   nothing until the bot was already beating the reference, which it never was, so
   it contributed exactly zero to every episode ever run. The finish rate climbed
   72% -> 82% over 600 generations while the fastest run did not move once.
3. **Two-sided, floored.** `50 - 30 * (seconds over)`, floored at 5 so a slow
   finish still beat a fall. Both halves were needed and together they made a
   cliff: past 40.5 s every finish paid the floor, so the reward could not tell a
   41 second run from a 44 second one. The policy drifted through the flat part -
   median run time 39.8 -> 44.5 s over 800 generations with the finish rate
   holding at 90%.
4. **Ratio**, `50 * (reference / time) ^ 8`. Always positive, so a finish always
   outscores a fall without a floor doing the work; falls off fastest where the
   runs are; never flattens.

A fall pays -1.0. Every finish must outscore it at any speed, or giving up
becomes the better move.

## The walls

Each of these looked like a learning problem and was not.

**60%.** A reward cliff: progress was rewarded, leaving the corridor was not
punished until it hit a hard limit, so 590 units off the route cost nothing and
600 ended the episode. A slope with no warning cannot be learned from. Fixed with
the quadratic deviation cost.

**72%.** Three confident diagnoses were wrong before measurement settled it. The
cause is in `addspeed = 30 - speed*cos(phi)`: cos turns negative past 90 degrees,
so addspeed *grows* with the angle and turn rate grows with it. Turning harder and
losing speed are the same act. At 4147 u/s, phi 89.75 gives 11 deg/s and phi 92
gives 161 deg/s. The bot had converged on 89.75 where roughly 300 deg/s was
needed - it looked down slightly and stopped steering. Forcing phi 92 cleared the
drop. The policy had collapsed to one side with p=0.0014 on the working action,
so it could never have sampled its way out; it had to be taught directly.

**88%.** Not a wall at that point at all. Completed and failed runs are identical
to within 5 u/s until 60%, and the entire gap opens between 75.5% and 78%, where
winners gain 540 u/s on a ramp and failures gain nothing because they enter it
about 100 units low. By 78% they are 600 u/s down; 87.8% is simply where a run
that slow runs out of height.

The pattern in all three: the band where runs end is the last place to look for
the cause, because by then every run in the failing population already shares the
same state.

## Measurement errors

Five, all with the same shape: a plausible number computed over the wrong
population, with no error, warning or implausible value to give it away.

| what was wrong | how it showed up |
|---|---|
| `replayqa` counted side switches as >90 deg view jumps | reported 0.00/s for every replay including the reference |
| eval hardcoded frameskip 2 while training ran at 6 | every eval replay drove the policy at three times its decision rate |
| `+csai_forceside -1` parsed as 0 (the parser rejects negatives) | five different forced trims returned identical results |
| eval always used the longest recorded prestrafe | reported 6.9% for a policy reaching 88% on five of eight openings |
| `best_progress` is share of track *gained* | pins at 1.0 the moment anything finishes, then never moves |
| finish rate sampled from batch files on disk | three readings in twenty minutes gave 0.8%, 20.0%, 67.9% |

The last one is the subtlest: the learner deletes a batch as soon as it uses one,
so what is left on disk is mostly batches passed over for being stale, produced
by older policies. The count now comes from the learner, which sees every episode
exactly once, as `runs_from_start`, `finished_from_start`, `best_full_run_s` and
`median_full_run_s`.

`best_full_run_s` alone is also misleading once entropy is annealed, because the
anneal shrinks the tail on purpose. The 39.178 s record at gen 6217 was a lucky
sample: with a deterministic policy the same opening repeats tick for tick, and
its real time was 39.52 s.

## Infrastructure failures

These cost more time than any of the learning problems.

**A missing pair of braces.** SourcePawn binds only the first statement to a
braceless `if`, and a comment between the two hides it:

    if (g_hBatchFile == null)
        PrintToServer(...);
        // why this matters
        g_bBatchBroken = true;

The flag was set on every batch, which suppressed the `.done` marker the learner
scans for. Eleven game servers ran at 100% of a core for 28 minutes writing files
nothing could see. Nothing crashed, so every liveness check passed.

**The checkpoint was written non-atomically.** `np.savez` straight over the only
record of thousands of generations, while the daemon force-kills the learner on
every power change. Now written to a temp file and moved into place, keeping the
previous copy.

**A byte-order mark in `power.txt`.** `Set-Content -Encoding utf8` prepends one in
PowerShell 5.1. The level read as `\ufeffmedium`, and training silently ran four
servers instead of six. The same lesson was already written in a comment twelve
lines above the offending call.

**All eleven actors shared one RNG seed**, so any two on the same policy
generation produced byte-identical episodes. Each actor now seeds its own stream.

**The learner was starved by its own workers.** Actors ran at AboveNormal while
the learner ran at Normal on a saturated machine, so it lost every scheduling
contest.

## Throughput

Game servers were never the bottleneck. At 11 they produce more than twice what
the learner can read, so most is discarded as stale, and they take the processor
time needed to read the rest.

| | consumed per hour | discarded |
|---|---|---|
| 11 servers, learner at Normal | 19.8 million steps | 55% |
| 6 servers, learner at AboveNormal | 44.3 million steps | none |

Twice the learning on half the machine. The remaining bottleneck is the PPO
update itself, about 8 seconds per 100k steps in numpy.

## Data quality

**The recorded runs disagree with the line the reward enforces.** An episode is
killed past 600 units from the centerline, and the centerline comes from exactly
one recorded run. Four of surf_demise's eight spend one to three seconds outside
that corridor, including the one cloning read. `setup_map.py --check` now reports
this per run.

**The centerline is drawn through teleports** unless the arc-length gap is
skipped. On surf_dune 44% of the track ran through void, and one teleport tick
paid about 23,000 units of reward.

**Recording tickrate was never validated.** surf_dune was recorded at 100 tick
against a 66.67 tick server. Recordings are now re-timed.

**Checkpoint fractions were time, not distance.** The states file writes
`frac = i / (n-1)` over frame index, so `-StateLo`/`-StateHi` selected a band
about seven points earlier on the map than intended. Resolved against the track at
load; the defaults were converted so the same checkpoints are still selected.

## Learning the opening

The wind-up before the timer starts was the only part of a run nothing was
optimising. With a deterministic policy the map is run identically every time -
five runs of one opening agree tick for tick - and the whole spread in finish time
came from which recorded opening was drawn: 39.52 s for the best, 40.20 s for the
worst, and one that failed outright.

`+csai_prelearn N` hands the policy the last N ticks of the wind-up. That phase
needs a different reading of the same actions: forward is held with the strafe
key so wishdir is 45 degrees off the view rather than 90, jump must not be held,
and the trim ladder is multiplied by 20 because 0-2 degrees is right at 4000 u/s
and far too fine at 280.

It did not work.

| checkpoint | wind-up ticks | finished | median |
|---|---|---|---|
| gen 8858 baseline | 0 | 8/8 | 39.63 s |
| gen 9034 | 8 | 7/8 | 39.67 s |
| gen 9311 | 16 | 7/8 | 39.67 s |
| gen 9453 | 24 | 8/8 | 40.21 s |
| gen 9893 | 32 | 8/8 | 42.22 s |

Neutral at 8-16 ticks, worse after. The reason is the discount: at gamma 0.997 and
frameskip 2 the horizon is 333 decisions, about ten seconds, and the finish is
roughly 1300 decisions after the opening. The time bonus reached it at 2%
strength. Nothing asked the opening to be fast, only not to fall over.

The curriculum that drove it promoted five times on finish rate alone while the
median run got four seconds slower. The experiment is closed and the curriculum
script removed.

## Heavier time cost, lighter deviation cost

Two attempts to buy back the 0.25 s gap by pricing time harder and the route
looser. Deterministic eval, eight runs from the start:

| checkpoint | finished | best | median |
|---|---|---|---|
| gen 8858 | 8/8 | 39.46 s | 39.61 s |
| gen 13226 | 8/8 | 39.32 s | 39.44 s |
| gen 14349 | 6/8 | 39.29 s | 39.41 s |
| gen 14754, time cost 0.40, deviation cost 0.05 | 8/8 | 39.43 s | 39.55 s |

Both were worse. Settings are back to time cost 0.08 and deviation cost 0.5, and
training resumed from gen 13226, the best checkpoint that finishes every run.

## Reward timing

The plugin reads the bot's position before each tick's movement, so a tick's
progress is the result of the previous tick's input. It was being credited after
the decision boundary, which handed every decision one tick of its predecessor's
progress. Progress and deviation are now charged before the boundary, and finish,
fall and stuck checks run on the state before the next input is applied. Reported
times are one tick (0.015 s) shorter as a result; the table above was measured
this way.

## The bot's own wind-up

The wind-up slot used to be scored on horizontal speed at first ramp contact. The
contact test (vertical acceleration no longer equal to gravity) misses steep
ramps, so the policy learned to slide down a ramp somewhere else: about 1000 u/s,
400 units off the line, 250 ticks in. The recording reaches the ramp at 466 u/s.
And the main run never used it; every run still opened with a recording.

The recordings settle when the clock starts: shavit starts it on the last tick on
the ground before the jump (tick 67, jump on 68), well inside the start zone.
Ground time is free.

Now the wind-up drives from standing until it chooses to jump or leaves the
ground or the start zone, and the main policy takes over on that tick with the
clock starting. The wind-up slot scores each wind-up on the finish time of the
whole run the main policy then flies, 20 points per second against the
reference. The main slot opens three runs in four with the learned wind-up, so it
learns to fly from the bot's own takeoffs.

First evaluation, greedy, eight spawn points, before any co-training:

| wind-up | ground ticks | takeoff | run |
|---|---|---|---|
| learned | 54 | 282 u/s | 39.34 s |
| learned | 56 | 279 u/s | 39.31 s |
| learned | 36 | 269 u/s | fell at 7% |
| learned | 36 | 270 u/s | fell at 7% |

A wind-up that holds the ground long enough already matches the best run on a
recorded one. After 20 generations of co-training, 7 of 8 wind-up runs finish;
the short 34 to 40 tick wind-ups are the slow and failing ones, which is what the
score now teaches away from.

## Where it stands

| | bot | reference |
|---|---|---|
| runs that finish | 8 of 8 | - |
| best time | 39.32 s | 39.04 s |
| median time | 39.44 s | - |
| strafe key changes | 0.76 per second | 1.05 per second |
| time at a usable strafe angle | 98.1% | 97.8% |
| median speed | 3603 units/s | 3589 units/s |

Technique matches the reference run. The remaining gap is under three tenths of a
second, spread evenly across the map rather than lost at any one point.

## Standing lessons

- Measure the thing being changed, in the state it will run in. Every wrong
  diagnosis here came from reasoning about a number computed somewhere else.
- A metric that cannot get worse is not measuring anything. Check what the number
  does at both ends of its range before trusting it.
- Liveness is not progress. Processes that are alive, busy and logging can be
  producing nothing usable.
- A parameter that no longer does anything should be deleted, not left parsed and
  ignored.
