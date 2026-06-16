---
title: slangfx
---

# slangfx

Apply libretro-format **slang shaders** — the multi-pass GPU shader format
used by RetroArch's vulkan / glcore drivers, including the entire
[libretro/slang-shaders](https://github.com/libretro/slang-shaders) corpus
(CRT, scanlines, NTSC/VHS, scalers, AA, …) — to any video file.

```bash
python wrappers/slangfx.py \
  -i input.mp4 \
  --preset crt/newpixie-crt.slangp \
  -o output.mp4
```

## Documentation

- **[Install]({{ '/install.html' | relative_url }})** — Vulkan SDK, ffmpeg, build from source on Windows / Linux / macOS.
- **[Usage]({{ '/usage.html' | relative_url }})** — `slangfx` CLI, the Python wrapper, low-level pipe orchestration.
- **[Tested shaders]({{ '/shaders.html' | relative_url }})** — what's known to work end-to-end, plus tuning notes.
- **[Bundled effects]({{ '/effects.html' | relative_url }})** — slangfx's own effect library (`shaders/`): colour, edges, blur/bloom, motion, glitch.
- **[Architecture]({{ '/architecture.html' | relative_url }})** — how the pieces fit together inside the binary.
- **[Roadmap]({{ '/roadmap.html' | relative_url }})** — what's done, what's deferred, what's coming.
- **[Slang format reference]({{ '/slang_format.html' | relative_url }})** — quick reference for the shader format we consume.
- **[Limitations]({{ '/limitations.html' | relative_url }})** — known gaps and workarounds.

## At a glance

slangfx reads raw RGBA frames on stdin, applies a multi-pass shader chain
on the GPU via Vulkan, writes processed frames to stdout. ffmpeg handles
demux/decode and encode/mux through pipes, so audio passthrough and
container muxing are free.

```
ffmpeg (decode + sws RGBA)  →  pipe  →
   slangfx (Vulkan multi-pass)  →  pipe  →
ffmpeg (encode + mux + audio passthrough)
```

True streaming, constant memory (~30–50 MB working set), three processes
pipelining across cores. See [Architecture]({{ '/architecture.html' |
relative_url }}) for details.

## Why not a libavfilter filter?

ffmpeg has no stable plugin ABI, so an out-of-tree filter that links
`libavfilter` privately isn't really feasible — the only paths to a real
`vf_slang` are forking ffmpeg or upstreaming. The standalone binary plus
pipe glue gives the same end-user experience without forcing either.

The libavfilter wrapper is on the [roadmap]({{ '/roadmap.html' |
relative_url }}) (Phase 10) but it's a packaging task, not a feature: the
shader pipeline core (parser, compiler, dispatcher, reflection,
multi-pass, feedback rings) is already complete and would be reused
verbatim.

## Source

[github.com/SteveCastle/slangfx](https://github.com/SteveCastle/slangfx)

[MIT licensed]({{ '/LICENSE' | relative_url }}). The libretro slang
shaders themselves carry their own licenses (most are GPL or public
domain); slangfx loads and runs them but does not redistribute their
source.
