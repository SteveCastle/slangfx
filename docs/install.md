---
title: Install
---

# Install

## Prerequisites

| What | Version | Why |
|---|---|---|
| **Vulkan SDK** | 1.3+ | GPU dispatch + bundled `shaderc` for runtime GLSL→SPIR-V compilation |
| **ffmpeg** | any modern | input/output IO via pipes (no codec changes from your normal ffmpeg) |
| **C compiler** | C11 | gcc / clang / MSVC all work |
| **meson** | ≥ 1.1 | build system |
| **ninja** | any | build system backend |
| **python 3** | 3.8+ | for the wrapper script (optional but recommended) |

## Per-platform setup

### Windows (MinGW)

```powershell
# Vulkan SDK (LunarG installer)
winget install KhronosGroup.VulkanSDK

# meson + ninja
python -m pip install meson ninja
```

ffmpeg: any recent build is fine. If you don't have one, install the
gyan.dev or BtbN build:

```powershell
winget install Gyan.FFmpeg
# or
winget install BtbN.FFmpeg.GPL
```

### Debian / Ubuntu

```bash
sudo apt install libvulkan-dev libshaderc-dev meson ninja-build ffmpeg
```

### macOS

```bash
brew install vulkan-sdk shaderc meson ninja ffmpeg
```

MoltenVK ships with the LunarG SDK and translates Vulkan to Metal — works
fine for slangfx but expect slightly different performance characteristics
vs native Vulkan on Linux/Windows.

## Build

```bash
git clone https://github.com/SteveCastle/slangfx
cd slangfx
meson setup build -Denable_gpu=true
meson compile -C build
```

This produces `build/slangfx` (or `build/slangfx.exe` on Windows).

### Windows post-build

`shaderc_shared.dll` from the Vulkan SDK isn't on PATH by default. Copy
it next to the binary so it's discoverable at runtime:

```powershell
cp "$env:VULKAN_SDK\Bin\shaderc_shared.dll" build\
```

Alternatively, add `$env:VULKAN_SDK\Bin` to your `PATH`.

## Test

```bash
meson test -C build
```

Should report `1/1 OK` for the parser tests.

For an end-to-end smoke test once built:

```bash
python wrappers/slangfx.py \
  --slangfx ./build/slangfx \
  -i some-video.mp4 \
  --preset path/to/slang-shaders/misc/image-adjustment.slangp \
  -o out.mp4
```

If you don't have a slang-shaders checkout handy:

```bash
git clone https://github.com/libretro/slang-shaders
```

## Build options

| Option | Default | Effect |
|---|---|---|
| `-Denable_gpu=true` | `false` | Link against Vulkan + shaderc. **Required for any real shader work.** Off by default so the parser can be built standalone. |
| `-Dvulkan_sdk=/path` | `$VULKAN_SDK` | Override SDK location on Windows where pkg-config isn't available. |
| `-Dbuildtype=release` | `debugoptimized` | Optimization level. Pass `release` for maximum perf. |

## Troubleshooting

**`Library vulkan-1 found: NO`** during `meson setup`
: Set the `VULKAN_SDK` environment variable before running meson, or pass
  `-Dvulkan_sdk=/explicit/path`.

**`error while loading shared libraries: shaderc_shared.dll`** when running the binary
: On Windows: copy `shaderc_shared.dll` from `$env:VULKAN_SDK\Bin` next
  to `slangfx.exe`. On Linux: `ldconfig` should find `libshaderc.so` in
  the standard library path.

**`no Vulkan physical devices`** at runtime
: Your GPU driver doesn't expose a Vulkan ICD. On Linux install
  `mesa-vulkan-drivers` (Intel/AMD) or the proprietary NVIDIA driver. On
  Windows the Vulkan ICD ships with your GPU driver — make sure that's
  current.

**Compiles but produces black frames**
: Most shaders need `enable_gpu=true`. Confirm your build picked up
  Vulkan + shaderc by re-running `meson setup build --reconfigure
  -Denable_gpu=true` and rebuilding.
