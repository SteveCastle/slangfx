---
title: Tested shaders
---

# Tested shaders

A non-exhaustive list of shaders from
[libretro/slang-shaders](https://github.com/libretro/slang-shaders) that
have been verified to run end-to-end. Most shaders work without
modification; presets that depend on features not yet implemented are
called out under [Limitations]({{ '/limitations.html' | relative_url }}).

## Verified working

### CRT

| Preset | Notes |
|---|---|
| `crt/newpixie-crt.slangp` | 4 passes. PassFeedback ghosting, scanlines, curvature, shadow mask, vignette, RGB phosphor bleed, noise, flicker. Heavy but produces "modern emulator CRT" look. |
| `crt/scanlines.slangp` | Single-pass scanline overlay. Cheap. |
| `crt/crt-easymode.slangp` | Mid-complexity CRT — scanlines + minor curvature + brightness compensation. |
| `crt/crt-easymode-halation.slangp` | Adds halation. PassFeedback. |
| `crt/crt-lottes.slangp` | Lottes' classic CRT model. |
| `crt/crt-geom*.slangp` | Geometric distortion family. |

### VHS / NTSC

| Preset | Notes |
|---|---|
| `vhs/ntsc-vcr.slangp` | 5 passes. Full NTSC composite encode/decode chain (4× horizontal upscale + float framebuffer for the carrier signal) plus VHS tape grain and scrolling tracking lines. |
| `ntsc/ntsc-blargg.slangp` | Blargg's NTSC encode-only filter. |

### Color grading

| Preset | Notes |
|---|---|
| `misc/image-adjustment.slangp` | gamma / saturation / contrast / luminance / overscan / per-channel gain / zoom / pan / per-edge mask / film grain / sharpen. Single pass. |

### Anti-aliasing / smoothing

| Preset | Notes |
|---|---|
| `anti-aliasing/fxaa.slangp` | FXAA. |
| `anti-aliasing/smaa.slangp` | SMAA. Multi-pass. |

### Sharpening

| Preset | Notes |
|---|---|
| `sharpen/sharpen.slangp` | Simple sharpening. |
| `sharpen/anime4k.slangp` | Anime upscale. |

### Pixel art scaling

| Preset | Notes |
|---|---|
| `pixel-art-scaling/scalefx*.slangp` | ScaleFX family. |
| `pixel-art-scaling/xbrz/*.slangp` | xBRZ. |

## Tuning notes per preset

### `crt/newpixie-crt.slangp`

Default parameters produce a moderately strong CRT effect at any input
resolution. Tunables:

```
parameters = "curvature;vignette;ghosting;wiggle_toggle;scanroll"
curvature     = 1.0    # 0.0 = flat, 4.0 = strongly curved
vignette      = 0.7    # 0.0 = off, 1.0 = full
ghosting      = 1.5    # 0.0 = off; multi-frame trail intensity
wiggle_toggle = 1.0    # signal interference
scanroll      = 1.0    # scanline rolling
```

### `vhs/ntsc-vcr.slangp`

```
parameters = "GRAIN_STR;saturation;target_gamma;luminance;bright_boost"
GRAIN_STR    = 6.0     # tape grain magnitude
saturation   = 0.7     # NTSC color desaturation
target_gamma = 3.5     # bumps contrast — 2.2 is "neutral"
luminance    = 1.10
bright_boost = 0.10
```

The `play.png` overlay (the OSD play-icon scrolling thing) is currently
not loaded since external textures are deferred work. The shader runs
fine without it; `use_frame=0` default suppresses the play-button overlay.

### `misc/image-adjustment.slangp`

Useful as the last pass in a custom chain (color-grade after a CRT
effect). Many parameters; common ones:

```
parameters = "ia_target_gamma;ia_saturation;ia_contrast;ia_luminance;ia_GRAIN_STR"
ia_target_gamma = 2.2
ia_saturation   = 1.0
ia_contrast     = 1.0
ia_luminance    = 1.0
ia_GRAIN_STR    = 0.0   # off; raise for film grain
```

## Reporting a shader that doesn't work

Run with `SLANGFX_DEBUG_REFLECT=1 SLANGFX_DEBUG_FORMAT=1` to capture the
reflection / format diagnostic. Open an issue including:

- The `.slangp` path (and whether you modified the preset)
- The diagnostic output
- One sample frame of input and output (zip is fine)
- What you expected to see

Most failures fall into one of:

- Missing external texture (a `textures = "..."` declaration). Phase 8
  work; usually the shader runs but the texture sampler reads zeros.
- Compute-shader pass (`#pragma stage compute`). Not implemented.
- A specific Vulkan format that needs additional support.

The pipeline is usable for the vast majority of slang shaders today;
unsupported features are documented in [Limitations]({{ '/limitations.html'
| relative_url }}).
