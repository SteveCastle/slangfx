# slangfx

Apply libretro-format **slang shaders** — the multi-pass GPU shader format
used by RetroArch's vulkan / glcore drivers, including the entire
[libretro/slang-shaders](https://github.com/libretro/slang-shaders) corpus
(CRT, scanlines, NTSC/VHS, scalers, AA, …) — to any video file.

Runs as a standalone Vulkan-based binary that takes raw RGBA frames on
stdin and writes processed frames on stdout. ffmpeg handles decode and
encode through pipes.

```
ffmpeg -i in.mp4 -f rawvideo -pix_fmt rgba - \
  | slangfx --preset crt/newpixie-crt.slangp --width 1920 --height 1080 \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mp4
```

Or use the bundled wrapper for the common case:

```
python wrappers/slangfx.py -i in.mp4 --preset crt/newpixie-crt.slangp -o out.mp4
```

Full documentation: <https://SteveCastle.github.io/slangfx/>

---

## Install

### Prerequisites

| What | Why |
|---|---|
| **Vulkan SDK 1.3+** ([download](https://vulkan.lunarg.com/sdk/home)) | GPU dispatch + bundled shaderc compiler |
| **ffmpeg** | input/output IO via pipes |
| **A C compiler + meson + ninja** | building from source |

On Windows:

```powershell
winget install KhronosGroup.VulkanSDK
python -m pip install meson ninja
```

On Debian/Ubuntu:

```bash
sudo apt install libvulkan-dev libshaderc-dev meson ninja-build ffmpeg
```

On macOS:

```bash
brew install vulkan-sdk shaderc meson ninja ffmpeg
```

### Build

```bash
meson setup build -Denable_gpu=true
meson compile -C build
```

This produces `build/slangfx` (or `build/slangfx.exe` on Windows).

On Windows, copy `shaderc_shared.dll` from the Vulkan SDK next to the
binary so it's discoverable at runtime:

```powershell
cp "$env:VULKAN_SDK\Bin\shaderc_shared.dll" build\
```

### Run the parser tests

```bash
meson test -C build
```

---

## Usage

### High-level (recommended)

`wrappers/slangfx.py` orchestrates `ffmpeg → slangfx → ffmpeg` for you.
Audio is taken from the input file unchanged.

```bash
python wrappers/slangfx.py \
  -i input.mp4 \
  --preset path/to/preset.slangp \
  -o output.mp4
```

Optional flags:

| Flag | Purpose |
|---|---|
| `--slangfx <path>` | Path to the `slangfx` binary (defaults to PATH lookup) |
| `--ffmpeg <path>` | Path to the `ffmpeg` binary (defaults to PATH lookup) |
| `--vcodec libx264` | Output video codec (default `libx264`) |
| `--crf 20` | Quality (lower = bigger / better) |
| `--preset-x264 veryfast` | x264 encoder preset |

### Low-level (direct pipes)

```
ffmpeg -i in.mp4 -f rawvideo -pix_fmt rgba - \
  | slangfx --preset PRESET.slangp --width W --height H \
  | ffmpeg -f rawvideo -pix_fmt rgba -s WxH -framerate FR -i - \
          -i in.mp4 -map 0:v -map 1:a -c:v libx264 -c:a copy out.mp4
```

`slangfx --help` prints the full option list. `--preset` is repeatable — each
one is a layer, applied in order and chained on the GPU with no intermediate
readback (a following `--params` binds to the preset before it).

### Choosing a shader

Most CRT, scanline, NTSC, and color-grading shaders from
[libretro/slang-shaders](https://github.com/libretro/slang-shaders) work
without modification. Pass the path to a `.slangp` preset file. Example
presets that have been verified to run end-to-end:

- `crt/newpixie-crt.slangp` — multi-frame CRT accumulator with curvature, scanlines, mask, vignette, ghosting trails (4 passes, PassFeedback)
- `vhs/ntsc-vcr.slangp` — NTSC composite encode/decode with VHS tape grain and scrolling lines (5 passes, float framebuffer)
- `misc/image-adjustment.slangp` — gamma / saturation / contrast / overscan (1 pass)
- Most `crt/*.slangp` presets (Easymode, Lottes, Geom, Royale variants)

Some shaders depend on features not yet implemented — see
[Limitations](https://SteveCastle.github.io/slangfx/limitations.html).

### Bundled effects

slangfx also ships its own library of original effects under [`shaders/`](shaders/)
— adjustment primitives built for stacking as layers (exposure, levels,
saturation, white balance, split-tone, mono, sharpen, vignette, grain,
transform), blurs and blooms, stylized looks (halftone, duotone, thermal,
posterize, kaleidoscope, pixelate, neon edges), motion-reactive feedback,
tempo-synced beats, glitch/analog looks, and a CRT & retro-display wing —
phosphor terminals (green/amber/paper), consumer TV, PVM, Game Boy LCD,
gas plasma, vector monitor, LED marquee/wall, e-ink — plus ten dithering strategies from Bayer and blue-noise to ASCII glyphs, CGA palettes, crosshatch and stipple, and a colour-split family (RGB split, lens chromatic aberration, anaglyph 3D-glasses, prism smear, temporal split), and a stamp/title overlay (66 presets). They
follow the same
`.slangp` format, so they run anywhere a libretro preset does:

```bash
python wrappers/slangfx.py -i my_clip.mp4 --preset shaders/blur-bloom/bloom/bloom.slangp -o out.mp4
```

A few to try: `green-terminal`, `gameboy`, `crt-tv`, `bloom`, `soft-crt`, `sobel-neon`, `thermal`, `chroma-shift`, `kaleidoscope`,
`tilt-shift`, `motion-trails`, `perlin-flow`, `voronoi-shatter`. Most expose an
`amount` (0 = passthrough) plus tunable sliders. See the full catalogue with
descriptions and parameters in
[**Bundled effects**](https://SteveCastle.github.io/slangfx/effects.html)
([`docs/effects.md`](docs/effects.md)). The easiest way to audition them is the
live tuner (next section), whose **Shader** menu lists the whole folder.

### Live preview & tuning

`wrappers/slangfx_live.py` streams a clip — or loops a still image — through a
**stack of effects (layers)** in a window with one slider per parameter,
grouped per layer. Add, remove, reorder, or bypass layers from the Layers
panel (the stack runs as one GPU chain inside a single slangfx process — no
intermediate readbacks, ~1.6× faster than piping processes at 1080p); switch
shader/video from a menu; scrub the clip; and export the stacked result to
H.264 or a single frame to PNG/JPEG (stills render as a clip of
`--image-duration` seconds). Headless, layers stack by repeating flags:

```bash
python wrappers/slangfx_live.py -i in.mp4 \
  --preset shaders/crt/soft-crt/soft-crt.slangp --params "scan_strength=0.2" \
  --preset shaders/glitch/chroma-shift/chroma-shift.slangp \
  --export out.mp4
```

No Python needed if you use a [release](https://github.com/SteveCastle/slangfx/releases):
the archive ships a self-contained **`slangfx-live`** executable (Python
runtime bundled) next to `slangfx` and `shaders/` — just run it (ffmpeg on
PATH is still required). See [Usage → Live parameter tuner](https://SteveCastle.github.io/slangfx/usage.html).

```bash
# One-time: a dedicated environment for the wrapper tools
python -m venv .venv
.venv/Scripts/python -m pip install -r wrappers/requirements.txt   # Windows
# .venv/bin/python -m pip install -r wrappers/requirements.txt     # Linux/macOS

.venv/Scripts/python wrappers/slangfx_live.py   # start empty; load a clip + effect from the menus
```

### Run it in the browser (WebGPU + WASM)

[`web/`](web/) contains **slangfx-web**: the same slang shader chains
running in the browser — the glslang + tint shader toolchain compiled to
WebAssembly, the multi-pass runtime ported to WebGPU, and a web version of
the live tuner (layers, sliders, scrub, PNG/WebM export). All 66 bundled
presets work, feedback and multi-pass included. It's also an embeddable ES
module for use in other apps (headless rendering supported).

**Try it live: <https://SteveCastle.github.io/slangfx/web/demo/>** — or run
it locally:

```bash
cd web && npm install && npm run serve   # → http://localhost:8788/web/demo/
```

See [`web/README.md`](web/README.md) for the architecture and embedding API.

### Tuning shader parameters

slang shaders declare runtime parameters via `#pragma parameter`. Override
defaults by editing the `parameters = "..."` block of the `.slangp` preset.
Example overrides for `vhs/ntsc-vcr.slangp`:

```
parameters = "GRAIN_STR;saturation"
GRAIN_STR = 12.0
saturation = 0.5
```

---

## Repo layout

```
slangfx/
├── src/                       the binary
│   ├── main.c                 stdin/stdout entrypoint
│   ├── slangp.{c,h}           .slangp preset parser
│   ├── slang_compile.{c,h}    slang → SPIR-V via shaderc
│   ├── slang_pipeline.{c,h}   Vulkan multi-pass dispatch
│   └── spv_reflect.{c,h}      SPIR-V reflection for layout discovery
├── shaders/                   bundled original effects (see docs/effects.md)
│   └── <category>/<effect>/<effect>.slangp + .slang passes
│       (adjust, blur-bloom, stylize, split, dither, crt, motion, beat, glitch, overlay)
├── wrappers/
│   ├── slangfx.py             ffmpeg | slangfx | ffmpeg orchestration
│   ├── slangfx_live.py        live preview + param sliders + export (Dear PyGui)
│   └── requirements.txt       Python deps for the live tuner
├── tests/                     unit tests + slang fixtures
├── docs/                      GitHub Pages site (architecture, usage, effects, etc.)
├── meson.build                build config
└── README.md                  this file
```

---

## License

MIT. See [`LICENSE`](LICENSE).

The libretro slang shaders themselves carry their own licenses (most are
GPL or public domain). slangfx merely loads and runs them; it does not
redistribute the shader source.

---

## Related

- [libretro/slang-shaders](https://github.com/libretro/slang-shaders) — source format
- [libretro/glslang](https://github.com/libretro/glslang) — reference slang preprocessor / compiler
- [`vf_libplacebo`](https://ffmpeg.org/ffmpeg-filters.html#libplacebo) — ffmpeg's existing GPU-shader filter (different shader format, mpv-style)
- [Khronos shaderc](https://github.com/google/shaderc) — runtime GLSL → SPIR-V compiler used internally
