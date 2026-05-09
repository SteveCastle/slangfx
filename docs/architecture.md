---
title: Architecture
---

# Architecture

## IO model

`slangfx` is a standalone process that reads raw RGBA8 frames from stdin
and writes processed frames to stdout. ffmpeg orchestrates everything
around it via pipes: decode → rawvideo → slangfx → rawvideo → encode.

```
┌─────────────┐  RGBA pipe  ┌─────────┐  RGBA pipe  ┌─────────────┐
│   ffmpeg    │ ──────────► │ slangfx │ ──────────► │   ffmpeg    │
│  (decode)   │             │ (GPU)   │             │  (encode)   │
└─────────────┘             └─────────┘             └─────────────┘
       ▲                                                   │
       │                                                   ▼
   in.mp4                                              out.mp4
```

This bypasses ffmpeg's missing plugin ABI: external libavfilter filters
cannot link the internal `ff_*` helpers, so we don't try. We only depend
on ffmpeg as a runtime executable that anyone already has. The libavfilter
patch for upstream submission becomes a Phase-10 packaging task, not a
build dependency.

`wrappers/slangfx.py` automates the orchestration: probes input dims,
spawns the three processes, passes audio through unchanged from the source
container.

## Internal components

```
┌──────────────────────────────────────────────────────────────────┐
│  src/main.c                stdin/stdout frame loop + arg parsing │
└────────────────────────────────┬─────────────────────────────────┘
                                 │
                  ┌──────────────┴──────────────┐
                  │ slang_pipeline (Vulkan      │
                  │ device, descriptor sets,    │
                  │ render passes, ring buffers)│
                  └──────────────┬──────────────┘
                                 │
                ┌────────────────┴────────────────┐
                │                                 │
        ┌───────┴────────┐               ┌────────┴────────┐
        │  slangp.c      │               │ slang_compile.c │
        │  preset parser │               │ slang→SPIR-V    │
        │                │               │ via shaderc     │
        └────────────────┘               └─────────────────┘
```

### `slangp.c` — preset parser

Parses libretro `.slangp` INI-format presets. Output: a `slangp_preset`
struct holding an array of `slangp_pass` (one per shader stage), the
external textures table, and runtime parameter overrides.

Per-pass fields: shader path, alias, `scale_type[_x|_y]`, `scale[_x|_y]`,
`filter_linear`, `mipmap_input`, `wrap_mode`, `frame_count_mod`,
`fbo_format`. Top-level: `textures = "name1;name2"` declarations and
`parameters = "knob1;knob2"` overrides.

The parser is hand-written; the format is small enough not to warrant an
INI library dependency.

### `slang_compile.c` — slang → SPIR-V

Runs each `.slang` file through libretro's slang preprocessor (port the
relevant ~600 lines of `libretro/glslang/glslang/glslang_util_cxx.cpp`)
to produce two GLSL strings, one per Vulkan stage (`#pragma stage vertex`
/ `#pragma stage fragment`). Each is compiled to SPIR-V by `shaderc`
(libshaderc ships with the Vulkan SDK).

Output per shader: `slang_module` containing the bytecode plus reflected
push-constant layout, UBO layout, sampler bindings (`Source`, `Original`,
`PassFeedback<N>`, `OriginalHistory<N>`, `Pass<n>`, declared aliases,
external textures), and parameter declarations.

### `slang_pipeline.c` — Vulkan dispatch

Owns the Vulkan device, descriptor pools, render passes, and per-pass
graphics pipelines. Maintains:

- **Pass output cache** — current-frame outputs, chained by alias.
- **Feedback ring buffers** — one per pass that has its output read as
  `PassFeedback<n>` somewhere downstream; ring of 2 textures, swapped
  each frame so frame N can read frame N-1's output.
- **Original history ring** — input frame history exposed as
  `OriginalHistory<0..N>`.

Per-frame loop:
1. Upload incoming RGBA bytes to the input texture (`Source` for pass 0).
2. Update `FrameCount` push constant (multiplied by `frame_count_mod`).
3. For each pass:
   - Bind input samplers — `Source`, `Original`, aliases, history,
     feedback, external textures.
   - Update push constants with `SourceSize`, `OriginalSize`,
     `OutputSize`, `FrameCount`, parameter values.
   - Draw a fullscreen triangle into this pass's framebuffer.
4. Read back the last pass's output to the host RGBA buffer for stdout.
5. Advance feedback rings (this pass's current output becomes next
   frame's `PassFeedback<n>`); rotate `OriginalHistory`.

### `main.c` — frame loop

Parses CLI args (`--preset`, `--width`, `--height`, optional `--params` /
`--frame-history`), opens the preset, builds the pipeline, then in a hot
loop:

```c
while (fread(frame_in, 1, frame_bytes, stdin) == frame_bytes) {
    slang_pipeline_run(pipeline, frame_in, frame_out);
    fwrite(frame_out, 1, frame_bytes, stdout);
}
```

Stdin/stdout are forced to binary mode on Windows so RGBA bytes aren't
mangled by CRLF translation.

## Memory model

- Vulkan resources allocated once at pipeline creation.
- Per-frame allocations: only the upload staging + readback staging
  buffers (re-used).
- Framebuffer textures use `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
  VK_IMAGE_USAGE_SAMPLED_BIT`.
- Feedback rings are flipped via descriptor-set updates (no copy).

## Testing strategy

Three layers:

1. **`tests/test_slangp_parse.c`** — parses fixtures from
   `tests/fixtures/`, asserts parsed struct contents match expected.
2. **Single-pass shader against synthetic input** — once GPU is on, feed
   a known image (gradient, color block) through a one-pass shader,
   diff against a CPU reference.
3. **Parity vs RetroArch** — compile select `slang-shaders/` presets
   through both this filter and RetroArch's `ffmpeg_libretro` core, diff
   resulting frames. Threshold acceptance to within rounding error.
