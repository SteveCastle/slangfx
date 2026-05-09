---
title: Slang shader format reference
---

# Slang shader format (libretro flavor)

Reference notes for the format we consume. The canonical implementation is
in [libretro/glslang](https://github.com/libretro/glslang) and the consumer
is RetroArch's vulkan / glcore drivers. This doc summarizes what `slangfx`
needs to support.

## File extensions

- `.slang` — single shader source
- `.slangp` — preset describing a multi-pass chain
- `.inc` / `.h` — included headers (text concatenation via `#include`)
- companion `.png` / `.jpg` — referenced by the `textures = ` line in a preset

## `.slangp` (preset) syntax

INI-flavored, key-value, one entry per line. Comments start with `#`.
Required keys at the top:

```
shaders = N
```

Then for each pass `i` in `[0, N)`:

```
shader<i>             = <relative path to .slang file>     (required)
alias<i>              = <name>                             (optional)
filter_linear<i>      = true | false                       (default false)
mipmap_input<i>       = true | false                       (default false)
wrap_mode<i>          = clamp_to_border | clamp_to_edge |
                        repeat | mirrored_repeat           (default clamp_to_border)
frame_count_mod<i>    = <integer>                          (default 0 = no mod)
srgb_framebuffer<i>   = true | false                       (default false)
float_framebuffer<i>  = true | false                       (default false)
fbo_format<i>         = R8_UNORM | R8G8_UNORM | R8G8B8A8_UNORM |
                        R10G10B10A2_UNORM | R16_UNORM | R16_SFLOAT |
                        R16G16B16A16_UNORM | R16G16B16A16_SFLOAT |
                        R32_SFLOAT | R32G32B32A32_SFLOAT
                        (overrides srgb_framebuffer / float_framebuffer)

# Output sizing — picks one of three modes per axis:
scale_type<i>         = source | viewport | absolute       (uniform x+y)
scale_type_x<i>       = source | viewport | absolute       (per axis)
scale_type_y<i>       = source | viewport | absolute
scale<i>              = <float>                            (uniform x+y)
scale_x<i>            = <float>                            (per axis)
scale_y<i>            = <float>                            (per axis)
```

`source` scales by `scale_x` * (previous pass's output width). `viewport`
scales by `scale_x` * (final output width). `absolute` makes
`scale_x` the literal pixel count.

Top-level texture and parameter blocks:

```
textures = "name1;name2;name3"
name1     = <path to image>
name1_linear    = true
name1_mipmap    = false
name1_wrap_mode = clamp_to_border

parameters = "knob1;knob2"
knob1     = 0.65    # override default declared in #pragma parameter
```

## `.slang` source layout

Always starts with `#version 450`. Vulkan-flavored GLSL with libretro
conventions.

### Stages

A single file holds *both* vertex and fragment stages, separated by
pragmas:

```glsl
#version 450

// (uniforms / push_constant blocks — visible to both stages)

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main() {
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

void main() {
    FragColor = texture(Source, vTexCoord);
}
```

The slang preprocessor splits on `#pragma stage <name>` and produces two
GLSL source strings, one per Vulkan stage, then each is compiled to
SPIR-V independently.

### Push constants

Per-pass constants live in a single `Push` block:

```glsl
layout(push_constant) uniform Push {
    vec4  SourceSize;     // (width, height, 1/width, 1/height)
    vec4  OriginalSize;   // same, for the original input image
    vec4  OutputSize;     // same, for this pass's output framebuffer
    uint  FrameCount;     // monotonic frame index, mod-applied per pass
    // ... custom params declared via #pragma parameter ...
    float curvature;
    float vignette;
} params;
```

The host populates these per pass, per frame. The standard fields
(SourceSize / OriginalSize / OutputSize / FrameCount / FrameDirection /
Rotation) are always available; custom fields appear only if declared.

### Global UBO

```glsl
layout(std140, set = 0, binding = 0) uniform UBO {
    mat4 MVP;
    // optional standard fields...
    // optional custom params...
} global;
```

In practice, libretro shaders nearly always have just `MVP` here. We
populate it with an identity / orthographic projection that maps the
fullscreen triangle to NDC.

### Sampler bindings

Set 0, binding ≥ 2 (binding 1 is reserved for some configurations).
Special sampler names the host recognizes:

| Sampler              | Bound to                                          |
| -------------------- | ------------------------------------------------- |
| `Source`             | Output of the previous pass (or original input    |
|                      | for pass 0).                                      |
| `Original`           | Original (pre-shader) input frame.                |
| `OriginalHistory<N>` | Original input from N frames ago (N >= 0;         |
|                      | `OriginalHistory0` == `Original`).                |
| `Pass<n>`            | Output of pass `n` (numeric, this frame).         |
| `<alias>`            | Output of the pass that declared `alias<i> = <name>`. |
| `PassFeedback<n>`    | Output of pass `n` from the *previous* frame.     |
| `<alias>Feedback`    | Same as `PassFeedback<n>` but using the alias.    |
| `<texture-name>`     | An external image listed in the `textures = ...`  |
|                      | line of the preset.                               |

Each sampler also has a corresponding `<sampler>Size` push-constant or
UBO field with `(w, h, 1/w, 1/h)`.

### Parameter pragmas

```
#pragma parameter <name> "<description>" <default> <min> <max> <step>
```

Each declared parameter becomes a push-constant float in the `Push` block.
`slangfx` exposes them as a `--params 'key=value,...'` CLI option so users
can override defaults from the command line.

### Format pragma

```
#pragma format R8G8B8A8_UNORM
```

Overrides the framebuffer format for this pass (alternative to
`fbo_format<i>` in the preset).

## What we lose by going slang-native

Nothing slang-related. We gain everything libplacebo's user-shader format
loses:
- True multi-frame `PassFeedback` (compounding accumulator).
- External texture bindings (CRT bezels, look-up tables).
- Per-pass `wrap_mode`, `mipmap_input`, framebuffer format choice.
- The `OriginalHistory<N>` ring for shaders that need older input frames.
- The full corpus at https://github.com/libretro/slang-shaders works
  unmodified.

## References

- [libretro/glslang](https://github.com/libretro/glslang) — slang
  preprocessor we link against (or port the relevant subset of).
- [libretro/RetroArch — vulkan_slang.cpp](https://github.com/libretro/RetroArch/blob/master/gfx/drivers_shader/glslang_util.cpp)
  — reference consumer.
- [libretro/slang-shaders](https://github.com/libretro/slang-shaders) —
  corpus we test against.
