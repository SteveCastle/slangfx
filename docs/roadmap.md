# Roadmap

Phased plan toward a `vf_slang` filter that can apply `crt/newpixie-crt.slangp`
(plus most of `libretro/slang-shaders`) with output indistinguishable from
RetroArch's vulkan driver.

The phases are sized for measurable, mergeable chunks. Each ends with a
demo and a checked-in test fixture.

---

## Phase 0 — Scaffold *(current)*

- [x] Repo layout, README, LICENSE, .gitignore
- [x] `meson.build` skeleton declaring deps (Vulkan SDK, shaderc,
      libavfilter, libavutil)
- [x] Source stubs with documented interfaces
- [x] `docs/architecture.md`, `docs/slang_format.md`

**Demo:** `meson setup build && meson compile -C build` produces a stub
`libvfslang.so` that registers a `slang` filter which currently no-ops
(passes input through unchanged). `ffmpeg -filters | grep slang` lists it.

---

## Phase 1 — Vulkan device + single SPIR-V dispatch

- [ ] Vulkan instance + device creation. Pick first discrete GPU.
- [ ] Swapchainless. We render to offscreen images.
- [ ] Allocate one input image (`VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT`) and one output image.
- [ ] Render pass, fullscreen-triangle vertex shader (hardcoded), wire one
      hardcoded fragment shader (e.g. invert) compiled to SPIR-V at build
      time.
- [ ] `filter_frame` uploads the AVFrame, dispatches, reads back.
- [ ] No slang parsing yet — just prove the GPU loop works.

**Demo:** `ffmpeg -i in.mp4 -vf slang=mode=invert out.mp4` produces a
color-inverted clip. Verify against reference.

---

## Phase 2 — `.slangp` parser

- [ ] Hand-written INI-style parser in `slangp.c`.
- [ ] Resolve relative shader paths against the preset directory.
- [ ] Populate `slangp_preset` (passes, textures, parameters).
- [ ] Test fixtures in `tests/fixtures/` covering the syntactic edge
      cases: `scale_type` per-axis vs uniform, alias with special chars,
      parameter overrides, multiple texture declarations, comments.
- [ ] `tests/test_slangp_parse.c` validates parse output matches
      hand-built expected structs.

**Demo:** `vfslang_dump_preset crt/newpixie-crt.slangp` prints the parsed
struct. Diff against an expected golden file.

---

## Phase 3 — slang → SPIR-V via shaderc

- [ ] `slang_compile.c` runs each `.slang` through libretro's slang
      preprocessor (port the relevant parts of `libretro/glslang/glslang`
      in `glslang_util_cxx.cpp`, ~600 lines, MIT-compatible).
      Alternative: invoke their `glslang` binary as a subprocess for v1.
- [ ] Resulting GLSL strings (one vertex, one fragment) are fed to
      `shaderc_compile_into_spv` with target `vulkan-1-3, spv-1-6`.
- [ ] SPIRV-Cross reflection (or our own MSL-like parsing) populates
      `push_constant_layout` and `ubo_layout`.
- [ ] Track each declared sampler by binding number and name so we can
      hook them up in the pipeline phase.

**Demo:** `vfslang_compile crt/shaders/newpixie/blur_horiz.slang` writes
`blur_horiz.vert.spv` and `blur_horiz.frag.spv`. Round-trip via
`spirv-dis` matches expected.

---

## Phase 4 — Single slang shader applied to video

- [ ] Wire phase 1 + phase 3: parse a one-pass slangp, compile its slang
      file, build a Vulkan pipeline from the resulting SPIR-V, run it.
- [ ] Push constants populated with `SourceSize`, `OutputSize`,
      `FrameCount`.
- [ ] UBO populated with identity `MVP`.
- [ ] Single `Source` sampler bound to the input image.

**Demo:** Apply a trivial single-pass slang shader (e.g. scanlines) to a
video. Output looks correct. Frame-by-frame parity check against
RetroArch.

---

## Phase 5 — Multi-pass chain

- [ ] Per-pass framebuffer allocation honoring `scale_type`/`scale`.
- [ ] Pass alias map: pass N's output is bound as both `Source` for
      pass N+1 and as the alias name (e.g. `accum1`) for any later pass
      that declares it.
- [ ] `Original` sampler always bound to the original input image.
- [ ] `Pass<n>` (numeric) accessors for any pass referencing previous
      pass outputs by index.

**Demo:** A two-pass blur shader produces correct output on both axes.

---

## Phase 6 — `PassFeedback<n>` and `OriginalHistory<n>`

- [ ] Detect at compile time which passes have feedback consumers
      (search SPIR-V reflection / GLSL source for `PassFeedback<n>`).
- [ ] Allocate ring of 2 framebuffers per feedback-producing pass; flip
      after each frame.
- [ ] Detect history depth needed for `OriginalHistory<n>`. Allocate
      ring of N+1 input snapshots; rotate.
- [ ] Wire into descriptor sets at the start of each frame.

**Demo:** `crt/newpixie-crt.slangp` runs end-to-end with a real
PassFeedback1 accumulator. The compounding multi-frame trail matches
RetroArch's output (within rounding error).

---

## Phase 7 — Push constants and UBO completion

- [ ] All standard slang push constant fields:
      `MVP, SourceSize, OutputSize, OriginalSize, FrameCount,
      FrameDirection, Rotation, etc.`
- [ ] `#pragma parameter` declarations exposed via the filter's `params`
      option (`-vf "slang=preset=...:params=curvature=2.0,vignette=1.0"`)
      and bound into the right push constant slot per pass.

**Demo:** All knobs of `newpixie-crt` are runtime-tunable from the CLI.

---

## Phase 8 — External textures

- [ ] `textures = "name"` directive: load the listed image (PNG/JPG)
      via stb_image (vendored single-header, MIT) or libavcodec.
- [ ] Honor `<name>_linear`, `<name>_wrap_mode`, `<name>_mipmap`.
- [ ] Bind into descriptor sets where any pass declares the named
      sampler.

**Demo:** `newpixie-crt` with `use_frame=1` and `frametexture =
crtframe.png` renders correctly with the bezel composited.

---

## Phase 9 — YUV input/output, hardware decode

- [ ] Accept YUV pixel formats. Convert to RGB on input via a
      compile-time-emitted shader prelude (BT.601/709/2020 + range).
- [ ] Output back to YUV if downstream expects it.
- [ ] Optionally accept `AV_PIX_FMT_VULKAN` frames directly (zero-copy
      from an upstream Vulkan filter or hwdec).

**Demo:** Apply newpixie-crt to a 4K HDR YUV source without an
intermediate format conversion stage.

---

## Phase 10 — Polish + parity sweep

- [ ] Run a representative subset of `libretro/slang-shaders` (CRT,
      scanlines, ntsc, sharpen) through both this filter and RetroArch.
      Build a perceptual-diff dashboard.
- [ ] Resolve any remaining behavioral divergences from the reference.
- [ ] Document filter usage on the `ffmpeg-user` ML; submit
      patchset for upstream review (libavfilter accepts external GPU
      filters; precedent: `vf_libplacebo`, `vf_vulkan`, `vf_ngl`).

**Demo:** `vf_slang` ships in ffmpeg HEAD as a default-built filter on
systems where Vulkan + shaderc are detected by `configure`.

---

## Out of scope (for now)

- Compute shader passes (libretro slang allows them; rare in the wild).
- HLSL / cg shader support (libretro has those formats too, but they're
  legacy; slang covers everything new).
- GUI / preset editor.
- Real-time preview UI.
