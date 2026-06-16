---
title: Usage
---

# Usage

Three ways to drive slangfx, easiest first.

## 1. The Python wrapper (recommended)

`wrappers/slangfx.py` orchestrates the whole pipeline: probes input
dimensions and frame rate, spawns ffmpeg for decode + encode, pipes raw
RGBA frames through `slangfx`, muxes audio passthrough into the result.

```bash
python wrappers/slangfx.py \
  -i input.mp4 \
  --preset path/to/preset.slangp \
  -o output.mp4
```

### All wrapper options

| Flag | Default | Effect |
|---|---|---|
| `-i PATH` | (required) | Input video file. Anything ffmpeg can decode. |
| `-o PATH` | (required) | Output file. Container inferred from extension. |
| `--preset PATH` | (required) | Path to a `.slangp` preset. |
| `--params 'k=v,...'` | none | Override `#pragma parameter` defaults at the CLI (applied by name to all passes that declare the parameter). |
| `--slangfx PATH` | `slangfx` (PATH) | Path to the `slangfx` binary. |
| `--ffmpeg PATH` | `ffmpeg` (PATH) | Path to ffmpeg. |
| `--vcodec NAME` | `libx264` | Output video codec. Use `h264_nvenc` for hardware encode. |
| `--crf N` | `20` | x264 CRF (0–51, lower = bigger / better). |
| `--preset-x264 NAME` | `veryfast` | x264 encoder preset. |

## 2. Direct pipes

Skip the wrapper for unusual workflows (live capture, custom codec
chains, multi-track output):

```
ffmpeg -i in.mp4 -f rawvideo -pix_fmt rgba - \
  | slangfx --preset PRESET.slangp --width W --height H \
  | ffmpeg -f rawvideo -pix_fmt rgba -s WxH -framerate FR -i - \
          -i in.mp4 -map 0:v -map 1:a -c:v libx264 -c:a copy out.mp4
```

The first ffmpeg decodes and converts to RGBA. slangfx applies the
shader. The second ffmpeg encodes the processed RGBA, optionally pulling
audio from a second input (the original file).

### `slangfx` CLI options

| Option | Required | Default | Effect |
|---|---|---|---|
| `--preset PATH` | yes | — | Path to `.slangp` |
| `--width N` | yes | — | Frame width in pixels |
| `--height N` | yes | — | Frame height in pixels |
| `--params 'k=v,...'` | no | — | Override declared parameters by name (applied to every pass that declares each one). |
| `--control-port N` | no | off | Bind `udp://127.0.0.1:N` and apply live `name=value` updates (newest wins), drained at each frame start. No rebuild. Powers the live tuner below. |
| `--frame-history N` | no | 8 | Cap `OriginalHistory` ring depth. Most shaders use 0–2. |

`slangfx --help` prints the full list.

## 4. Live parameter tuner (Dear PyGui)

`wrappers/slangfx_live.py` streams a video through a preset and shows it in a
window with one slider per `#pragma parameter`. Dragging a slider sends a live
`name=value` update over UDP (`--control-port`), applied on the next frame with
no rebuild. The **menu bar** switches shader or video on the fly — the
**Shader** and **Video** menus quick-pick every `.slangp` found under the
shaders tree / every video beside the current one (plus a **Browse…** dialog),
and the **Params** menu has copy/reset/pause. Switching restarts the
ffmpeg|slangfx pair (each on a fresh control port) and rebuilds the sliders for
the new preset; any source is letterboxed into a fixed preview so different
aspect ratios just work. **Copy params** emits the `k=v,k=v` string for
`slangfx --params` / `beat_cut --shader-params`.

Install the preview tool's dependencies (the `slangfx` binary itself has none),
then launch it on any clip and preset:

```bash
python -m pip install -r wrappers/requirements.txt
python wrappers/slangfx_live.py -i my_clip.mp4 \
  --preset path/to/effect.slangp        # add --width 1280 to set preview size
```

Once the window is open you don't need to relaunch to try other looks — use the
menu bar:

