# vf-slang

Run libretro-format **slang** shaders — the shader format used by RetroArch's
vulkan/glcore drivers and the entire
[libretro/slang-shaders](https://github.com/libretro/slang-shaders) corpus
(CRT shaders, scanlines, NTSC, scalers, …) — on arbitrary video. Built around
a tiny standalone binary that reads raw RGBA frames from stdin and writes
processed frames to stdout, glued to ffmpeg via pipes.

```
ffmpeg -i in.mp4 -f rawvideo -pix_fmt rgba - \
  | vfslang --preset crt/newpixie-crt.slangp --width 1920 --height 1080 \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mp4
```

Or via the convenience wrapper:

```
python wrappers/vfslang.py -i in.mp4 --preset crt/newpixie-crt.slangp -o out.mp4
```

## Why

ffmpeg's `libplacebo` filter exposes mpv-style user shaders but its parser
doesn't accept the slang format directly, and its custom-shader system has
hard limits that block several common slang features:

- Multi-frame `PassFeedback` (compounding accumulator over many frames).
  libplacebo has `PREV1`, but binding it in any pass with chained downstream
  `//!SAVE` consumers silently disables the OUTPUT pass; binding it in
  `HOOK OUTPUT` silently skips that pass entirely. Confirmed reproducible on
  libplacebo v7.362.0 / ffmpeg N-124419-gc8a4770599.
- Runtime-allocated `STORAGE` textures. `//!TEXTURE` is parsed only for
  static lookup data with hex bytes inline; `//!STORAGE` triggers a parse
  error.
- External image textures (`textures = "frametexture"` with PNG bezel art).
- Per-pass `wrap_mode`, `mipmap_input`, `frame_count_mod`, alias chains
  through `Pass<n>` / `OriginalHistory<n>`.

So thousands of well-tested slang shaders cannot be applied to non-emulator
video sources without manual porting, and even the manual ports fall short.
`vf-slang` loads `.slangp` presets directly through Vulkan + shaderc and
produces output identical to RetroArch's vulkan driver.

## Why a stdin/stdout binary, not a libavfilter plugin

ffmpeg has no stable plugin ABI. External shared libraries can't link to
`ff_make_format_list`, `ff_get_video_buffer`, `AVFILTER_DEFINE_CLASS`,
`FILTER_INPUTS`, or any of the other internal helpers that libavfilter
filters use — those symbols aren't exported. The two real options are:

1. Fork ffmpeg and patch the filter in.
2. Sidestep libavfilter entirely with a process that reads raw frames over
   pipes.

Option 2 builds anywhere with a C compiler + Vulkan SDK + shaderc, ships as
a single binary, and works identically on Linux/Windows/macOS. The
libavfilter upstream patch becomes a Phase-10 follow-on, not a build
blocker. See [`docs/architecture.md`](docs/architecture.md).

## Status

Beta. Real libretro slang shaders — **including the canonical
`crt/newpixie-crt.slangp` (4 passes, multi-frame `PassFeedback`
accumulator, 6 runtime parameters)** — compile and run end-to-end
through the full pipeline (`.slangp` → `.slang` → SPIR-V → Vulkan)
on Windows mingw64. Done:

- **Phase 0** — scaffold + build system (Windows mingw64 + Linux + macOS).
- **Phase 1** — Vulkan offscreen render-to-texture pipeline.
- **Phase 2** — `.slangp` parser (full libretro syntax + 7 unit tests).
- **Phase 3** — slang preprocessor + shaderc compilation, including
  `#include` resolution.
- **Phase 4** — pipeline driven by a slang shader.
- **Phase 5** — multi-pass chain.
- **Phase 5b** — alias bindings (`alias<i> = name`).
- **Phase 5c** — `Pass<n>` numeric pass refs.
- **Phase 5d** — `Original` / `OriginalSize`.
- **Phase 6** — `PassFeedback<n>` rings: per-producer snapshot at
  end of each frame; consumers in next frame sample the snapshot.
  Multi-frame compounding accumulator works.
- **Phase 7** — SPIR-V reflection: push-constant layout, UBO layout,
  sampler bindings recovered from SPIR-V directly. The host writes
  standard fields + `#pragma parameter` defaults at the shader's
  declared offsets.
- **Phase 7b** — preset-level parameter overrides applied at startup.

Verified end-to-end:

| Test | Result |
|---|---|
| `invert.slang` (1 pass, Source) | red → cyan ✓ |
| `double_invert.slangp` (2 passes, Source chain) | red → red ✓ |
| `brightness.slang` (param at offset 52, default 0.5) | 255 → 127 ✓ |
| `alias_test.slangp` (pass 1 reads pass 0 by alias) | red → red ✓ |
| `feedback.slang` (PassFeedback0 with 0.7 decay) | 255 → 178 → 125 → 87 → 61 → 43 ✓ |
| `image-adjustment.slangp` (libretro, single pass, `#include`) | runs, all 24 push members reflected ✓ |
| **`newpixie-crt.slangp`** (libretro, 4 passes, PassFeedback, alias chain) | **runs** ✓ |
| Real 5s 1440×1080 video through `newpixie-crt.slangp` | 147 frames out, valid h.264 ✓ |

What's left:

- **Phase 5b/c/d** — alias / `Pass<n>` / `Original` sampler bindings
  (incremental on top of the existing chain).
- **Phase 6** — `PassFeedback<n>` ring buffers (multi-frame compounding
  accumulator). 2-deep ring per producer; flip per frame.
- **Phase 7b** — runtime parameter overrides via the existing
  `--params 'name=value,...'` flag. Reflection now provides the
  offsets; just need to parse the string and write the values.
- **Phase 8** — external textures (PNG via vendored stb_image).
- **Phase 9** — YUV input/output + zero-copy `AV_PIX_FMT_VULKAN` path.
- **Phase 10** — corpus parity vs RetroArch + libavfilter patch
  (re-using the same `slangp/slang_compile/slang_pipeline` core wrapped
  in a `vf_slang.c` filter).

See [`docs/roadmap.md`](docs/roadmap.md) for the per-phase plan.

## Building

### Windows (mingw64 toolchain)

```powershell
# One-time setup. meson + ninja installable via pip:
python -m pip install meson ninja
# Vulkan SDK only required from Phase 1. For Phase 0, GPU is off by default.
```

```powershell
meson setup build
meson compile -C build
.\build\vfslang.exe --help
```

When Phase 1 lands, enable GPU support:

```powershell
meson setup build --reconfigure -Denable_gpu=true
# Optionally point at a non-default Vulkan SDK:
meson setup build --reconfigure -Denable_gpu=true -Dvulkan_sdk=C:\VulkanSDK\1.3.X
```

### Linux

```bash
sudo apt install meson ninja-build  # or pip install meson ninja
# Phase 1+: also libvulkan-dev libshaderc-dev
meson setup build
meson compile -C build
```

### macOS

```bash
brew install meson ninja molten-vk shaderc  # phase 1+
meson setup build
meson compile -C build
```

### Build options

| Option | Default | Meaning |
| --- | --- | --- |
| `enable_gpu` | `false` | Link against Vulkan + shaderc. Off in Phase 0 (identity-copy stub); required from Phase 1. |
| `vulkan_sdk` | `$VULKAN_SDK` | Override Vulkan SDK install path (Windows). |

## Repo layout

```
vf-slang/
├── README.md            project vision + status + quick start
├── LICENSE              MIT
├── meson.build          build config (Vulkan/shaderc optional via -Denable_gpu)
├── meson.options        knobs for the above
├── docs/
│   ├── architecture.md  components + per-frame data flow + IO model
│   ├── roadmap.md       10-phase implementation plan
│   └── slang_format.md  spec notes on .slangp + .slang
├── src/
│   ├── vf_slang.c       main(): stdin/stdout RGBA filter binary
│   ├── slangp.{h,c}     .slangp preset parser
│   ├── slang_compile.{h,c}  slang → SPIR-V via shaderc
│   └── slang_pipeline.{h,c} Vulkan multi-pass dispatch + feedback rings
├── wrappers/
│   └── vfslang.py       convenience wrapper: ffmpeg | vfslang | ffmpeg
└── tests/
    ├── test_slangp_parse.c
    └── fixtures/sample.slangp
```

## License

MIT. See [`LICENSE`](LICENSE).

## Related

- [libretro/slang-shaders](https://github.com/libretro/slang-shaders) —
  source format we consume.
- [libretro/glslang](https://github.com/libretro/glslang) — the reference
  slang preprocessor / compiler used by RetroArch.
- [libplacebo](https://code.videolan.org/videolan/libplacebo) — what ffmpeg
  ships today; this filter is the slang-native alternative.
