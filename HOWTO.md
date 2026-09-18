# Running it

Run `CsAI.bat`. It opens a single window.

```
  training: running   power: high   game servers: 6
  generation 10049   finishing 94% of runs   median run 39.52s   the time to beat is 39.05s

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

Training runs six game servers, a learner and a supervisor, each with its own
console. All of them start hidden and the panel hides any that appear later, so
the panel is the only window on screen. Selecting a row and pressing **show
window**, or double-clicking the row, brings that console up; the same again
hides it.

The lower half tails the logs. The drop-down selects which.

Closing the panel does not stop training. It continues in the background until
**stop training** is pressed.

## Power levels

How much of the machine training may use. Changeable at any time, including
mid-run. It takes effect within about 15 seconds and nothing trained is lost: the
learner checkpoints every generation and resumes from it.

| level | servers | use |
|---|---:|---|
| idle | 1 | machine in use for gaming or video |
| low | 2 | machine in use for work |
| medium | 4 | background |
| **high** | **6** | **default** |
| max | 11 | not recommended |

Raw simulation speed does keep rising with server count. Measured on a 6-core
Ryzen 7500F, total game ticks per second:

| servers | ticks/s |
|---:|---:|
| 6 | 15276 |
| 10 | 21586 |
| 12 | 22915 |
| 14 | 22066 |

That is not the number that matters. The learner can only read so much of it, and
anything more than 12 generations old is discarded as stale. Measured on 12
logical cores:

| servers | practice consumed per hour | discarded |
|---:|---|---|
| 11 | 19.8 million steps | 55% |
| 6 | 44.3 million steps | none |

At 11 servers the extra copies take the processor time the learner needs to read
what the first six already produced. Six is the point where production and
consumption match.

The graphics card does nothing here and cannot. The server has no renderer and
loads no textures, and the physics is single-threaded C++ with no GPU path.

## Watching replays

**replay viewer**, or `Viewer.bat`, opens the viewer at `http://127.0.0.1:3000`.
The newest bot run is at the top of Recent Times; select it, then **View
Replay**. The first load of a map takes a few seconds while its geometry is
cached.

Training scores itself every few minutes and saves the furthest of those runs, so
a recent replay is always available.

## How a run is put together

Each episode is one continuous attempt at the whole map.

1. **Prestrafe** - the recorded pre-timer inputs are replayed through real
   physics. The policy cannot produce this: its action space is the air-strafe
   angle with no forward movement, and that restriction is what made it
   controllable. Letting the policy drive the wind-up was tried and measured
   slower.
2. **Handover** - the policy takes over at about 285 u/s. Nothing before this
   point is trained on; the progress baseline starts here.
3. **The run** - the bot surfs until it finishes, falls, or stops advancing.

Jump is held throughout. With `sv_enablebunnyhopping 1` that means clipping a
ramp auto-hops instead of sticking and dumping momentum.

### Continuous vs segmented

Every episode starts at the beginning. The alternative is starting from the 24
checkpoints spread along the route, which is easier to learn from but optimises
"advance from anywhere" rather than "complete the map". A share of episodes do
start mid-map, set by `-StateMix`.

To change it, set `'+csai_states', '0'` in `tools/daemon.ps1` (1 = always the
start, 0 = sample all checkpoints).

The end-to-end numbers are `runs_from_start`, `finished_from_start` and
`median_full_run_s` in `data/train_log.csv`. Progress percentages in older entries
of `docs/experiments.md` measured progress from a random checkpoint and are not
comparable.

## Using a different map

One command turns a recorded run into everything needed:

```bash
python tools/setup_map.py surf_dune
```

It locates the timer's replay for that map, derives the reference line, the
restart checkpoints, the prestrafe and the run to clone from, then validates the
result. Nothing about the bot is tied to a particular map; only the trained
weights are, and those start from scratch.

```bash
python tools/setup_map.py surf_dune --check
```

Then train:

```bash
.\tools\daemon.ps1 -Power high -Map surf_dune
```

On maps with stages, failed attempts that reset to a stage start are detected and
dropped, and the teleport between stages is not counted as distance travelled.
Counting it would pay an enormous one-tick reward for being teleported.

## Recording runs

More recordings are the most useful addition to the project.

The bot learns technique by copying recorded runs. surf_demise has eight. Runs
that are slightly off the usual line are the ones it has least of, and they are
what teach it to recover.

### Recording

Recording is automatic and always on. Every completed run is saved, fast or slow,
and confirmed in chat:

```
[CsAI] saved run 2 - 41.83s, 2 runs now available
```

Runs are numbered and never overwrite each other, up to 32.

A timer cannot be used for this. It keeps one replay per map and replaces it only
on a faster time, so slower runs are discarded, and slower runs are the useful
ones here.

### Before playing

Stop training, or set power to idle, from the panel. Six copies of the server
otherwise leave the game unplayable.

### What kind of runs help

Messy runs are worth more than clean ones. The bot already has a good line. What
it lacks is what to do when it is too low, too fast, or drifting wide. A run that
clips a ramp, wobbles and recovers teaches the recovery; another clean run does
not.

A useful mix is a few normal runs and a few that deliberately go wide, come in
low, or scrape through a section and save it. Five or six is enough to start.

A recording only helps if it stays near the route the bot is scored against.
`python tools/setup_map.py <map> --check` lists any that stray far enough to count
as a fall, which makes them poor sources to clone technique from.

### Commands

In chat:

| command | effect |
|---|---|
| `!csai_runs` | how many runs are saved, and whether recording is active |
| `!csai_save` | save the current run without finishing it |
| `!csai_drop` | discard the current recording and restart it |

`!csai_save` covers partial runs: to supply a specific hard section rather than
the whole map, run into that section and save there.

### Retraining on new recordings

```bash
python tools/bc.py --epochs 400 --print
```

Stop training first. `bc.py` republishes the policy as generation 1, which
discards training done since the last copy. Restart training afterwards.

Capture replays every recorded run through real physics in one pass, so five runs
give about 6000 examples rather than 1200.

## Reports

Written automatically to `reports/`:

- `latest.md` - current
- `report_YYYYMMDD_HHMM.md` - a snapshot every 5 minutes

**report** writes and opens the latest. **open folder** gives access to the rest.

Each report carries the end-to-end finish rate and times, progress sparklines, the
last 12 generations, and a control-quality table. `phi in window` is the share of
frames where aim is in the only range where air acceleration does anything at surf
speed; the reference run sits at 97.8%. It distinguishes surfing from flailing,
which finish time alone does not.

## Logs

- `logs/daemon.log` - starts, stops, power changes, crashes, restarts. The panel
  tails it live.
- `logs/learner.log` and `logs/learner.err.log` - the learner's own output.
- `data/train_log.csv` - one row per generation.
- `docs/experiments.md` - each configuration change and whether it worked.

## If something looks wrong

The daemon restarts the learner or any actor that dies, and logs it. It also
restarts everything if no generation completes for long enough, and reports when
batch files are piling up unread, which means actors are producing data the
learner cannot see.

`bash tools/watch.sh` prints a one-line health check and exits non-zero on a
problem.

Training and the viewer are independent; stopping one does not affect the other.
