# Running it

Double-click **`CsAI.bat`**. That's the whole interface.

```
  CsAI - Counter-Strike: Source surf AI
  ---------------------------------------------------------------
  training : RUNNING   power: HIGH
  progress : gen 149   gain 7.70%   best 18.14%   entropy 1.58
  viewer   : http://127.0.0.1:3000
  target   : beat 39.10 s on surf_demise (100% of track)
  ---------------------------------------------------------------

   [1] Start training          [2] Stop training
   [3] Change power level      [4] Watch replays (viewer)
   [5] Show report             [6] Make a replay now
   [7] Open reports folder     [8] Live log
   [0] Exit  (training keeps running)
```

**Closing the panel does not stop training.** It runs in the background until you
choose [2], so you can start it and walk away for hours.

## Power levels

How much of the machine training is allowed to use. Change it at any time,
including mid-run — it takes effect within ~15 seconds and **nothing trained is
lost**, because the learner checkpoints every generation and resumes from it.

| level | timescale | priority | when to use |
|---|---:|---|---|
| idle | 5× | Idle | gaming or watching video |
| low | 20× | BelowNormal | you're working on the PC |
| medium | 50× | Normal | background |
| **high** | **80×** | Normal | **default** |
| max | 150× | AboveNormal | you're away |

`high` is the measured knee of the throughput curve (5136 ticks/s); `max` buys
only ~6% more for noticeably more disruption. Use `max` overnight, `low` or `idle`
while you're at the machine.

## Watching replays

[4] opens the local replay viewer at `http://127.0.0.1:3000` — or double-click
**`Viewer.bat`**. The newest bot run is at the top of Recent Times; click it, then
**View Replay**. First load of a map takes a few seconds while its geometry is
cached, then it's instant.

The daemon evaluates the policy every 40 generations, so there is always a recent
replay to watch without doing anything.

[6] makes one on demand from the current policy.

## How a run is put together

Each episode is **one continuous attempt at the whole map**:

1. **Prestrafe** — your recorded pre-timer inputs (67 ticks, W+strafe while
   turning, 0 -> 286 u/s) are replayed through real physics. The policy cannot
   produce this: its action space is the air-strafe angle with no forward
   movement, and that restriction is what made it controllable at all.
2. **Handover** — the policy takes over at 287 u/s, against your 285. Nothing
   before this point is trained on; the episode's progress baseline starts here.
3. **The run** — the bot surfs until it finishes, falls, or stops advancing.

Jump is held throughout, as you do — with `sv_enablebunnyhopping 1` that means
clipping a ramp auto-hops instead of sticking and dumping all momentum.

### Continuous vs segmented

Every episode starts at the beginning. The alternative is starting from the 24
replay checkpoints spread along the route, which is easier to learn from (the bot
gets signal on the last third without solving the first two) but optimises
"advance from anywhere" rather than "complete the map".

To switch, set `'+csai_states', '0'` in `tools/daemon.ps1` (1 = always the start,
0 = sample all checkpoints).

**Reading the numbers:** with continuous runs, `gain %` is progress along the
*whole map*. It is not comparable to the checkpoint-start runs in
`docs/experiments.md`, which measured progress from a random checkpoint. The
honest end-to-end number is the eval line in the report.

## Adding more of your own runs

This is the highest-value thing you can do for the bot right now.

The policy learns technique by cloning your inputs, and it currently has **one
run** to learn from — 1213 decisions, all on a single line. It has never seen a
state slightly off that line, which is exactly why it surfs cleanly and then
fails to recover: it rides one ramp beautifully and falls where your line would
have switched.

More recorded runs fix this directly. They do not need to be perfect or fast —
**varied** is worth more than clean here, because the value is in covering states
your best run never visits. A run that wobbles and recovers is *especially*
useful.

### How to add one

1. Run the map normally with your timer so it records a `.replay`.
2. Convert it to a demo file, numbering it after the ones already there:

```bash
python tools/replay.py "path	o\your.replay" --demo "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrikeddons\sourcemod\data\csai\surf_demise_demo_2.txt"
```

The naming is `surf_demise_demo.txt`, then `_2`, `_3`, ... up to 32. They must be
contiguous — the first gap ends the set.

3. Recapture and retrain:

```bash
python tools/bc.py --epochs 400 --print
```

(Capture runs automatically as part of training startup; to force it now, launch
srcds with `+csai_democapture 1`.)

Capture replays every run through real physics in one pass and appends them all,
so 5 runs gives ~6000 samples instead of 1213. Verified working at 3 runs.

**Note:** `bc.py` republishes `weights.txt` as generation 1, which discards RL
progress made since cloning. Stop training first, add the runs, retrain, then
start training again.

## Reports

Written automatically to `reports/`:

- `latest.md` — always current
- `report_YYYYMMDD_HHMM.md` — a snapshot every 5 minutes, so you can see how
  things moved while you were away

[5] shows the latest in the window. [7] opens the folder.

Each report carries progress sparklines, the last 12 generations, and a **control
quality** table — `phi in window` is the share of frames where the bot's aim is in
the only range where air acceleration does anything at surf speed. A human sits
at 98.1%. That number says whether the bot is surfing or flailing, which the
finish time alone does not.

## Logs

- `logs/daemon.log` — starts, stops, power changes, crashes and restarts. [8]
  tails it live.
- `data/train_log.csv` — one row per generation, the raw record.
- `docs/experiments.md` — what each configuration change was and whether it worked.

## If something looks wrong

The daemon restarts the learner or the actor automatically if either dies, and
logs it. If training seems stuck, check [8] for a restart loop.

Training and the viewer are independent — stopping one doesn't affect the other.
