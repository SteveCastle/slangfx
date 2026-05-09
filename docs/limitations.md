---
title: Limitations
---

# Limitations

What slangfx doesn't do yet, and how to work around it.

## External textures (`textures = "..."` in `.slangp`)

**Status:** deferred (Phase 8).

Some shaders reference image files alongside the preset — typically
shadow-mask LUTs, dust/grime overlays, OSD icons. The `textures` block in
the preset declares them:

```ini
textures = "MaskTexture;DustTexture"
MaskTexture        = textures/mask.png
MaskTexture_linear = true
DustTexture        = textures/dust.png
```

slangfx parses the declarations but currently doesn't load the images.
Bound samplers read zeros instead, so any visual contribution from those
textures is missing.

**Workaround:** most affected shaders still produce a sensible-looking
output — the LUT contribution is usually multiplicative or additive on
top of an already-correct image. If a particular preset goes black or
visibly wrong, fork the `.slangp`, comment out the `textures = ...`
block, and patch the shader to default the missing sampler to white
(`vec4(1.0)`) or zero. For `vhs/ntsc-vcr.slangp` specifically, the
default `use_frame=0` parameter already disables the only external
texture.

## Compute shader passes (`#pragma stage compute`)

**Status:** not implemented.

Slang format allows passes to declare themselves as compute kernels
instead of vertex/fragment pairs. slangfx is fragment-only.

**Workaround:** none in slangfx. None of the popular CRT / NTSC / VHS /
scaler shaders use compute stages — they're a niche feature. If you hit
one, the slang preset will fail to load with a clear error.

## YUV input / zero-copy

**Status:** deferred (Phase 9).

Today, ffmpeg decodes any source format and converts to RGBA8 via
`-pix_fmt rgba` before piping to slangfx. That's a forced sws colorspace
conversion on every frame.

For 4K@60 footage this CPU-side conversion can become the bottleneck
before shader cost does. A future YUV-direct path would import frames
straight into Vulkan via `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` and skip
sws entirely.

**Workaround for high-resolution real-time use:**

- Drop to a lower processing resolution. Many CRT / NTSC shaders are
  designed for ≤1080p anyway.
- Use the Python wrapper's `--vcodec h264_nvenc` / `h264_qsv` /
  `h264_videotoolbox` to keep the encode side off CPU.
- For offline (non-realtime) work this isn't a problem — sws keeps up
  with most encoders.

## libavfilter integration (`-vf slangfx`)

**Status:** roadmap (Phase 10).

Today the binary runs as a separate process talking via pipes. There's
no way to use slangfx as a node inside an ffmpeg `-filter_complex` graph.

**Workaround:** chain ffmpeg → slangfx → ffmpeg explicitly via pipes.
Anything you'd normally do in `-filter_complex` you can do in either of
the two ffmpeg processes:

```bash
# Crop, scale, then shader, then overlay text
ffmpeg -i in.mp4 -vf "crop=...,scale=1280:720" -f rawvideo -pix_fmt rgba - \
  | slangfx --preset preset.slangp --width 1280 --height 720 \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -framerate 30 -i - \
          -vf "drawtext=..." -c:v libx264 out.mp4
```

The wrapper script can grow first-class flags for these once a clear
common-case pattern emerges.

## Parameter override at the CLI

**Status:** partial — parser exists, application TBD.

`--params 'curvature=2,vignette=0.5'` is accepted on the command line
but currently the values aren't propagated into the runtime parameter
table; you have to set `parameters = ...` plus `name = value` lines
inside the `.slangp` itself.

**Workaround:** copy the `.slangp` to a working file, edit it in place.

## What the pipeline absolutely won't do

To set expectations clearly:

- **No frame interpolation / temporal scaling.** It's 1 frame in,
  1 frame out. PassFeedback gives shaders access to history, but the
  output frame rate is identical to input.
- **No audio processing.** Audio is passed through ffmpeg untouched.
- **No de-interlacing.** Pre-process with `-vf yadif` or similar
  beforehand.
- **No HDR.** Working colorspace is sRGB RGBA8 (or float framebuffers
  internally where shaders request them, but the I/O is 8-bit).

## Reporting a missing capability

[GitHub issues](https://github.com/SteveCastle/slangfx/issues) — include
the preset path, what behavior you expected vs got, and any
`SLANGFX_DEBUG_REFLECT=1` / `SLANGFX_DEBUG_FORMAT=1` output.
