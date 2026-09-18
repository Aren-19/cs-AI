# Running it

Double-click **`CsAI.bat`**. That's the whole interface: one window.

```
  training: running   power: high   game servers: 6
  generation 8870   finishing 89% of runs   median run 39.81s   the time to beat is 39.05s

   what        pid     cpu    memory   window   doing
   daemon      15132   1%     93 MB    none     supervisor
   learner     18108   78%    109 MB   hidden   ppo update
   actor 0     10156   96%    324 MB   hidden   collecting episodes
   actor 1     18556   95%    323 MB   hidden   collecting episodes
   ...

   [start training] [stop training]   power: high
   [show window]    [hide window]     [hide them all]
   [replay viewer]  [report]          [open folder]
```

Training runs six game servers, a learner and a supervisor, and each of those
opens its own console. They all start hidden and the panel puts away any that
turn up, so the only window on screen is this one. Pick a row and press **show
window** - or just double-click it - to bring that one console up when you want
to read it, and again to put it away.

The bottom half tails the logs. The drop-down picks which one.

**Closing the panel does not stop training.** It runs in the background until you
press stop training, so you can start it and walk away for hours.

## Power levels

How much of the machine training is allowed to use. Change it at any time,
including mid-run - it takes effect within ~15 seconds and **nothing trained is
lost**, because the learner saves every generation and picks up where it left off.

| level | servers | when to use |
|---|---:|---|
| idle | 1 | gaming or watching video |
| low | 2 | you are working on the PC |
| medium | 4 | background |
| **high** | **6** | **default, about half the machine** |
| max | 11 | you are away |

The thing that makes training faster is **how many copies of the server run at
once**, not how fast each one is told to go. One server only uses one core, and
once several are running each manages about 23-32x real speed - nowhere near the
80x it is aiming for. Telling it to go faster than it can does nothing.

Measured on a 6-core Ryzen 7500F, total game ticks simulated per second:

| servers | ticks/s |
|---:|---:|
| 6 | 15276 |
| 10 | 21586 |
| **12** | **22915** |
| 14 | 22066 |

It keeps improving past 6 servers even though the CPU has 6 cores, because the
server spends a lot of time waiting on memory and the spare thread on each core
can use that gap. 12 is the best it gets; 14 is worse. `max` uses 11 and leaves
room for the trainer itself.

**The graphics card does nothing here and cannot.** The server has no renderer
and loads no textures at all - about 200 MB each, all of it map geometry and
game logic. The physics is plain C++ on one core and there is no version of it
that runs on a GPU. Spare VRAM cannot help.

## Watching replays

**replay viewer** opens it at `http://127.0.0.1:3000` — or double-click
**`Viewer.bat`**. The newest bot run is at the top of Recent Times; click it, then
**View Replay**. First load of a map takes a few seconds while its geometry is
cached, then it's instant.

Training scores itself every few minutes and saves the best of those runs, so
there is always a recent replay to watch without doing anything.

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

## Using a different map

One command turns a recorded run into everything the bot needs:

```bash
python tools/setup_map.py surf_dune
```

It finds your timer's replay for that map, derives the reference line, the
restart checkpoints, the prestrafe and the run to clone from, then checks the
result and tells you if anything is wrong. Nothing about the bot is tied to a
particular map - only the trained weights are, and those start from scratch.

```bash
python tools/setup_map.py surf_dune --check    # just validate what is there
```

Then train it:

```bash
.	ools\daemon.ps1 -Power high -Map surf_dune
```

A note on maps with stages: if your run contains failed attempts that reset you
to a stage start, those are detected and dropped, and the teleport between
stages is not counted as distance travelled. Getting that wrong would pay the
bot an enormous one-tick reward for being teleported.

## Recording your own runs

This is the most useful thing you can do for the bot right now.

It learns technique by copying you, and it has exactly one run to copy from. It
has never seen what happens when you are slightly off your usual line, which is
why it surfs a ramp nicely and then has no idea how to save itself.

### Just play

Recording is automatic and always on. Play the map normally and **every run you
finish is saved**, fast or slow. You will see a chat message when it happens:

```
[CsAI] saved run 2 - 41.83s, 2 runs now available
```

Runs are numbered and never overwrite each other, up to 32 of them.

The timer cannot do this for you, which is why this exists. It keeps one replay
per map and only replaces it when you beat your time, so a slower run is thrown
away. Slower runs are exactly the ones worth keeping here.

### Before you play

Stop training first, or set the power to idle, both from the panel. Six copies of
the server are using your CPU and the game will feel awful otherwise.

### What kind of runs help

Not your best ones. **Messy runs are worth more than clean ones.**

The bot already knows what a good line looks like. What it does not know is what
to do when it is too low, too fast, or drifting wide, because you have never
shown it. A run where you clip a ramp, wobble and recover teaches it the
recovery. A perfect run teaches it nothing it does not already have.

So: do a few normal runs, and a few where you deliberately take it wide, come in
low, or scrape through a section badly and save it. Five or six runs is plenty to
start with.

### Commands

Type these in chat:

| command | what it does |
|---|---|
| `!csai_runs` | how many runs are saved, and whether it is recording now |
| `!csai_save` | save what you have done so far without finishing |
| `!csai_drop` | throw away the current recording and start again |

`!csai_save` is for partial runs. If you want to give it a specific hard section
and not the whole map, run into that section and save there.

### Then retrain

Once the runs are in:

```bash
python tools/bc.py --epochs 400 --print
```

Stop training before you do this. `bc.py` republishes the bot's brain as
generation 1, which throws away the training done since the last copy. Start
training again afterwards.

Capture replays every recorded run through real physics in one pass, so five runs
gives about 6000 examples instead of 1200.

## Reports

Written automatically to `reports/`:

- `latest.md` — always current
- `report_YYYYMMDD_HHMM.md` — a snapshot every 5 minutes, so you can see how
  things moved while you were away

**report** writes the latest one and opens it. **open folder** gets you the rest.

Each report carries progress sparklines, the last 12 generations, and a **control
quality** table — `phi in window` is the share of frames where the bot's aim is in
the only range where air acceleration does anything at surf speed. A human sits
at 98.1%. That number says whether the bot is surfing or flailing, which the
finish time alone does not.

## Logs

- `logs/daemon.log` — starts, stops, power changes, crashes and restarts. The panel
  tails it live.
- `data/train_log.csv` — one row per generation, the raw record.
- `docs/experiments.md` — what each configuration change was and whether it worked.

## If something looks wrong

The daemon restarts the learner or the actor automatically if either dies, and
logs it. If training seems stuck, read `daemon.log` in the panel for a restart loop.

Training and the viewer are independent — stopping one doesn't affect the other.
