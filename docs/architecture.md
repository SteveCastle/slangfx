# Architecture

Three layers, plus the ffmpeg integration shell.

```
┌────────────────────────────────────────────────────────────────────────┐
│  ffmpeg (libavfilter host)                                             │
│                                                                        │
│   ┌──────────────────────────────────────────────────────────────┐    │
│   │  vf_slang.c    AVFilter registration + frame in/out          │    │
│   └────────────────────┬─────────────────────────────────────────┘    │
│                        │                                              │
└────────────────────────┼──────────────────────────────────────────────┘
                         │
            ┌────────────┴───────────┐
            │ slang_pipeline (Vulkan │
            │ device, descriptor &   │
            │ render-pass plumbing)  │
            └────────────┬───────────┘
                         │
                ┌────────┴────────┐
                │                 │
        ┌───────┴──────┐   ┌──────┴────────┐
        │  slangp.c    │   │ slang_compile │
        │  preset      │   │ (slang -> SPV │
        │  parser      │   │  via shaderc) │
        └──────────────┘   └───────────────┘
```

## Components

### `slangp.c` — preset parser

Parses libretro `.slangp` INI-format presets. Output: a `slangp_preset`
struct holding an array of `slangp_pass` (one per shader stage), the textures
table, and runtime parameter overrides.

Key fields per pass:
- `path` — file path to the `.slang` shader (relative paths resolved
  against the preset directory)
- `alias` — optional name used to bind this pass's output as a sampler in
  later passes (e.g. `accum1`)
- `scale_type_x/y` — `source` / `viewport` / `absolute` — controls the
  framebuffer dimensions of this pass's output
- `scale_x/y` — multiplier applied to the chosen scale type
- `filter_linear` — bilinear vs nearest sampling on this pass's output
- `mipmap_input` — whether downstream samplers see mipmaps
- `wrap_mode` — `clamp_to_border` / `clamp_to_edge` / `repeat` / `mirrored_repeat`
- `frame_count_mod` — applied to the FrameCount push constant exposed to
  the shader
- `fbo_format` — pixel format for this pass's framebuffer
  (`R8G8B8A8_UNORM`, `R16G16B16A16_SFLOAT`, etc.)

Top-level fields:
- `textures` — list of `(name, path, filter_linear, mipmap, wrap_mode)`
  for external image bindings (CRT bezel art, look-up tables, etc.)
- `parameters` — overrides for `#pragma parameter` declared values

Parser is hand-written; the format is small enough to not warrant pulling
in an INI library.

### `slang_compile.c` — slang → SPIR-V

Runs each `.slang` file through libretro's slang preprocessor (or our own
re-implementation of its preprocessing rules) to produce two GLSL strings,
one per shader stage (`#pragma stage vertex` / `#pragma stage fragment`).
Each is then compiled to SPIR-V by `shaderc` (libshaderc is bundled with
the Vulkan SDK and most distros).

Output per shader: `slang_module`
- `vert_spirv`, `frag_spirv` — bytecode arrays
- `push_constant_layout` — extracted `Push` block (offset/size of each field)
- `ubo_layout` — extracted `UBO` block (set 0, binding 0)
- `samplers[]` — declared `sampler2D` references (Source, Original,
  PassFeedback*, OriginalHistory*, Pass<n>, alias names, declared textures)
- `parameters[]` — declared `#pragma parameter` knobs (name, default, min,
  max, step)

Notes on the slang dialect (see `docs/slang_format.md` for the full spec
notes):
- Always `#version 450`
- Uses `layout(push_constant) uniform Push { ... }` for per-pass constants
- `layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; ... }`
  for the global uniform block
- Texture samplers at `set = 0, binding = N` for N >= 2 (binding 1 is
  conventionally reserved)

### `slang_pipeline.c` — Vulkan dispatch

Owns the Vulkan device, descriptor pools, render passes, and per-pass
graphics pipelines. Handles framebuffer allocation per pass based on
parsed scale rules. Maintains:
- **Pass output cache** — current-frame outputs, chained by alias
- **Feedback ring buffers** — one per pass that has its output read as
  `PassFeedback<n>` somewhere downstream; ring of 2 textures, swapped each
  frame so frame N can read frame N-1's pass output
- **Original history ring** — input frame history (`OriginalHistory0..N`)
  exposed to shaders that ask for it

Per-frame loop:
1. Upload incoming AVFrame to the input texture (`Source` for pass 0).
2. Update `FrameCount` push constant (multiplied by per-pass
   `frame_count_mod`).
3. For each pass in order:
   a. Bind input samplers — `Source` (= previous pass output or original
      input), `Original` (= input frame), aliases, history, feedback,
      external textures.
   b. Update push constants with `SourceSize`, `OriginalSize`, `OutputSize`,
      `FrameCount`, and any parameter values.
   c. Draw a fullscreen triangle into this pass's framebuffer.
4. Read back the last pass's output to a host AVFrame.
5. Advance feedback rings (this pass's current output becomes next frame's
   `PassFeedback<n>`), advance original history.

### `vf_slang.c` — ffmpeg integration

Standard `AVFilter` boilerplate:
- `init()` — parse preset path option, allocate `slang_pipeline`.
- `query_formats()` — declare we accept RGB and YUV (color conversion
  handled internally; slang shaders run in RGB).
- `config_input()` — once we know the input dimensions, set up Vulkan
  framebuffers per pass.
- `filter_frame()` — run the pipeline on one AVFrame, push the result
  downstream.
- `uninit()` — tear down Vulkan resources.

Filter options:
- `preset` — path to `.slangp` file
- `params` — `key=value,key2=value2` overrides for `#pragma parameter`
  knobs
- `frame_history` — max history depth to keep (default: derived from
  shaders, capped at 8)

## Per-frame data flow

```
AVFrame (NV12 or RGB)
   │
   │ ① upload to input image
   ▼
[Source / Original = input texture]
   │
   │ ② pass 0 reads Source + (optional aliases / history / feedback)
   ▼
[pass 0 output framebuffer = alias 'accum1' if declared]
   │
   │ ③ pass 1 reads pass 0 output as 'Source', plus alias 'accum1'
   ▼
[pass 1 output framebuffer]
   │
   │ ... etc for each pass ...
   ▼
[final pass output]
   │
   │ ④ readback to AVFrame
   ▼
AVFrame out

After step 3 each frame:
  [pass N output] → [feedback ring slot for next frame's PassFeedback<N>]
  [Original]      → [history ring slot for next frame's OriginalHistory<0>]
```

## Memory model

- Vulkan resources allocated once per filter instance at `config_input`.
- Per-frame allocations: only the upload staging + readback staging
  buffers (re-used).
- Framebuffer textures use `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
  VK_IMAGE_USAGE_SAMPLED_BIT`.
- Feedback rings are flipped via descriptor set update (no copy).

## Testing strategy

Three layers:
1. **`tests/test_slangp_parse.c`** — parse fixtures from `tests/fixtures/`,
   verify struct contents.
2. **Single-pass shader against synthetic input** — feed a known image
   (gradient, color block) through a one-pass shader, verify output
   matches a CPU reference.
3. **Parity vs RetroArch** — compile select `slang-shaders/` presets
   through both this filter and RetroArch's `ffmpeg_libretro` core,
   diff the resulting frames. Threshold acceptance to within rounding
   error.
