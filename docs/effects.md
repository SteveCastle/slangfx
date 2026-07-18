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
  --preset shaders/blur-bloom/bloom/bloom.slangp -o out.mp4

# Tune it live with sliders (and switch effects / export from the menus):
python wrappers/slangfx_live.py -i my_clip.mp4 --preset shaders/blur-bloom/bloom/bloom.slangp

# Override any parameter (comma-separated name=value):
python wrappers/slangfx.py -i my_clip.mp4 --preset shaders/stylize/thermal/thermal.slangp \
  --params "contrast=1.8,outline=1.2" -o out.mp4
```

In the live tuner, the **Shader** menu auto-lists everything in `shaders/`, so
you can audition the whole library without relaunching.

## Adjustment primitives (for layering)

Building blocks meant to be **stacked as layers** (Shader → Add layer in the
live tuner, or repeated `--preset` flags): tone, colour, detail, texture, and
geometry, each doing one thing with clean sliders. The adjustment ones default
to **neutral** — adding the layer changes nothing until you dial it — and all
keep `amount = 0` as an exact passthrough.

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `exposure` | 1 | Exposure (photographic stops), brightness offset, contrast, gamma. | `exposure`, `brightness`, `contrast`, `gamma_adj` |
| `levels` | 1 | Input/output black & white points with midtone gamma — the Levels dialog as a layer. | `in_black`, `in_white`, `mid_gamma`, `out_black`, `out_white` |
| `saturation` | 1 | Saturation, vibrance (boosts muted colours more), hue rotation. | `saturation`, `vibrance`, `hue_deg` |
| `temperature` | 1 | White balance: warm/cool + green/magenta tint, brightness-preserving. | `temperature`, `tint`, `luma_lock` |
| `split-tone` | 1 | Tint shadows one hue, highlights another (chroma-only, luma untouched). | `shadow_hue`, `shadow_amt`, `high_hue`, `high_amt`, `balance` |
| `mono` | 1 | B&W via channel mixer (ratios matter, not sums) + optional tint (sepia). | `red_w`, `green_w`, `blue_w`, `tint_hue`, `tint_amt` |
| `sharpen` | 1 | Unsharp mask: strength + radius. | `strength`, `s_radius` |
| `vignette` | 1 | Radial corner darkening: strength, size, softness, roundness. | `strength`, `v_size`, `softness`, `roundness` |
| `grain` | 1 | Animated film grain (true static, never drifts), optional colour grain, shadow-weighted. | `strength`, `g_size`, `shadow_bias`, `colored` |
| `transform` | 1 | Zoom / pan / rotate / flip, aspect-corrected, black outside the frame. | `zoom`, `pan_x`, `pan_y`, `rot_deg`, `flip_h`, `flip_v` |

## Stylize

Graphic looks — print, cel, false-colour, geometric.

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `thermal` | 1 | False-colour "thermal camera": luminance mapped through a black→magenta→red→orange→yellow→white heat ramp, with a Sobel rim outline. | `contrast`, `bias`, `outline` |
| `duotone` | 1 | Two-tone print: maps luma onto a gradient between a shadow hue and a highlight hue, plus animated film grain. | `shadow_hue`, `light_hue`, `contrast`, `grain` |
| `posterize-pop` | 1 | Comic / cel look: quantises colour into a few flat bands, boosts saturation, and inks a black Sobel outline around shapes. | `levels`, `sat`, `outline`, `thickness` |
| `halftone` | 1 | Print-style halftone: luma-sized dots on a rotated screen grid. | `dot_size`, `angle`, `sharp` |
| `kaleidoscope` | 1 | Folds the frame into N mirrored wedges around the centre, slowly rotating the mirror and cycling the hue. | `segments`, `zoom`, `spin`, `hue_speed` |
| `pixelate` | 1 | Mosaic resample to N-px blocks. | `px` |
| `sobel-neon` | 1 | Sobel edge detection drawn as glowing neon lines whose hue follows the gradient direction; the flat interior is darkened so edges pop. | `scale` (edge gain), `threshold`, `glow`, `bg` |

## Colour split

Channel-separation looks — every way to pull R, G and B apart. (See also
`chroma-shift` under Glitch & analog for the original single-tap radial CA.)

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `rgb-split` | 1 | The classic directional split: red and blue pull apart along an angle, with R/B balance and an animated wiggle. | `dist_rs`, `angle_rs`, `spread_rs`, `wiggle_rs` |
| `chromatic-aberration` | 1 | Lens-style transverse CA: channels resampled at different radial magnifications across soft multi-tap fringes, growing toward the edges. | `strength_ca`, `falloff_ca`, `soft_ca` |
| `anaglyph-3d` | 1 | 3D-glasses encoding (red/cyan, green/magenta, amber/blue) from a faked stereo pair; luma-driven pseudo-depth makes bright things pop. Works with real paper glasses. | `sep_3d`, `depth_3d`, `mode_3d`, `rivalry_3d` |
| `spectrum-smear` | 1 | Prismatic rainbow streak: the frame smears along a direction (or radially) with spectrally-tinted taps — highlights drag little rainbows. | `length_sp`, `angle_sp`, `radial_sp`, `vivid_sp` |
| `channel-drift` | 1 | R/G/B wander independent Lissajous orbits — woozy projector-plate misregistration, from dreamy to nervous. | `drift_cd`, `speed_cd`, `sway_cd` |
| `radial-split` | 1 | Red and blue counter-rotate around the centre, growing with radius: registered core, RGB pinwheel edges. | `rot_rp`, `falloff_rp` |
| `time-split` | 1 | Temporal split: green and blue lag behind the live red channel through feedback — motion pulls rainbow fringes, stills stay registered. | `lag_g`, `lag_b` |

## Dithering

Ten ways to lie about how many tones you have. Different threshold
strategies (ordered, blue-noise, clustered, palette-constrained) and
drawn-by-hand looks (hatching, stipple, glyphs) — all with chunky-pixel
scaling and tintable inks where it makes sense.

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `bayer-dither` | 1 | The reference ordered dither: selectable 2×2/4×4/8×8 Bayer matrix, per-channel levels, color or mono. | `matrix_bd`, `levels_bd`, `chunk_bd`, `mono_bd` |
| `blue-noise-dither` | 1 | Interleaved-gradient-noise thresholds — organic, pattern-free grain; optional per-frame re-roll for living dither. | `levels_bn`, `chunk_bn`, `animate_bn`, `softness_bn` |
| `cluster-dot` | 1 | Newsprint photo screen: clustered-dot matrix grows ink dots from cell centers; tintable ink + paper. | `cell_cd`, `contrast_cd`, `ink_*`, `paper_*` |
| `line-screen` | 1 | Engraving line halftone: angled ink lines whose thickness carries the tone, with hand-printed wobble. | `period_ls`, `angle_ls`, `gamma_ls`, `wobble_ls` |
| `crosshatch` | 1 | Pen-and-ink hatching: strokes layer in by darkness (diagonal → cross → horizontal → vertical), nib jitter, paper grain. | `period_ch`, `width_ch`, `jitter_ch`, `ink_*` |
| `stipple` | 1 | Etching stipple: static random dots, density and size follow tone, jittered centers. | `cell_st`, `dot_st`, `jitter_st`, `gamma_st` |
| `ascii-dither` | 1 | Terminal glyph ramp ( `.:*o&8@` ) per cell from procedural 5×5 bitmaps; green-on-black or colored by the source. | `cell_ac`, `colored_ac`, `fg_*`, `bg_ac` |
| `cga-dither` | 1 | Ordered dither constrained to CGA-era palettes: cyan/magenta/white, green/red/brown, or 1-bit. | `palette_cg`, `chunk_cg`, `spread_cg` |
| `mac-1bit` | 1 | System 7 one-bit: 8×8 Bayer to platinum paper + soft black ink, chunky pixels, invert toggle. | `chunk_mb`, `contrast_mb`, `invert_mb` |
| `flow-dither` | 1 | Animated duotone dither whose threshold field is domain-warped by drifting currents — the boundary flows like ink in water. | `warp_fd`, `speed_fd`, `scale_fd`, `dark_*`, `light_*` |

## CRT & retro displays

Full display emulations — point them at anything and it plays back on
period glass. Three of them are monochrome phosphor terminals sharing one
engine (`phosphor.slang`) with per-preset tints; several use `PassFeedback`
for phosphor persistence / LCD lag.

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `green-terminal` | 1 | P1 green phosphor terminal (VT100 / IBM 5151): scanlines, curvature, glow, long-persistence trails, mains flicker, tube noise. | `scan_strength`, `curvature`, `glow`, `persistence`, `flicker`, `tint_*` |
| `amber-terminal` | 1 | P3 amber phosphor (Wyse / Hercules) — same engine, warm tint. | same as `green-terminal` |
| `paper-terminal` | 1 | Paper-white mono (VT320 / NeXT): crisper, faster phosphor — less glow and persistence. | same as `green-terminal` |
| `crt-tv` | 1 | Living-room television: curvature, slot-mask triads, composite chroma bleed, beam scanlines, drifting hum bar, antenna static. | `curvature`, `mask_strength`, `bleed`, `hum`, `static_tv` |
| `pvm-grille` | 1 | Sony PVM/BVM broadcast monitor: razor aperture grille, tight bloom, faint scanlines, saturation lift, no curvature. | `grille_strength`, `bloom_p`, `sharp_p`, `sat_p` |
| `gameboy` | 1 | DMG-01 LCD: four pea-soup green shades, Bayer dither, dot grid, slow-LCD motion ghosting. | `res_px`, `ghost`, `grid_gb`, `dither_gb` |
| `plasma-display` | 1 | Orange gas-plasma panel (GRiD Compass): coarse cells, few brightness levels, gas glow, 60 Hz shimmer. | `cell_px`, `levels_p`, `gap_p`, `glow_p` |
| `vector-display` | 1 | XY vector monitor (Asteroids / radar): Sobel strokes as glowing beam traces with halo, phosphor trails, jitter, graticule. | `beam`, `threshold_v`, `trail_v`, `jitter_v`, `tint_*` |
| `dot-matrix` | 1 | Amber LED marquee: round dots lit by luma, quantized levels, unlit husks; recolor via `led_*`. | `dot_px`, `dot_size_dm`, `unlit_dm`, `led_*` |
| `led-wall` | 1 | Stadium RGB LED billboard: rounded modules, dark seams, driver banding, bloom across gaps, refresh shimmer. | `cell_w`, `gap_w`, `banding_w`, `bloom_w` |
| `eink` | 1 | E-paper: warm paper + ink, ordered dither, static grain, refresh ghosting — the anti-CRT. | `ink_levels`, `dither_e`, `ghost_e`, `paper_e` |
| `soft-crt` | 3 | Subtle analog finishing grade: gentle full-frame soften, highlight bloom, exposure + contrast trim, fine scanlines, corner vignette and animated film grain — every part on its own slider, at 0 (or 1.0 for exposure) that part switches off. | `soften`, `bloom_intensity`, `scan_strength`, `exposure`, `vignette`, `noise_strength` |
| `scrambled-crt` | 4 | newpixie CRT (scanlines, mask, curvature, vignette) plus an analog cable-scramble: sync tear, hum bars, AGC pump, detune, static. | `scramble_amount`, `static_strength`, `curvature`, `vignette`, `ghosting` |

## Blur & bloom

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `gaussian-blur` | 2 | Separable Gaussian blur, mixed against the clean input by `amount`. | `radius`, `amount` |
| `bloom` | 4 | Threshold bright-pass (half-res) → separable Gaussian blur → screen-composited back over the clean frame, so highlights bleed soft coloured light. | `threshold`, `radius`, `intensity`, `sat` |
| `tilt-shift` | 3 | Separable Gaussian blur with a sharp horizontal focus band kept from the clean frame — the miniature/toy "tilt-shift lens" look, with a focus-band saturation pop. | `radius`, `focus` (band y), `band`, `softness`, `pop` |

## Motion-reactive

These read the previous frame (via a passthrough + `PassFeedback`) and react to
inter-frame change — no audio needed.

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `motion-bloom` | 5 | Bloom keyed to inter-frame **motion**: only moving regions emit the glow, so movement bleeds coloured light while static areas stay sharp. | `sensitivity`, `radius`, `intensity`, `sat` |
| `motion-pulse` | 2 | Full-frame pulse (zoom punch / flash / RGB kick) driven by the prevailing amount of motion in the frame. | `sensitivity`, `zoom_punch`, `flash`, `rgb_kick` |
| `motion-shatter` | 2 | Block-displacement glitch that triggers **only where the frame is moving**; still areas stay clean. | `sensitivity`, `block_size`, `displace`, `rgb_split` |
| `motion-trails` | 2 | Wild motion-reactive smear: drags a feedback buffer along the flow and tears the R/G/B channels into rainbow ghost streaks. | `sensitivity`, `decay`, `smear_len`, `tear`, `hue_drift` |
| `perlin-flow` | 2 | Motion-reactive liquid melt: dissolves the image through an animated Perlin domain-warp; motion deepens the warp and cross-fades a psychedelic palette. | `scale`, `warp`, `motion_warp`, `speed`, `trip` |
| `voronoi-shatter` | 2 | Motion-reactive crystalline shatter: fractures the frame into a moving Voronoi mosaic with glowing leaded cracks; motion explodes the shards. | `cells`, `jitter`, `shatter`, `crack`, `swirl` |

## Tempo-synced

These take your track's tempo (`bpm`) and the render `fps` and gate the effect
to the beat (`beat_div` = subdivision). Set `bpm`/`fps` to match your song, then
slide `beat_phase` (±1 beat) to line the hits up with the track's actual
downbeats. Pair with `beat_cut.py` for cut-locked visuals.

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `beat-glitch` | 1 | Tempo-gated databend burst — slice/scramble/RGB-split that fires on the beat and decays. | `bpm`, `fps`, `beat_div`, `slice_amount`, `rgb_split` |
| `beat-kaleido` | 1 | Tempo-synced kaleidoscope with a beat-driven zoom pulse. | `bpm`, `beat_div`, `segments`, `spin`, `zoom_pulse` |
| `beat-strobe` | 1 | Tempo-synced strobe / zoom-punch with optional RGB kick and periodic invert. | `bpm`, `beat_div`, `strobe`, `zoom_punch`, `rgb_kick` |

## Glitch & analog

| Effect | Passes | What it does | Key params |
|---|---|---|---|
| `chroma-shift` | 1 | Radial chromatic aberration: R/G/B fringe apart toward the edges, with a soft vignette and saturation lift — a cheap-lens-wide-open look. | `aberration`, `falloff`, `vignette`, `sat` |
| `datamosh` | 2 | Compression-glitch / datamosh emulation: optical-flow bleed, block drift, freeze and trail decay, chroma shift. | `mosh_amount`, `bleed`, `flow_strength`, `block_glitch`, `freeze_amount` |
| `datamosh-skin` | 2 | Datamosh masked to skin tones (detected on the clean `Original`), so faces/skin stay readable while the rest moshes. | `mosh_amount`, `bleed`, `block_glitch` |
| `slice-glitch` | 1 | Databending look: horizontal slice tears, banding, RGB split and periodic glitch bursts. | `slice_amount`, `slice_density`, `bands`, `rgb_split`, `burst_freq` |
| `pixelsort` | 1 | Kim-Asendorf-style pixel sorting along threshold-bounded spans, with a moving wobble. | `threshold`, `sort_length`, `invert_dir`, `wobble` |
| `feedback-echo` | 1 | Analog video-feedback art (a camera pointed at its own monitor): zoom + rotate + decay trails with hue cycling. | `persistence`, `zoom`, `rotate`, `decay`, `hue_rate` |

## Authoring notes

- Each effect is self-contained in its folder (`<name>.slang` + `<name>.slangp`,
  plus any helper passes). Copy a folder as a starting template. Effects are
  grouped into category folders — `shaders/<category>/<effect>/` — matching
  the sections on this page (`adjust`, `blur-bloom`, `stylize`, `split`,
  `dither`, `crt`, `motion`, `beat`, `glitch`).
- The standard push-constant fields (`SourceSize`, `OriginalSize`, `OutputSize`,
  `FrameCount`) and the realtime `Time` field are available; see
  [slang format]({{ '/slang_format.html' | relative_url }}).
- Motion effects use a 1-pass passthrough aliased `FrameNow` so the next pass can
  read the previous frame as `PassFeedback0`. Bloom-style effects read the clean
  input as `Original` in their composite pass.
- Keep `amount` as a 0..1 mix so `amount = 0` is always a clean passthrough —
  the live tuner and `beat_cut` rely on that convention.
