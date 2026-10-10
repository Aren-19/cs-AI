# Running it

Run `CsAI.bat`. The first time, it builds `CsAI.exe` next to it; after that
either one opens the panel. The panel is the only window. Every game server,
learner and supervisor runs on a separate, invisible desktop, so nothing else
ever appears on screen.

```
  main: running, 6 server(s)   power high   gen 14754   finishing 100%   median 39.48s
  windup: running, 2 server(s)   power low   gen 3120   mean score 4.1   best 8/8 at 39.30s

   what         slot     pid     cpu    memory   doing
   supervisor   main     15132   1%     93 MB    keeps the slot running
   learner      main     18108   78%    109 MB   updating the policy
   server 0     main     10156   96%    324 MB   playing episodes
   ...

   slot: [all]   [start] [stop]   power: [high]
   [replay viewer] [report] [open folder]
```

Selecting a row shows that process's log in the lower half. The drop-down there
picks any log directly.

Start and stop run in the background, so the panel never freezes. Stop ends the
slot's supervisor, learner, servers and any evaluation in progress.

Closing the panel while training runs asks first: **Yes** stops everything,
**No** leaves it running in the background, **Cancel** keeps the panel open.

### Slots

A slot is an independent training run: its own weights, batches, checkpoint, logs
and power level. `main` trains the bot to run the map; `windup` trains it to build
its own speed before the start.

The slot drop-down picks what **start**, **stop** and the power level apply to.
It defaults to **all**.

A slot's settings are the daemon arguments in `data/daemon_args.txt` (main) or
`data/daemon_args_<name>.txt`. A new file there is a new slot.

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

Training scores itself every few minutes and saves the fastest finish of each
evaluation, or the furthest run if none finished.

## How a run is put together

Each episode is one continuous attempt at the whole map.

1. **Wind-up** - the bot starts standing and the `windup` policy drives. Each
   decision is a gesture: a strafe key and a mouse speed, turning towards that
   key. On the ground it holds forward; the view turns at most 3.5 degrees a
   tick and eases in. A chosen side is kept at least 12 ticks. Ground time is
   free, as it is on the timer.
2. **Takeoff** - the wind-up jumps from inside the start zone, at least 16 units
   in from its edge. The recorded runs jump 37 to 53 units in.
3. **Zone air** - it keeps strafing in the air, left and right, until it leaves
   the zone. The recorded runs spend 43 to 49 ticks here and turn 170 to 230
   degrees. Walking to the edge, walking off anything, never jumping, landing
   again inside the zone, or staying in the air there too long all count as a
   failed wind-up.
4. **Leaving the zone** - the clock starts, the combined speed is capped at 475
   u/s as the server does for players, and the `main` policy takes over with the
   mouse and keys as they are. The start zone comes from the timer's database;
   `setup_map.py` copies it and re-times the reference run from the moment it
   left the zone.
5. **The run** - the bot surfs until it finishes, falls, or stops advancing.

The `main` slot opens a share of its runs with the learned wind-up and the rest
with a recorded one (`-LearnedMix`). The `windup` slot trains the wind-up: each of
its episodes is a whole run, with `main` flying everything after the zone, and
the score is how that run ends: +10 for finishing at the reference time, more
when faster, and between -10 and -20 for not finishing, by how far it got. A
failed wind-up scores below that. Evaluations and replays use the learned
wind-up when there is one.

### Hands

The run acts through these limits, set per slot in the daemon arguments:

| setting | default | what it limits |
|---|---|---|
| `-MouseAcc` | 1.5 | how fast the mouse speed may change in the run, degrees per tick per tick |
| `-MouseMax` | 7 | mouse speed, degrees per tick |
| `-MinPress` | 12 | ticks a strafe key stays down |
| `-MinCoast` | 6 | ticks with no key before the next press |
| `-MaxCoast` | 16 | ticks with no key before a press is required |
| `-SwitchGap` | 2 | ticks the new key may lag a change of side in the run |

The wind-up has hands of its own. It keeps a chosen side 12 ticks, may lift and
press again at any decision in the zone air but is hands-off there at most
`-MaxCoast` ticks, and on a reversal its new key waits at most `-MinCoast` ticks
while the mouse still turns the old way. Its mouse eases at 1.0 deg/tick^2 on the
ground (at most 3.5 deg/tick) and 2.5 in the zone air (at most `-MouseMax`).
`-MinPress`, `-SwitchGap` and `-MouseAcc` do not apply to it.

The wind-up slot flies the frozen main policy after the zone under its own hand
settings, so `data/daemon_args_windup.txt` should give the same ones as the main
slot's arguments. The run is also paid for the speed it keeps (`-EnergyScale`, default
40000: that much of half the speed squared plus gravity times height earns 1, so
dropping down a ramp neither earns nor costs, and a hard landing or a scrape
costs at once). 0 turns it off. One decision earns or loses at most
`-EnergyClamp` (5) from it, so hitting a wall at full speed does not outweigh a
finish. The policy only sees the actions these limits allow, and the learner
knows which those were.