- **Shader →** pick any discovered `.slangp` (or *Browse…*) to swap the effect live.
- **Video →** pick any sibling clip (or *Browse…*) to swap the source live.
- **Params →** Copy params / Reset to defaults / Pause.

Drag any slider to change that parameter on the live video instantly.

| Flag | Default | Effect |
|---|---|---|
| `-i PATH` | (required) | Video/image to stream (looped). |
| `--preset PATH` | (required) | `.slangp` whose `#pragma parameter`s become sliders. |
| `--width N` / `--height N` | auto | Preview size; width-only preserves source aspect (defaults to ≤1280 wide). |
| `--fps N` | 30 | Preview frame rate. |
| `--control-port N` | 9000 | Base UDP port for live updates (each switch uses the next port). |
| `--selftest` | off | Headless check: confirms live control works and a shader switch succeeds (no window). |

The preview runs at 720p-ish by default so it stays smooth regardless of source
resolution; the exported params apply unchanged at full render resolution.

### Environment variables

| Var | Effect |
|---|---|
| `SLANGFX_DEBUG_REFLECT=1` | Per-shader, dump the reflected push-constant block layout, UBO layout, and sampler bindings to stderr. Useful when porting / debugging an unfamiliar shader. |
| `SLANGFX_DEBUG_FORMAT=1` | Log the resolved framebuffer format per pass (handy when diagnosing `float_framebuffer` / `srgb_framebuffer` issues). |

## 3. Live capture / streaming

The pipe model is genuinely streaming — constant memory, no
look-ahead. You can chain a webcam or screen capture in front:

```bash
# Linux V4L2 webcam → CRT shader → RTMP to a local server
ffmpeg -f v4l2 -i /dev/video0 -f rawvideo -pix_fmt rgba - \
  | slangfx --preset crt/easymode.slangp --width 640 --height 480 \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 640x480 -framerate 30 -i - \
          -c:v h264_nvenc -tune zerolatency -bf 0 -f flv rtmp://localhost/live/test
```

For real-time use, prefer:

- Lower resolutions (720p instead of 1080p halves shader cost)
- Hardware encode (`-c:v h264_nvenc`, `h264_qsv`, `h264_videotoolbox`)
- `-tune zerolatency -bf 0` on the encoder to disable B-frames and reduce latency

See [Limitations]({{ '/limitations.html' | relative_url }}) for what to
expect at different resolutions.

## Tuning shader parameters

slang shaders declare their own runtime parameters via `#pragma
parameter`. To override them, edit the `parameters = "..."` block of the
`.slangp` preset:

```ini
shaders = 1
shader0 = newpixie-crt.slang
filter_linear0 = true

parameters = "curvature;vignette;ghosting"
curvature = 1.5
vignette  = 0.8
ghosting  = 1.5
```

Parameter names are case-sensitive and must match exactly the names
declared in the shader's `#pragma parameter` lines. To find them, inspect
the `.slang` file or run with `SLANGFX_DEBUG_REFLECT=1` and look at the
push-constant or UBO field names.

## Common workflows

### Apply a CRT shader to a video for posting online

```bash
python wrappers/slangfx.py \
  -i my-clip.mp4 \
  --preset crt/newpixie-crt.slangp \
  --crf 18 \
  -o my-clip-crt.mp4
```

### Apply a VHS / NTSC look

```bash
python wrappers/slangfx.py \
  -i my-clip.mp4 \
  --preset vhs/ntsc-vcr.slangp \
  -o my-clip-vhs.mp4
```

### Color-grade only (no scanlines / curvature)

```bash
python wrappers/slangfx.py \
  -i my-clip.mp4 \
  --preset misc/image-adjustment.slangp \
  -o my-clip-graded.mp4
```

Edit `image-adjustment.slangp`'s `parameters = ...` block to dial in
gamma, saturation, contrast, etc.

### Process to lossless intermediate (for further editing)

```bash
python wrappers/slangfx.py \
  -i my-clip.mp4 \
  --preset crt/newpixie-crt.slangp \
  --vcodec libx264rgb --crf 0 \
  -o my-clip-crt.mov
```
