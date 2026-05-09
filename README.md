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

Pre-alpha. Phase 0 is complete: the build system works on Windows + Linux,
the binary compiles and runs end-to-end (currently as an identity copy), and
the component interfaces (`slangp.h`, `slang_compile.h`, `slang_pipeline.h`)
are stubbed and documented. Phase 1 brings up the Vulkan device and replaces
the identity copy with a real GPU dispatch. See
[`docs/roadmap.md`](docs/roadmap.md) for the phased plan.

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
