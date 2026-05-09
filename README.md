# vf-slang

Native ffmpeg video filter that runs libretro-format **slang** shaders — the
shader format used by RetroArch's vulkan/glcore drivers — directly on video
frames, without going through libplacebo's user-shader format.

```
ffmpeg -i in.mp4 -vf "slang=preset=crt/newpixie-crt.slangp" out.mp4
```

## Why

libplacebo's `custom_shader_path` (the `mpv-format` user-shader system that
ffmpeg's `libplacebo` filter exposes) cannot represent the full slang shader
contract. In particular, the libplacebo user-shader format does not support:

- Multi-frame `PassFeedback` (compounding accumulator over many frames). It
  does support `PREV1` — but only in narrow contexts; binding `PREV1` in a
  pass that has any chained downstream `//!SAVE` consumer silently disables
  the OUTPUT pass, and binding it in `HOOK OUTPUT` silently skips that pass
  entirely. Confirmed reproducible on libplacebo v7.362.0 / ffmpeg
  N-124419-gc8a4770599.
- Runtime-allocated `STORAGE` textures for arbitrary persistent state. The
  `//!TEXTURE` directive is parsed but only handles static lookup data with
  hex bytes inline.
- External image textures (e.g. `frametexture = path.png` for CRT bezel art).
- Per-pass `wrap_mode`, `mipmap_input`, `frame_count_mod`, alias chains
  through `Pass<n>`, `OriginalHistory<n>`, etc.

This means thousands of well-tested slang shaders in
[libretro/slang-shaders](https://github.com/libretro/slang-shaders) cannot be
applied to non-emulator video sources without manual porting. `vf-slang`
loads slang `.slangp` presets directly and produces output identical to
RetroArch's vulkan driver.

## Status

Pre-alpha. The repo currently contains the architecture + roadmap docs and
empty stubs for each component. The first milestone is a single-shader
proof of concept: load one `.slang` file, compile to SPIR-V, apply it to one
video frame through Vulkan, write the result. From there we add the preset
parser, multi-pass chaining, history textures, and feedback buffers.

See [`docs/roadmap.md`](docs/roadmap.md) for phased work.

## Quick start (when it builds)

```bash
meson setup build
meson compile -C build
# Make ffmpeg find the filter (linux):
LD_LIBRARY_PATH=$PWD/build ffmpeg -filters | grep slang
```

## Architecture

See [`docs/architecture.md`](docs/architecture.md) for the component diagram
and per-frame data flow. tl;dr:

```
.slangp ──► parser ──► AST ──► glslang/shaderc ──► SPIR-V
                                                      │
                                                      ▼
input frame ──► Vulkan upload ──► multi-pass pipeline ──► readback ──► output frame
                                       │
                                       ├── current frame textures
                                       ├── pass output framebuffers (chained by alias)
                                       ├── feedback ring buffers (PassFeedback*)
                                       └── frame history (OriginalHistory*)
```

## Building

Requires:
- Vulkan SDK 1.3+ (loader + headers)
- `shaderc` (for runtime GLSL → SPIR-V compilation; ships with Vulkan SDK)
- `libavfilter` development headers (ffmpeg 6.0+)
- `meson` ≥ 0.60 + `ninja`

Platform support: Linux, Windows, macOS (via MoltenVK). Initial development
on Windows; CI for the other two added before v0.1.

## License

MIT. See [`LICENSE`](LICENSE).

## Related

- [libretro/slang-shaders](https://github.com/libretro/slang-shaders) —
  source format we consume.
- [libretro/glslang](https://github.com/libretro/glslang) — the reference
  slang preprocessor / compiler used by RetroArch.
- [libplacebo](https://code.videolan.org/videolan/libplacebo) — what ffmpeg
  uses today; this filter is the slang-native alternative.