`python tools/humanlike.py <replay>` puts a run next to the record on the same
map. Every evaluation runs it and logs a `looks:` line.

Jump is held throughout. With `sv_enablebunnyhopping 1` that means clipping a
ramp auto-hops instead of sticking and dumping momentum.

### Continuous vs segmented

Every episode starts at the beginning. The alternative is starting from the 24
checkpoints spread along the route, which is easier to learn from but optimises
"advance from anywhere" rather than "complete the map". A share of episodes do
start mid-map, set by `-StateMix`.

To sample all checkpoints instead, set `'+csai_states', '0'` in `tools/daemon.ps1`.

The end-to-end numbers are `runs_from_start`, `finished_from_start` and
`median_full_run_s` in `data/train_log.csv`. Progress percentages in older entries
of `docs/experiments.md` measured progress from a random checkpoint and are not
comparable.

## Using a different map

Set a time on it with the timer, then:

```bash
CsAI.bat teach surf_dune
```

It locates the timer's replay for that map, derives the reference line, the
restart checkpoints and the recorded wind-up, validates the result, copies the
start zone from the timer's database, times the reference run from leaving it,
adds the map to `-Map` in `data/daemon_args.txt` and restarts training if it is
running. Maps found in `download/maps` count as installed.
`CsAI.bat forget surf_dune` takes it out again.

The setup step on its own, without adding the map to training:

```bash
python tools/setup_map.py surf_dune
```

It builds the route, restart points, recorded wind-up and record run, then checks
them. `--check` only checks a map that is already set up (it also refreshes the
start zone and the reference time).

One policy learns all the maps: each slot's servers are shared out between them,
every batch says which map it came from, and evaluations take turns. The panel
and `CsAI.bat status` show each map on its own line, and the best result is kept
per map. When an evaluation beats the record, the supervisor log says
`RECORD BEATEN` and the time goes into `data/records.txt`.

The wind-up slot trains on the same maps; `teach` and `forget` change both
slots. On a map the wind-up slot does not train on, runs open with a recorded
wind-up instead of its learned one.

Track points are 92 units apart on every map, because that is what the policy
learned on. A track built at another spacing changes what it sees ahead.

On maps with stages, failed attempts that reset to a stage start are detected and
dropped, and the teleport between stages is not counted as distance travelled.
Counting it would pay an enormous one-tick reward for being teleported.

## Recording runs

The route and the 24 restart points come from one run only: the timer's record
for the map. To change them, set a new record and teach the map again.

Runs recorded in game give the bot recorded wind-ups: the part of each run before
the timer starts. The main slot opens a share of its runs with them, and falls back
on one when its own wind-up fails. The first 16 usable runs are used (an opening of
16 to 512 ticks). Everything after the start zone is not used in training, so a
recording helps only through how it starts.

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

Different starts: jumping from different places in the zone, on either key, at
different speeds. Five or six is enough. How the rest of the run goes does not
matter.

`python tools/setup_map.py <map> --check` also shows how far each recording strays
from the route. That matters for the timer's record, which is the route; for the
other runs it only matters to a `bc.py` capture.

### Commands

In chat:

| command | effect |
|---|---|
| `!csai_runs` | how many runs are saved, and whether recording is active |
| `!csai_save` | save the current run without finishing it |
| `!csai_drop` | discard the current recording and restart it |

`!csai_save` keeps a run that was not finished; only its opening is used. A run
that reached a section by teleport has no opening and is not used.

### Using new recordings

The servers read the recordings when they start, so restart training (or teach
the map again, which restarts it) to use new ones:

```bash
CsAI.bat teach surf_demise
```

`tools/bc.py` can instead copy a policy from a capture (`+csai_democapture`), but
that replaces the trained policy with the copy, as generation 1. The old one is
kept as `data/ckpt.npz.before_bc` and `weights.txt.before_bc`. It is only worth
it to start a brand-new policy.

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

- `logs/daemon.log`, `logs/daemon_<slot>.log` - starts, stops, power changes,
  crashes and restarts.
- `logs/learner.log`, `logs/learner_<slot>.log` - the learner's output.
- `logs/eval_<slot>.log` - every evaluation.
- `data/best.<map>.txt`, `data/best_<slot>.<map>.txt` and
  `data/ckpt_best*.<map>.npz` - the best evaluation so far on each map and the
  checkpoint that made it.
- `data/records.txt` - every record the bot has beaten.
- `cstrike/logs/csai_<slot>_a<N>.log` - each game server's console.
- `data/train_log.csv` - one row per generation.
- `docs/experiments.md` - each configuration change and whether it worked.

## If something looks wrong

The supervisor restarts the learner or any server that dies, and any server that
goes quiet for ten minutes while the others keep working. It restarts everything
if no generation completes for long enough, and reports when batch files pile up
unread.

Training and the viewer are independent; stopping one does not affect the other.
