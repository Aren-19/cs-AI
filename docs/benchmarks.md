# Measured throughput

Hardware: Ryzen 5 7500F (6C/12T), 15.6 GB RAM, RTX 5060 Ti.
Server: `srcds_win64`, CS:S, **66 tick** (engine default 66.67, no `-tickrate`),
`+servercfgfile server_66.cfg`, map `surf_demise`, full plugin set loaded
(shavit + QoL), single instance, no client attached.

Measured by `tools/bench.ps1`: the plugin records `GetEngineTime()` (real wall
time) across a fixed number of simulated ticks.

## host_timescale sweep at 66 tick

| host_timescale | ticks/s | speedup | peak h-speed |
|---:|---:|---:|---:|
| 1 | 66 | 1.00× | 354 |
| 40 | 2710 | 40.66× | 354 |
| 80 | 5136 | 77.04× | 354 |
| 150 | **5460** | **81.90×** | 354 |

**The CPU ceiling is ~5400 ticks/second**, and that number is the same at 66 and
100 tick — the cost is per tick, not per second of game time. So a 66-tick server
converts that ceiling into **~82× realtime**, against ~53× at 100 tick. Running at
66 buys roughly 1.5× more simulated game time per second of compute.

Timescale 80 sits at the knee; 150 gains only 6% more.

## Physics are invariant under timescale

Every run above produced an **identical** trajectory: start 285, end 44, peak
354 u/s, at every timescale from 1× to 150×. Speeding up the server does not
perturb the simulation — it only runs the same fixed-tick physics faster. This is
a precondition for the whole approach, and it holds.

## What this means for training

Roughly 4–5 concurrent `srcds` on 6 physical cores, leaving headroom for the
learner:

- aggregate ≈ **22,000–27,000 ticks/s** ≈ 330–410× realtime
- a full `surf_demise` attempt is 2607 ticks, so ≈ **8–10 complete map attempts
  per second**, ≈ 700k–900k attempts/day
- ≈ 2.3 billion ticks/day; at frame-skip 4, ≈ **580M agent steps/day**

For reference, Linesight beat Trackmania WRs on tens of millions of steps.
Throughput is not the binding constraint on this machine.

### Caveats

Multi-instance scaling is **unmeasured**. 5 instances will not be 5 × 5460 —
memory bandwidth and shared L3 take a cut. Measure before relying on it.

The benchmarked bot also spends most of its run on flat ground rather than riding
ramps, so per-tick collision work is below what real surfing costs. Re-measure
once a policy actually completes sections.

## Air-strafe controller: validated against theory

With the bot started from the replay's state 0 and left to air-strafe, observed
horizontal speed matched the theoretical optimum `sqrt(v0^2 + N*900)` **exactly**
across all 48 airborne ticks:

| tick | observed | theory | delta |
|---:|---:|---:|---:|
| 0 | 286.91 | 286.91 | +0.00 |
| 12 | 305.15 | 305.15 | −0.00 |
| 24 | 322.36 | 322.36 | +0.00 |
| 36 | 338.70 | 338.70 | −0.00 |
| 47 | 353.01 | 353.01 | +0.00 |

**100.00% of optimum.** The `900` is `wishspd^2` with `wishspd = 30` (AIR_SPEED_CAP):
in the uncapped regime the optimal angle is 90 degrees and each tick adds exactly
`wishspd` perpendicular, so `|v|` grows as `sqrt(v0^2 + N*wishspd^2)`.

This validates the corrected clamp (`cos θ* = clamp((wishspd−k)/speed, 0, 1)`, floor
at 0 not −1), the yaw/sidemove decoding, and the `OnPlayerRunCmd` injection path
all at once.

It does **not** mean the bot can surf. It accelerates optimally but cannot steer —
it circles, lands on the start platform, and friction decays it to 44 u/s.
Steering along the route is what the learned policy has to supply.

## Gotchas found the hard way

1. **`host_timescale` needs `sv_cheats 1`, and shavit turns it back off.**
   `shavit-core` installs a change hook on `sv_cheats` that forces it back to 0
   (`shavit_core_disable_sv_cheats`, default 1, set in
   `cfg/sourcemod/plugin.shavit-core.cfg`). Both a direct `ConVar.SetInt` and a
   console `sv_cheats 1` get silently reverted. Issue
   `shavit_core_disable_sv_cheats 0` first. Symptom when missed: timescale
   reads back correctly but speedup stays exactly 1.00×.

2. **CS:S round management freezes the bot.** A bot that reports
   `MOVETYPE_WALK`, no `FL_FROZEN`, and simply never moves is stuck in
   freezetime / round state. `mp_freezetime 0`, `mp_ignore_round_win_conditions 1`,
   `mp_restartgame 1` — and the restart must happen **well before** the run, or it
   respawns the bot mid-episode and teleports it out of the trajectory.

3. **`sv_hibernate_when_empty` does not exist in CS:S.** It is not the reason an
   empty server looks idle.

4. **Command-line cvars for a plugin's own ConVars do not work.** The engine
   reports `Unknown command "csai_bench_ticks"` at startup and does *not* queue
   the value for when the cvar later registers. Read the command line inside the
   plugin instead (`GetCommandLineParamInt("+csai_bench_ticks", …)`).

5. **`-console` with stdin redirected to `/dev/null` kills srcds.** It hits EOF
   and shuts down, usually before the plugin's timers fire. It needs a real
   console of its own.

6. **`TeleportEntity` respects world geometry.** Lifting a bot to a point inside
   the ceiling appears to succeed (the immediate origin read-back matches) and
   then the engine shoves it back to the floor within a few ticks. Starting from
   a replay state avoids the problem entirely — that position is known reachable.

7. **shavit rotates the map out from under a training run.** `shavit-mapchooser`
   and `shavit-timelimit` will vote/cycle to the next map mid-run. The actor then
   has no track and no start states for the new map, stops producing batches, and
   idles silently while the learner waits out its timeout - the run just stops,
   with no error. `SetupRound()` now unloads both plugins (runtime-only, reverts
   on restart), and `OnMapStart` aborts loudly if a map change happens anyway.
   Symptom to recognise: `no states file for <some other map>` in the server log.

8. **`Logarithm()` in SourcePawn defaults to base 10, not e.** Using it for a
   policy log-probability puts log10 values into PPO's importance ratio. Symptom:
   entropy pinned at `log(nactions)` while KL is a *constant* non-zero value and
   clipfrac is 1.000. For 25 actions the constant was +1.82 = `ln(1/25) - log10(1/25)`.
   Pass the base: `Logarithm(p, 2.718281828459045)`.

9. **Don't pipe `spcomp` into another command in a shell `&&` chain.** The exit
   status becomes the *pipe's* last stage, so a failed compile still "succeeds"
   and a stale binary gets deployed. `tools/build.ps1` checks `$LASTEXITCODE`.

10. **srcds cannot be hidden by the usual means.** `srcds_win64.exe` is a GUI
   program that opens its own console, so a hidden window style or
   `CreateNoWindow` does nothing. `tools/hidden.ps1` starts it on a separate
   desktop that is never shown, and gives each server its own log with
   `+con_logfile` (the `cstrike/logs` folder has to exist first).
