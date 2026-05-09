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

`slangfx --help` prints the full option list.

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
├── wrappers/
│   └── slangfx.py             ffmpeg | slangfx | ffmpeg orchestration
├── tests/                     unit tests + slang fixtures
├── docs/                      GitHub Pages site (architecture, usage, etc.)
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
