# ESP32Rive

Plays [Rive](https://rive.app) animations on an ESP32-P4 using the official
[Rive C++ runtime](https://github.com/rive-app/rive-runtime) and the
[ThorVG](https://github.com/thorvg/thorvg) software vector rasteriser, on the
**Waveshare ESP32-P4-WIFI6-Touch-LCD-4B** / **ESP32-P4-86-Panel-ETH-2RO**
(720×720 MIPI-DSI display, GT911 touch, ES8311 audio codec).

## How it works

```
 .riv ──► Rive runtime ──► ThorvgRenderer ──► DisplayList ──┬─► lane 0 (core 1): ThorVG canvas, top half ────┐
         (state machines,  (records draw      (paths, paints,│                                                ├─► ARGB8888 frame (PSRAM)
          animations)       calls, no pixels)  clips, images) └─► lane 1 (core 0): ThorVG canvas, bottom half ─┘          │
                                                                                                                       ▼
                                             PPA: changed rows ARGB8888 → RGB565 (+ optional 2× upscale) → panel back buffer
                                                                                                                       │
                                                                                                                       ▼
                                                                         ST7703 panel, 2 frame buffers, page flip on vsync
```

The Rive runtime parses files and runs animations, but it doesn't draw: it
calls a `rive::Factory` and a `rive::Renderer` that the platform provides.
Rive's own renderers all need a GPU, and the P4 doesn't have one.
`components/rive_thorvg` implements both on top of ThorVG's CPU renderer:

- **`ThorvgRenderer`** records each Rive draw call into a `DisplayList`:
  immutable snapshots of the path, paint, transform, opacity and clip stack.
- **`ThorvgLane`** owns one ThorVG canvas for a horizontal slice of the
  frame and keeps its ThorVG shapes between frames. Each frame it compares
  the new list with the last one:
  - same draws in the same order: changed draws update their ThorVG shape in
    place (new path, transform or fill), or get a new one if their stroke
    style, blend mode or opacity changed;
  - anything else: the tree is rebuilt.
  With partial redraw on, ThorVG then redraws only the regions that changed.
- **Two lanes** (top and bottom half) render in parallel, one per CPU core.
  Every 8 frames the split moves toward the slower lane, so both take about
  as long.
- Frames alternate between **two ARGB8888 buffers**. Each lane has one
  ThorVG canvas per buffer, so partial redraw stays exact.
- While the cores draw frame N+1, the P4's **PPA** (pixel-processing
  accelerator) converts frame N's changed rows to the panel's RGB565 format,
  and can upscale them, in the background. The panel has three frame
  buffers, so there is always a free one to copy into without waiting for a
  vsync.

## Repository layout

| Path | What |
|---|---|
| `main/` | App: display init (`display_p4.c`), playback loop (`main.cpp`), test files in `assets/` (one is embedded, chosen in menuconfig) |
| `components/rive_thorvg/` | Rive → ThorVG adapter: `ThorvgFactory`, `ThorvgRenderer` (records draws), `ThorvgLane` (retained ThorVG canvas per core), `RivePlayer` |
| `components/rive_runtime/` | ESP-IDF build wrapper for `third_party/rive-runtime` |
| `components/thorvg/` | ESP-IDF build wrapper for `third_party/thorvg` (software engine + PNG/JPG/WebP loaders), plus build-time patches in `fast/` (see below) |
| `third_party/` | Git submodules: rive-runtime (`runtime-v0.1.675`), ThorVG (`v1.1.3`), and for text and layouts the versions rive-runtime pins: HarfBuzz (`rive-app/harfbuzz@rive_13.1.1`), SheenBidi (`v2.6`), Yoga (`rive-app/yoga@rive_yoga_dee88d2dfcad`), and for audio miniaudio (`rive-app/miniaudio@rive_changes_11`) |
| `host/` | Desktop build of the same code that renders `.riv` frames to image files |
| `tools/` | Submodule setup scripts |
| `components/patch_source.cmake` | Applies small fixes / hooks to upstream files at build time (see below) |
| `main/touch_gt911.c` | Polled GT911 touch driver (I2C on GPIO 7/8), events forwarded to the Rive state machine |
| `main/pie_selftest.cpp` | Boot self-test + benchmark for the PIE (SIMD) gradient span |
| `main/raster_profile.cpp` | Optional per-core profile of ThorVG's raster functions (linker `--wrap`) |

**Upstream portability fixes.** On the ESP32 RISC-V toolchain, `uint32_t`
and `int32_t` are `long` types, not `int`. That exposes two small type
mismatches the desktop compilers accept:

- `rive-runtime/src/shapes/paint/color.cpp`
- ThorVG's `tvgSwRasterCore.cpp`

The submodules aren't modified. CMake builds a patched copy of each file in
the build directory, and skips the patch automatically once it's fixed
upstream.

**ThorVG patches** (`components/thorvg/thorvg_sources.cmake`, code in
`components/thorvg/fast/`), each verified against stock ThorVG on the host:

- **Partial-redraw gradient fix** (`sw_rle_clip.h`). Under partial redraw,
  ThorVG draws an unchanged shape once per dirty region it touches. Solid
  fills clip to the region, but gradients didn't, so an unchanged gradient
  shape was drawn several times over its whole area. That gave wrong colours
  for translucent or blended gradients and wrong layering for opaque ones.
  The patch clips gradient spans to the region.
- **Faster linear-gradient spans** (`sw_fill_fast.*`). The span loop is
  specialised per blend operator, with no per-pixel function-pointer call or
  spread-mode switch, and with 8-pixel chunks that need no clamping. The
  output is bit-identical. Translucent spans can use PIE (SIMD), see below.
- **Skip the dirty-region clear** while the lane background is opaque: the
  background repaints those regions anyway.
- **Bigger scanline cell buffer** (2× ThorVG's default). ThorVG converts an
  outline to coverage spans in horizontal bands. When a band needs more
  cells than the buffer holds, it is thrown away, halved and redone. That
  happened ~13 times per frame on the car at 720; now almost never, which
  cuts span generation by 20%. The output is identical.

ThorVG also doesn't redraw a removed paint's old area, so `ThorvgLane` hides
a paint before removing it, which marks its area dirty.

## Setup

Requires **ESP-IDF v5.5** or newer (builds with v5.5.2 and v5.5.6).

1. Fetch the pinned sources (once, from the repo root):

   ```powershell
   # PowerShell
   ./tools/setup_submodules.ps1
   ```
   ```bash
   # bash / Git Bash
   bash tools/setup_submodules.sh
   ```

   Later clones only need `git submodule update --init --depth 1`.

2. Chip revision: `sdkconfig.defaults` pins the build to ESP32-P4
   **revision < v3.0** (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`), because this
   board reports `revision v1.0` (`esptool.py --port COMx chip_id`). For a
   v3.x board, set it to `n` and delete `sdkconfig` before building.

3. Build and flash:

   ```bash
   idf.py set-target esp32p4
   idf.py build flash monitor
   ```

   The first build downloads the ST7703 panel driver
   (`waveshare/esp_lcd_st7703`) from the ESP component registry.

The monitor prints timing lines every 2 seconds:

```
I (5123) rive: 6.6 fps | advance 6.6  build 2.8  render 131.0  blit wait 0.0  vsync 0.0 ms | dirty 100%  copied 100% | 106 draws, 0 rebuilds
I (5123) rive:   lane 0 (core 1, rows 0-400): 130.2 ms = apply 13.0 + prep 25.1 + fill 92.1
I (5124) rive:   lane 1 (core 0, rows 400-720): 129.8 ms = apply 13.1 + prep 45.0 + fill 71.7
```

(These are example numbers, not measurements.)

- **advance**: Rive animation and state machine update
- **build**: recording Rive's draw calls
- **render**: wall time of both lanes running in parallel. Per lane:
  - **rows**: its current slice
  - **apply**: creating ThorVG shapes for changed draws
  - **prep**: ThorVG tessellation and span generation
  - **fill**: pixels
- **blit wait**: time still waiting for the previous frame's background copy
  to the panel. It should be about 0.
- **vsync**: waiting for a free panel buffer. It should be about 0.
- **dirty**: share of rows redrawn
- **copied**: share of rows copied to the panel
- **rebuilds**: frames where the draw order changed, so ThorVG's shapes were
  rebuilt from scratch
- A **split moved** line appears when the balancer moved the split between
  the lanes

## Using your own animation

Four test files are included; pick one in `idf.py menuconfig` → Rive
player → **Embedded .riv file**:

- **Off-road car** (`sample.riv`, default): animation, gradients, clips
- **Electrified button** (`electrified_button.riv`): state machine with
  pointer listeners and text. Tap it.
- **Scroll snap** (`scroll_snap.riv`): text in layouts, touch scrolling
- **Sound** (`sound.riv`): a red square that plays a sound when touched
  (hover listener + audio event). Needs a speaker on the board's MX1.25
  header.

To use your own file, replace `main/assets/sample.riv` with it (or add it
and a choice in `main/Kconfig.projbuild`) and rebuild. The artboard is
scaled to fit the 720×720 screen (`Fit::contain`, centred), so a 720×720
artboard maps 1:1. The player runs the artboard's default state machine. If
there isn't one, it uses the first state machine, and failing that, the first
animation.

## Configuration (`idf.py menuconfig`)

- **Rive player → Render on both CPU cores** (default on): top half on
  core 1, bottom half on core 0, with the split balanced automatically.
- **Rive player → Redraw only changed regions** (default on): ThorVG
  SmartRender plus copying only changed rows to the panel.
- **Rive player → Render at 360x360 and upscale 2x with the PPA**: renders a
  quarter of the pixels, for a large speed-up at the cost of slightly softer
  edges.
- **Rive player → Use PIE (SIMD) for translucent gradient spans** (default
  on): checked against the C version by a self-test at boot and only used if
  it matches exactly. The result is logged with cycles per pixel.
- **Rive player → Experiment: profile ThorVG raster functions** (default on
  for now): per-core ms per frame for each raster function, plus gradient
  pixel counts.
- **Rive player → Update changed shapes in place** (default on): see "How
  it works". The output is identical; turn off to compare.
- **ThorVG → Faster linear-gradient spans** / **Skip the dirty-region clear**
  / **Scanline cell buffer size**: on by default; turn off (or set the size
  to 75%) to compare with stock ThorVG.
- **Rive player → Audio output** (default on, needs Rive runtime → Audio):
  plays Rive's audio through the ES8311 codec and NS4150B amplifier to the
  2-pin MX1.25 speaker header (8 Ω, 1–3 W speaker, not included with the
  board). Volume, sample rate (48 kHz) and pins (MCLK 13, BCLK 12, WS 10,
  DOUT 9, amplifier enable 53; codec on the touch I2C bus at 0x18). The
  stats line logs the output peak level, so you can see sounds play
  without a speaker. **Decode sounds up to N s in advance** (default 10;
  0 = always decode while playing).
- **Rive player → Backlight brightness**, **stats interval**

## Performance

The first measurement on the board (sample car animation, 720×720, rendered
as one full frame in PSRAM) gave **4.2 fps**:

- advance 4 ms
- build 18 ms
- raster 191 ms
- blit 26 ms

A host profile of the same frames shows about 45 M instructions of ThorVG
work per frame:

- gradient fills: 35%
- path rasterisation: about 25%
- per-pixel blending: most of the rest

Strip rendering into internal SRAM (tried next) didn't speed up the raster
(211 ms) and only cut the copy to the panel. So the raster is **CPU-bound**,
not PSRAM-bound. 360×360 with the PPA's 2× upscale reached **7.8 fps**.

Hence the current design:

- **Keep shapes between frames.** Unchanged draws aren't re-tessellated or
  freed and reallocated.
- **Partial redraw.** Only changed regions are rasterised and copied.
- **Both cores, balanced.** Two lanes render in parallel.
- **Background copy.** The copy to the panel overlaps with drawing the next
  frame.

| Mode (sample car) | First version | Retained + partial + 2 cores |
|---|---|---|
| 720×720 | 4.2 fps | 5.2 fps |
| 360×360 | 7.8 fps | 11.9 fps |

That last measurement had the cores unbalanced: the bottom half took 154 ms
vs 109 ms for the top. The copy to the panel also still ran after rendering
(26 ms at 720). Both are now addressed.

Since then: lane culling, the gradient span rewrite, skipping redundant
clears and background fills, and the two partial-redraw fixes above. At
720×720 the car went from 6.2 to 9.0 fps on the board (see the project
notes for every step).

The host test compares partial redraw (1 or 2 lanes, 1 or 2 buffers, with or
without balancing) against a true full redraw on the car, the knight, a bank
card with blended gradients, and files with images. Every frame matches to
within 2/255 per channel. ThreadSanitizer reports no races with 2 lanes, 2
buffers and balancing.

## Host test build (Linux / macOS / WSL)

Renders frames through the exact adapter code the board runs. Use it to check
an animation, or a change to the adapter, without flashing:

```bash
cmake -S host -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-host
mkdir -p out && ./build-host/rive_host_render main/assets/sample.riv out 30
# → out/frame_000.ppm … frame_029.ppm (+ average timing per frame)
# Arguments: <file.riv> <out_dir> [frames=10] [fps=30] [size=720] [lanes=2] [partial=1]
#            [buffers=2] [balance=1] [cull=1] [bgskip=1] [inplace=1]
#            [wav=<file>] [tap=x,y,downFrame,upFrame] [predecode=1]
# wav: also mix the file's audio (48 kHz stereo) and write it to a WAV file.
# predecode: decode the sounds in advance as the board does (0: stream them).
# tap: one touch at panel pixel x,y, like the board's touch screen, e.g.
#   ./build-host/rive_host_render main/assets/sound.riv out 120 30 720 2 1 2 1 1 1 1 out/sound.wav 396,180,10,60
# Lanes beyond the first run on worker threads, like core 0 on the board.
```

## Supported Rive features

Working:
- Shapes, fills and strokes (joins, caps, fill rules)
- Linear, radial and angular gradients
- Clipping, including nested clips
- Blend modes
- Opacity
- Bitmap images (PNG, JPEG, WebP)
- Animations, state machines
- Pointer input: GT911 touch is forwarded to the state machine (move to the
  touch point, down, move, up). Animations without a state machine ignore
  it; touches are logged.
- **Text** (HarfBuzz + SheenBidi, fonts embedded in the `.riv`): glyphs are
  drawn as ordinary paths. menuconfig → Rive runtime → Text
- **Layouts** (Yoga). menuconfig → Rive runtime → Layouts
- **Audio** (miniaudio: MP3, WAV, FLAC, Vorbis): audio events play through
  the speaker header. Rive's engine runs as an "external" engine and a task
  on core 0 pulls 5 ms blocks from it. Sounds are decoded as they play, from
  the compressed bytes in the `.riv`, except that sounds up to 10 s
  (configurable) are decoded once when the file loads, already converted to
  the output's 48 kHz mono, so playing them only mixes ready samples (about
  30× cheaper; ~188 KB of PSRAM per second of sound). Rive mixes in mono,
  since the board has one speaker. menuconfig → Rive runtime → Audio
- Touch: a lifted finger also sends a pointer exit, so hover listeners fire
  on every tap

Not yet:
- **Hosted assets**: fonts or images stored outside the `.riv` (on Rive's
  CDN or as separate files) aren't loaded
- **Scripting** (Luau): compiled out
- **Mesh-deformed images**: ThorVG has no textured-triangle API. A warning is
  logged once.
- **Feathering** (soft edges): not drawn
- **Partial "additive" blending**: rounded to fully on or off
- **Non-normal blend modes inside a clipped group**: blend against the
  group's layer instead of the full background

## Next steps

- Load hosted assets (fonts / images referenced outside the `.riv`)
- Report the two partial-redraw bugs (unclipped gradient spans, no damage on
  paint removal) and the `EngineOption::Default` includes SmartRender
  surprise to ThorVG
- Load `.riv` files from the SD card or over Ethernet instead of embedding
  them

## Licences

Project code: MIT (see `LICENSE`). Rive runtime: MIT. ThorVG: MIT.
HarfBuzz: MIT ("Old MIT"). SheenBidi: Apache 2.0. Yoga: MIT. miniaudio:
public domain / MIT-0. esp_codec_dev: Apache 2.0. The files in
`main/assets/` are test assets from the rive-runtime repository
(`off_road_car.riv`, `electrified_button_simple.riv`, `scroll_snap.riv`,
`sound.riv`).
