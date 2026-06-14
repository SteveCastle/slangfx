---
title: Realtime Roadmap
---

# Realtime Roadmap

slangfx today is an **offline batch filter**: a blocking `stdin → GPU →
stdout` loop with ffmpeg subprocesses on both ends, a mandatory host
readback every frame, RGBA8-only I/O, no swapchain, a frame *counter* (not a
clock), and parameters baked at pipeline-build time. This document plans the
work to turn the same `slang_pipeline` core into a **general low-latency
streaming engine** with pluggable sources/sinks, live parameter control,
audio reactivity, and GPU zero-copy / hardware decode.

It mirrors the phase style of [the build roadmap](roadmap.html): each phase is
a mergeable chunk that ends with a demo and a test.

## The four things that block realtime

1. **Blocking serial loop** (`src/main.c`): one frame fully in flight at a
   time; GPU idles waiting on CPU I/O.
2. **Mandatory host readback** (`slang_pipeline_run` reads every frame back to
   a host buffer) — pure latency when the destination is a screen.
3. **ffmpeg-as-subprocess + RGBA8 only** — CPU `sws` colour conversion and
   pipe copies on both ends; no GPU-resident frames; zero-copy impossible
   across a pipe.
4. **Counter time + startup-baked params** — `FrameCount` is a counter, not a
   wall clock; parameters are fixed at build time.

## Dominating decision: link libav\* directly

Zero-copy + hardware decode (R6) require **linking libavcodec / libavformat**
rather than shelling out to the `ffmpeg` binary. This reverses an explicit
choice in [architecture.md](architecture.html) ("we only depend on ffmpeg as a
runtime executable"). It is the right call for this goal but is the biggest
single decision: it changes build deps, and the GPL/LGPL status of the linked
libav\* affects distribution. The streaming **engine and source/sink seam
(R1)** are designed so the subprocess-pipe path and a future linked-libav\*
path are just two `frame_source`/`frame_sink` implementations.

## Target architecture

```
 sources (pluggable)        engine core (pipelined, threaded)         sinks (pluggable)
+----------------+      +--------------------------------------+   +------------------+
| file / libav   |      |  read thread -> [bounded queue] ->   |   | display(swapchain)|
| live capture   | ---> |  render thread (in-flight ring,      |-->| encoder/stream-out|
| net (RTMP/SRT) |      |  async upload||render||present,       |   | file/batch (compat)|
| Vulkan hwframe |      |  clock/pacer w/ drop policy, walltime)|   +------------------+
+----------------+      +-----------+------------------+-------+
                                    |                  |
                         control plane            audio engine
                    (MIDI/OSC/keys -> param     (capture -> FFT/onset/
                     snapshot per frame)         tempo -> reactive signals)
```

`frame_source` and `frame_sink` are C vtable interfaces (see `src/frame_io.h`);
the engine is agnostic to where frames come from or go.

## Phases

Effort is a relative T-shirt size, not a calendar estimate.

### R0 — Latency baseline & instrumentation *(S)*

- Per-stage timing in the binary behind `SLANGFX_METRICS=1` (read / render /
  write wall-clock, summary on exit).
- An out-of-tree harness that pushes synthetic timestamped frames through the
  real binary and reports the latency distribution + sustained throughput.
- Set the optimisation target (e.g. added latency budget at 1080p60).

**Exit:** a measured baseline and a regression guard for everything below.

### R1 — Decouple I/O behind source/sink interfaces *(M)*

- Define `frame_source` / `frame_sink` vtables (`src/frame_io.h`).
- Reimplement the current stdin/stdout raw-RGBA path *as* a
  `stdio_source` / `stdio_sink` — no behaviour change.
- `main.c` becomes: build source, build sink, build engine, pump.

**Exit:** offline parity suite stays green; a real clip byte-matches the
pre-refactor output.

### R2 — Threaded pipeline + wall-clock time *(L, critical path)*

- **R2a (CPU threading):** read / render / write on separate threads joined by
  bounded queues, overlapping CPU I/O with GPU work. Engine owns the render
  thread; a single thread owns Vulkan queue submission (queues are not
  thread-safe).
- **R2b (device pipelining):** N in-flight frames inside the device — per-frame
  command buffers + fences/semaphores, overlapping upload‖render‖readback;
  triple buffering. Revisit the PassFeedback end-of-frame image copy (a
  per-producer copy+sync chosen for simplicity in build-Phase 6) → parity-flip
  ring.
- **Wall-clock time:** thread a real time base (seconds + per-frame delta) into
  `slang_pipeline_run`; expose a `Time` standard push field while keeping
  `FrameCount` for corpus compatibility. A clock/pacer with an explicit
  drop/repeat policy for late/early frames and source stalls.

**Exit:** throughput no longer capped by CPU I/O; latency at the R0 target; a
stalled source does not stall render.

### R3 — Swapchain display sink *(M)*

- Windowing (GLFW or SDL3) + Vulkan swapchain.
- `display_sink` blits/renders the final pass straight into a swapchain image —
  **zero host readback** for preview. Vsync + present pacing.

**Exit:** a live media file plays through an effect on screen with bounded
latency and no readback.

### R4 — Live parameter control plane *(M)*

- Shared param table consumed as an atomic snapshot at each frame start (no
  pipeline rebuild — completes the half-done `--params` plumbing).
- Inputs: keyboard/hotkeys → OSC (UDP) → MIDI (RtMidi). A mapping config
  (control → named param, with scale/curve).

**Exit:** a MIDI knob moves a shader param live with no hitch.

### R5 — Audio reactivity *(M)*

- Audio capture (miniaudio / PortAudio, device + loopback).
- Analysis thread: RMS level, FFT bands, onset detection, tempo estimate.
- Expose reactive signals (`audio_level`, `audio_bass/mid/treble`,
  `beat_phase`, `bpm`) into the param table; mapping config.

**Exit:** the `beat-*` / `motion-*` effects drive off real sound.

### R6 — Zero-copy + hardware decode *(L; build-roadmap Phase 9)*

- Hardware decode via libavcodec hwaccel into Vulkan/NV12 GPU frames; import as
  Vulkan images; on-GPU YUV→RGB prelude (BT.601/709/2020 + range) emitted at
  compile time. Add a hw-encode stream-out sink.

**Exit:** 4K60 live source with no CPU `sws` and no host upload. **Top risk** —
interop is per-platform (Windows NVDEC/D3D11, Linux VAAPI/drm, macOS
VideoToolbox + MoltenVK); v1 may scope to one platform/vendor.

### R7 — Dynamic-stream robustness *(M)*

- Source disconnect/reconnect, mid-stream resolution/format changes (rebuild
  framebuffers + swapchain live), VFR, backpressure, graceful degradation.
- Long-run soak tests.

**Exit:** survives a source that drops, resizes, and reconnects without a
restart.

### R8 — Hardening, perf sweep, docs *(M)*

- Hit latency/throughput targets; profile out stalls.
- Cross-platform source/sink backends (Windows primary).
- Document the streaming API.

## Deferred (not in v1)

- Multiple simultaneous outputs (preview + stream-out at once; output routing
  layer). Designed for in the sink interface, not implemented.

## Critical path & parallelism

- Critical path: **R0 → R1 → R2 → R3**.
- Once R2 lands, **R4 / R5 / R6** are largely independent tracks.
- Throughout, the **offline parity suite must stay green** — batch mode is the
  safety net proving the refactor did not change pixels.
