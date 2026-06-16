# slangfx live shader-param playground — design

**Date:** 2026-06-16
**Branch:** `realtime-streaming`
**Roadmap:** completes **R4 (live parameter control plane)**; deliberately
sidesteps R3 (native swapchain window) by displaying frames in a separate app.

## Goal

A UI for experimenting with shader parameters **in real time while a video
streams** through the effect. Drag a slider, see the effect change on live
video with no hitch and no pipeline rebuild. When a look is dialed in, export
the parameter string so it can be reused with `beat_cut --shader-params` /
`slangfx --params`.

## Architecture: split core + GUI app

Two units joined by a thin seam. slangfx stays a frame filter; a separate
Python app owns display and controls.

```
video.mp4 ─ffmpeg(loop,scale)─▶ rawRGBA ─▶ slangfx ──rawRGBA──▶ reader thread ─▶ Dear PyGui texture
                                            ▲                                         │
                                            └────────── UDP k=v (localhost:P) ◀── sliders
```

Rationale: the C change is small and purely additive; the heavy lifting (UI,
orchestration) lives in Python where iteration is cheap. No native windowing,
swapchain, or GUI toolkit is introduced into the C codebase.

## Unit 1 — slangfx C core: live param control plane

### Why it's nearly free
`slang_pipeline_run` already re-reads each pass's
`mod->params[].default_value` into the push-constant blob **every frame**
(`src/slang_pipeline.c` ~line 1637). Mutating those values between frames
changes the effect on the next frame. No rebuild, no reflection changes.

### Changes
1. **`slang_pipeline_set_param(struct slang_pipeline *p, const char *name,
   float value)`** (new, in `slang_pipeline.h/.c`): find every pass whose
   `mod->params[]` contains `name` and set its `default_value`. A param name
   may appear in multiple passes; set all matches. No-op if the name is unknown
   (return a count so callers can warn on typos).
2. **Wire `--params` at startup.** Today `main.c` parses `--params` into
   `args.params` but never passes it to the pipeline — it is a **no-op**. Add a
   small `apply_params_string(pipeline, str)` that splits on `,` then `=` and
   calls `set_param`. This fixes the existing dead flag (and makes
   `beat_cut --shader-params` work for the first time).
3. **`--control-port N`** (new flag): bind a **non-blocking localhost UDP**
   socket. At the top of each frame loop in `main.c`, drain *all* pending
   datagrams; each datagram is one or more `k=v` lines (newline- or
   comma-separated). Reuse the same parser as `--params`. Newest value per name
   wins. The serial single-threaded loop means no locks are needed.
   - Winsock (`WSAStartup`, `closesocket`) guarded under `#if defined(_WIN32)`;
     POSIX sockets otherwise.
   - Socket bound to `127.0.0.1` only (no remote exposure).
4. Batch mode is unchanged when `--control-port` is absent. The offline parity
   path stays byte-identical.

### Control protocol
Plain text, `name=value`, newline- or comma-separated, one or more per UDP
datagram. Example datagram: `warp=0.12\nspeed=0.4`. Chosen over JSON so the C
side needs no JSON parser and can reuse the `--params` splitter. Fire-and-
forget; loss is acceptable because the next slider movement resends.

## Unit 2 — `slangfx/wrappers/slangfx_live.py` (Dear PyGui)

Single dependency: `pip install dearpygui`.

### Launch
```
python wrappers/slangfx_live.py -i video.mp4 --preset <path.slangp> \
       [--width 1280] [--height 720] [--control-port 9000] [--slangfx PATH] [--ffmpeg PATH]
```

### Responsibilities
1. **Orchestrate subprocesses.** Spawn `ffmpeg -stream_loop -1 -i video -vf
   scale=W:H -f rawvideo -pix_fmt rgba -` and pipe its stdout into
   `slangfx --preset … --width W --height H --control-port P`; read slangfx's
   stdout as the processed RGBA stream. (Preview size defaults to 1280x720 for
   smooth realtime regardless of source resolution.)
2. **Reader thread.** Read exactly `W*H*4` bytes per frame into a single-slot
   "latest frame" buffer, dropping stale frames so the UI never blocks
   (newest-wins). The Dear PyGui render loop uploads the latest frame to a raw
   texture shown in the preview pane.
3. **Auto-build sliders.** Parse the shader's
   `#pragma parameter name "label" default min max step` lines from the
   `.slang` file(s) referenced by the preset, applying `.slangp` default
   overrides. One `add_slider_float` per param, labeled, ranged, stepped.
4. **Send updates.** On slider change, send `name=value` over UDP to
   `127.0.0.1:P`.
5. **Buttons:**
   - **Copy params** — assemble the current `k=v,k=v` string, copy to clipboard
     and print to stdout (ready to paste into `--shader-params`).
   - **Reset to defaults** — restore parsed defaults and resend.
   - **Pause** — stop reading frames (freeze preview); ffmpeg/slangfx keep the
     pipe warm or are paused via stdin flow control.

### Why these tech choices
- Preview at 720p: slangfx render is ~1ms/frame at 720p after the HOST_CACHED
  readback fix (R0 finding); pipe throughput ~220 MB/s at 60fps is comfortable.
- Dear PyGui is purpose-built for live texture + control panels and matches the
  project's existing Python tooling (`wrappers/slangfx.py`, `beat_cut.py`).

## Scope

**In (v1):** one preset per launch, live sliders from `#pragma parameter`,
looping video preview, copy/reset/pause, `--params` fix, `--control-port`.

**Out (noted for later):**
- Live preset-switching (needs subprocess teardown/rebuild + slider rebuild).
- MIDI/OSC control inputs (R4 stretch).
- Audio reactivity (R5).
- Recording/exporting while previewing.
- Native swapchain window (R3) — the separate app replaces it for v1.

## Testing

- **C unit test:** call `slang_pipeline_set_param` then run one frame and assert
  the value reaches the push blob at the param's resolved offset. Add to the
  existing test target alongside `test_slangp_parse`.
- **Offline parity:** batch render of a real clip stays byte-identical with no
  `--control-port` (the safety net that the additions changed no pixels).
- **`--params` regression:** a batch render with `--params 'k=v'` now visibly
  differs from the default (proving the dead flag is alive), and matches an
  equivalent `.slangp` default.
- **Manual end-to-end:** launch on a test clip; confirm a slider visibly changes
  the live effect with no hitch; confirm **Copy params** → `beat_cut
  --shader-params` reproduces the dialed-in look.

## Risks / notes

- **Pipe stall / backpressure:** if Python stops reading, slangfx blocks on
  stdout write. The reader thread + drop policy keeps it draining; Pause must
  keep draining (or intentionally let it block) — decide in implementation.
- **Param in multiple passes:** `set_param` updates all matches by design (e.g.
  a shared `amount`). Document this.
- **Windows sockets:** guard all socket code under `_WIN32`/POSIX; init/cleanup
  Winsock once.
- **Preset path:** presets live in `beat-cut/shaders/…`, outside the slangfx
  repo; the path is passed in by the caller (no coupling).
