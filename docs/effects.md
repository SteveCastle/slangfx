---
title: Bundled effects
---

# Bundled effects

slangfx ships a library of original slang effects under [`shaders/`](https://github.com/SteveCastle/slangfx/tree/main/shaders).
These are written for this project (distinct from the external
[libretro/slang-shaders corpus]({{ '/shaders.html' | relative_url }})) and cover
colour grading, edge detection, blurs, blooms, motion-reactive feedback,
tempo-synced effects, and glitch/analog looks.

Each effect is a standard libretro `.slangp` preset in its own folder. Most
expose an **`amount`** parameter where **`amount = 0` is a clean passthrough**,
plus tunable `#pragma parameter` sliders. Multi-pass effects (bloom, tilt-shift,
motion-bloom, scrambled-crt, datamosh) keep their helper passes in the same
folder.

## Running an effect

```bash
# Batch-render a clip to a file (native res/fps, audio copied):
python wrappers/slangfx.py -i my_clip.mp4 \
  --preset shaders/bloom/bloom.slangp -o out.mp4

# Tune it live with sliders (and switch effects / export from the menus):
python wrappers/slangfx_live.py -i my_clip.mp4 --preset shaders/bloom/bloom.slangp

# Override any parameter (comma-separated name=value):
python wrappers/slangfx.py -i my_clip.mp4 --preset shaders/thermal/thermal.slangp \
  --params "contrast=1.8,outline=1.2" -o out.mp4
```

In the live tuner, the **Shader** menu auto-lists everything in `shaders/`, so
you can audition the whole library without relaunching.

## Colour & tone

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `chroma-shift` | 1 | Radial chromatic aberration: R/G/B fringe apart toward the edges, with a soft vignette and saturation lift — a cheap-lens-wide-open look. | `aberration`, `falloff`, `vignette`, `sat` |
| `thermal` | 1 | False-colour "thermal camera": luminance mapped through a black→magenta→red→orange→yellow→white heat ramp, with a Sobel rim outline. | `contrast`, `bias`, `outline` |
| `duotone` | 1 | Two-tone print: maps luma onto a gradient between a shadow hue and a highlight hue, plus crawling film grain. | `shadow_hue`, `light_hue`, `contrast`, `grain` |
| `posterize-pop` | 1 | Comic / cel look: quantises colour into a few flat bands, boosts saturation, and inks a black Sobel outline around shapes. | `levels`, `sat`, `outline`, `thickness` |
| `kaleidoscope` | 1 | Folds the frame into N mirrored wedges around the centre, slowly rotating the mirror and cycling the hue. | `segments`, `zoom`, `spin`, `hue_speed` |

## Edge detection

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `sobel-neon` | 1 | Sobel edge detection drawn as glowing neon lines whose hue follows the gradient direction; the flat interior is darkened so edges pop. | `scale` (edge gain), `threshold`, `glow`, `bg` |

(`posterize-pop` also stamps a Sobel cel outline — see Colour & tone.)

## Blur & bloom

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `bloom` | 4 | Threshold bright-pass (half-res) → separable Gaussian blur → screen-composited back over the clean frame, so highlights bleed soft coloured light. | `threshold`, `radius`, `intensity`, `sat` |
| `tilt-shift` | 3 | Separable Gaussian blur with a sharp horizontal focus band kept from the clean frame — the miniature/toy "tilt-shift lens" look, with a focus-band saturation pop. | `radius`, `focus` (band y), `band`, `softness`, `pop` |
| `motion-bloom` | 5 | Bloom keyed to inter-frame **motion**: only moving regions emit the glow, so movement bleeds coloured light while static areas stay sharp. | `sensitivity`, `radius`, `intensity`, `sat` |
| `soft-crt` | 3 | Subtle analog finishing grade: gentle full-frame soften, highlight bloom, fine scanlines, a contrast lift and animated film grain — every part on its own slider, at 0 that part switches off. | `soften`, `bloom_intensity`, `scan_strength`, `contrast`, `noise_strength` |

## Motion-reactive

These read the previous frame (via a passthrough + `PassFeedback`) and react to
inter-frame change — no audio needed.

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `motion-pulse` | 2 | Full-frame pulse (zoom punch / flash / RGB kick) driven by the prevailing amount of motion in the frame. | `sensitivity`, `zoom_punch`, `flash`, `rgb_kick` |
| `motion-shatter` | 2 | Block-displacement glitch that triggers **only where the frame is moving**; still areas stay clean. | `sensitivity`, `block_size`, `displace`, `rgb_split` |
| `motion-trails` | 2 | Wild motion-reactive smear: drags a feedback buffer along the flow and tears the R/G/B channels into rainbow ghost streaks. | `sensitivity`, `decay`, `smear_len`, `tear`, `hue_drift` |
| `perlin-flow` | 2 | Motion-reactive liquid melt: dissolves the image through an animated Perlin domain-warp; motion deepens the warp and cross-fades a psychedelic palette. | `scale`, `warp`, `motion_warp`, `speed`, `trip` |
| `voronoi-shatter` | 2 | Motion-reactive crystalline shatter: fractures the frame into a moving Voronoi mosaic with glowing leaded cracks; motion explodes the shards. | `cells`, `jitter`, `shatter`, `crack`, `swirl` |

## Tempo-synced

These take your track's tempo (`bpm`) and the render `fps` and gate the effect
to the beat (`beat_div` = subdivision). Set `bpm`/`fps` to match your song; pair
with `beat_cut.py` for cut-locked visuals.

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `beat-glitch` | 1 | Tempo-gated databend burst — slice/scramble/RGB-split that fires on the beat and decays. | `bpm`, `fps`, `beat_div`, `slice_amount`, `rgb_split` |
| `beat-kaleido` | 1 | Tempo-synced kaleidoscope with a beat-driven zoom pulse. | `bpm`, `beat_div`, `segments`, `spin`, `zoom_pulse` |
| `beat-strobe` | 1 | Tempo-synced strobe / zoom-punch with optional RGB kick and periodic invert. | `bpm`, `beat_div`, `strobe`, `zoom_punch`, `rgb_kick` |

## Glitch & analog

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `datamosh` | 2 | Compression-glitch / datamosh emulation: optical-flow bleed, block drift, freeze and trail decay, chroma shift. | `mosh_amount`, `bleed`, `flow_strength`, `block_glitch`, `freeze_amount` |
| `datamosh-skin` | 2 | Datamosh masked to skin tones (detected on the clean `Original`), so faces/skin stay readable while the rest moshes. | `mosh_amount`, `bleed`, `block_glitch` |
| `slice-glitch` | 1 | Databending look: horizontal slice tears, banding, RGB split and periodic glitch bursts. | `slice_amount`, `slice_density`, `bands`, `rgb_split`, `burst_freq` |
| `pixelsort` | 1 | Kim-Asendorf-style pixel sorting along threshold-bounded spans, with a moving wobble. | `threshold`, `sort_length`, `invert_dir`, `wobble` |
| `feedback-echo` | 1 | Analog video-feedback art (a camera pointed at its own monitor): zoom + rotate + decay trails with hue cycling. | `persistence`, `zoom`, `rotate`, `decay`, `hue_rate` |
| `scrambled-crt` | 4 | newpixie CRT (scanlines, mask, curvature, vignette) plus an analog cable-scramble: sync tear, hum bars, AGC pump, detune, static. | `scramble_amount`, `static_strength`, `curvature`, `vignette`, `ghosting` |

## Authoring notes

- Each effect is self-contained in its folder (`<name>.slang` + `<name>.slangp`,
  plus any helper passes). Copy a folder as a starting template.
- The standard push-constant fields (`SourceSize`, `OriginalSize`, `OutputSize`,
  `FrameCount`) and the realtime `Time` field are available; see
  [slang format]({{ '/slang_format.html' | relative_url }}).
- Motion effects use a 1-pass passthrough aliased `FrameNow` so the next pass can
  read the previous frame as `PassFeedback0`. Bloom-style effects read the clean
  input as `Original` in their composite pass.
- Keep `amount` as a 0..1 mix so `amount = 0` is always a clean passthrough —
  the live tuner and `beat_cut` rely on that convention.
