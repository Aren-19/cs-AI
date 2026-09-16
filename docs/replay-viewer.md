# Web replay viewer — architecture

Goal: watch a bot run in the browser as a live 3D scene driven by a small
`.replay` file, the way offstyles.net does it — not a video.

## How offstyles.net actually does it

Established by inspecting the running site (2026-09-15). Loading
`/run/<id>` and pressing **View Replay** creates a 1280×720 WebGL2 canvas plus a
240×135 minimap, with follow-cam, speed readout, W/A/S/D + duck/jump key display,
a timeline scrubber and 0.25×–5× playback.

| Component | What it is | Transfer |
|---|---|---|
| `noclipRenderer-*.js` | noclip.website's Source-engine BSP renderer, WebGL2 | 661 KB |
| `/api/csspak/batch` | CS:S game assets (BSP, VTF textures, VMT materials) on demand | 534 KB |
| `bhop_replay_viewer_wasm_bg.wasm` | replay parser, Rust → WASM | 75 KB |
| `bz2Worker-*.js` | bzip2 worker for FastDL-style `.bsp.bz2` | 2 KB |
| `/api/replay?id=…` | the replay itself | 59 KB |

Renderer provenance is not a guess: the bundle contains the string `noclip`, BSP
lump names (`entities`, `texinfo`, `pakfile`), `VTF`/`vmt` material handling,
LZMA (Source compresses BSP lumps with it) and `#version 300 es` shaders. There
is no three.js or babylon.js — it is a purpose-built renderer.

The key economics: **the replay is tiny (59 KB) because it is only per-tick
position and angles. The map is the heavy part**, fetched once and cached.

## What we already have

- **Replay format fully decoded** — `tools/replay.py` parses shavit v9 end to end
  (40-byte frames: `pos[3]`, `ang[2]`, `buttons`, `flags`, `movetype`, packed
  `mousexy`, packed `vel`). Porting the frame decode to JS is trivial; the
  Rust/WASM step offstyles uses is not needed at our scale.
- **Teleport/segment handling** — the viewer must not interpolate across
  teleports, or the camera will fly between stages. `segments()` already
  identifies them by an implausible per-tick speed.
- **The bot's own runs** — the harness can emit a trajectory for any episode.

## What has to be built

1. **BSP → web geometry.** The real work. Two routes:
   - **(a) Reuse noclip's Source renderer.** MIT-licensed, already handles BSP +
     VTF + VMT + lightmaps. Highest fidelity, largest integration cost, and needs
     CS:S assets served (they are copyrighted — fine for a LAN/personal tool,
     not for public redistribution).
   - **(b) Pre-convert the BSP to glTF offline** and render with three.js. Much
     simpler, no runtime BSP parsing, no texture pipeline if we ship untextured
     or flat-shaded geometry. For watching a surf line, the ramp *shape* is what
     matters — textures are close to irrelevant.

   **Recommendation: (b) first.** A flat-shaded surf_demise is enough to see
   whether the bot's line is good, and it is a fraction of the work. (a) stays
   open as an upgrade.

2. **Replay playback.** Interpolate position/angles between ticks against a
   wall-clock timer, with playback rate control. Straightforward.

3. **Viewer UI.** Follow-cam and free-cam, timeline scrubber, speed/keys HUD,
   and — the thing offstyles does not have and we want — **overlay the bot's run
   against the human reference line**, plus the centerline the reward uses.

## Licensing note

CS:S map and texture assets are Valve's. Serving them publicly is redistribution.
For a personal/LAN tool this is moot; if this is ever hosted publicly, ship
geometry only (no textures) or require the user to supply their own game files.
