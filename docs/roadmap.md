# Roadmap

Phased plan toward a `vf_slang` filter that can apply `crt/newpixie-crt.slangp`
(plus most of `libretro/slang-shaders`) with output indistinguishable from
RetroArch's vulkan driver.

The phases are sized for measurable, mergeable chunks. Each ends with a
demo and a checked-in test fixture.

---

## Phase 0 — Scaffold *(done)*

- [x] Repo layout, README, LICENSE, .gitignore, .gitattributes
- [x] `meson.build` (Vulkan + shaderc gated behind `-Denable_gpu=true`).
- [x] Source stubs with documented interfaces.
- [x] `docs/architecture.md`, `docs/roadmap.md`, `docs/slang_format.md`
- [x] `wrappers/vfslang.py` for ffmpeg ↔ vfslang ↔ ffmpeg orchestration.

**Demo:** `meson setup build && meson compile -C build` produces a working
`vfslang` binary. It reads RGBA frames from stdin and writes them to
stdout (currently identity copy, the work happens in subsequent phases).
`vfslang --help` prints usage. `wrappers/vfslang.py -i in.mp4 --preset
foo.slangp -o out.mp4` runs end-to-end (with the preset parser stubbed
so it'll bail with a friendly "not yet implemented (Phase 2)" message —
proving the whole pipe pipeline works).

---

## Phase 1 — Vulkan device + single SPIR-V dispatch *(done)*

- [x] VkInstance + VkDevice + queue + memory properties cache.
- [x] Offscreen render-to-texture (no swapchain).
- [x] Input image (`TRANSFER_DST + SAMPLED`); output image
      (`COLOR_ATTACHMENT + TRANSFER_SRC`).
- [x] Render pass, framebuffer, sampler, descriptor pool/set.
- [x] Fullscreen-triangle vertex shader + passthrough fragment shader
      (built-in slang strings compiled through shaderc at startup).
- [x] `slang_pipeline_run`: upload via staging buffer, dispatch, readback.

**Verified:** 4×4 red input → byte-perfect red output (passthrough
shader); 16×16 RGB gradient → byte-perfect gradient out, all sample
positions correct.

---

## Phase 2 — `.slangp` parser *(done)*

- [x] Hand-written INI-style lexer + key dispatcher in `slangp.c`.
- [x] All indexed pass keys: `shader<i>, alias<i>,
      scale[_x|_y]<i>, scale_type[_x|_y]<i>, filter_linear<i>,
      mipmap_input<i>, wrap_mode<i>, frame_count_mod<i>,
      srgb_framebuffer<i>, float_framebuffer<i>, fbo_format<i>`.
- [x] `textures = "a;b"` with per-texture sub-options
      (`<n>_linear, _wrap_mode, _mipmap`).
- [x] `parameters = "k;l"` with float overrides.
- [x] Comments, quoted strings, whitespace tolerance, `scale_<i>`
      underscore typo tolerance.
- [x] Relative path resolution against preset directory.
- [x] `tests/test_slangp_parse.c` — 7 test cases pass under `meson test`.

---

## Phase 3 — slang → SPIR-V via shaderc *(done — reflection deferred to Phase 7)*

- [x] Source scanner splits on `#pragma stage vertex` / `fragment`,
      prepends shared header to each stage buffer.
- [x] Collects `#pragma parameter NAME "DESC" DEFAULT MIN MAX STEP`
      into `slang_module.params`. Tolerates missing min/max/step.
- [x] Collects `#pragma format <FMT>` into `fbo_format_pragma`.
- [x] Each stage compiled with shaderc targeting Vulkan 1.3 / SPIR-V 1.5
      at `optimization_level_performance`.
- [x] **`#include` resolution** via shaderc include callbacks. Resolves
      relative to the requesting file's directory; falls back to the
      slang file's parent dir or an explicit `include_dir`. This is what
      unblocks the libretro corpus.
- [ ] **Deferred (Phase 7):** SPIR-V reflection to populate
      `push_constant_layout` and `ubo_layout` so the host knows where to
      write each declared parameter and standard field.

**Verified:** real libretro shader `misc/image-adjustment.slangp`
(which `#include`s `../../include/colorspace-tools.h`) compiles
through end-to-end and the resulting pipeline runs.

---

## Phase 4 — Single slang shader applied to video *(done)*

- [x] Pipeline binding contract matches slang convention:
      UBO @ set=0,binding=0; Source sampler @ set=0,binding=2;
      push_constant range covering `slang_push`; vertex inputs at
      locations 0 (vec4 Position) and 1 (vec2 TexCoord).
- [x] Vertex/index buffers (4-vert fullscreen quad, 6 indices, two
      triangles) — slang vertex shaders consume these directly.
- [x] UBO populated with identity MVP.
- [x] Push constants populated with SourceSize, OriginalSize,
      OutputSize, FrameCount each frame.
- [x] When preset has 1 pass with a path → compile via
      `slang_compile_file`, use its SPIR-V; otherwise fall back to a
      built-in slang passthrough.

**Verified:** `tests/fixtures/invert.slang` (full slang shader with
both stages) inverts solid-red input to cyan; spatial gradient inverts
pixel-perfect; built-in passthrough preserves red.

---

## Phase 5 — Multi-pass chain *(MVP done; alias / Pass<n> / Original deferred)*

- [x] Per-pass `pass_state` holding its own framebuffer image, render
      pass, descriptor set, pipeline, sampler.
- [x] Per-pass framebuffer dimensions resolved from `scale_type[_x|_y]`
      and `scale[_x|_y]` (source / viewport / absolute, per axis).
- [x] Per-pass sampler honors `filter_linear` and `wrap_mode`.
- [x] Source chaining: pass N's `Source` descriptor binds pass N-1's
      output view (or the original input for pass 0). Render pass
      finalLayout = SHADER_READ_ONLY so chaining is layout-correct.
- [ ] **Phase 5b — alias bindings:** preset `alias<i> = name` declares
      a sampler the next pass can bind by that name. Implement: build
      alias→pass-index map at setup, extend descriptor sets to include
      one extra sampler binding per resolved alias used by the
      consuming pass.
- [ ] **Phase 5c — `Pass<n>`:** numeric pass refs. Same mechanism as
      aliases but keyed by index. Detect by SPIR-V reflection in
      Phase 7.
- [ ] **Phase 5d — `Original` / `OriginalSize`:** every pass should be
      able to bind the *original* input image as a sampler named
      `Original` and read `OriginalSize` from push constants. Currently
      `OriginalSize` is set in push but no descriptor binding exists.

**Verified:** 2-pass `double_invert.slangp` produces byte-perfect
identity from a 16×16 RGB gradient (every pass writes through the
chain correctly). 4-pass shaders without alias references would also
work. Real libretro shader `image-adjustment.slangp` (single pass with
`#include`) compiles and runs.

---

## Phase 6 — `PassFeedback<n>` and `OriginalHistory<n>` *(not started)*

- [ ] Detect at compile time which passes have feedback consumers
      (search SPIR-V reflection — Phase 7 dep — for samplers named
      `PassFeedback<n>` / `<alias>Feedback`).
- [ ] Per feedback-producing pass: extend `pass_state` to a 2-deep ring
      of `(out_img, out_view)`. Per frame, this frame's pass output goes
      to slot[parity], next frame's consumer reads slot[parity ^ 1].
      Maintain a `frame_parity` bit that flips at the end of each
      `pipeline_run`.
- [ ] For consumers: extra sampler binding per declared `PassFeedback<n>`
      ref, descriptor written to point at slot[parity ^ 1] before each
      frame.
- [ ] Detect history depth needed for `OriginalHistory<n>`. Allocate
      ring of N+1 input snapshots; rotate.

**Demo:** `crt/newpixie-crt.slangp` runs end-to-end with a real
multi-frame accumulator (ghosting trails decay over many frames, not
just one). Frame-perfect parity vs RetroArch.

---

## Phase 7 — Push constants and UBO completion *(not started — top priority)*

- [ ] **SPIR-V reflection.** Parse the SPIR-V binary (manually — search
      for `OpDecorate <id> Offset N` on push-constant block members)
      to discover, per shader stage:
        - The push-constant block size + the offset of every member.
        - The UBO block size + member offsets.
        - The actual binding numbers used by every sampler.
      Store on `slang_module.push_fields[]` / `ubo_fields[]` /
      `samplers[]`. (Alternatively vendor SPIRV-Reflect, ~3K LOC, MIT.)
- [ ] Standard fields populated by the host at their *reflected*
      offsets: `MVP, SourceSize, OriginalSize, OutputSize, FrameCount,
      FrameDirection, Rotation, FinalViewportSize, OriginalHistorySize<n>,
      PassOutputSize<n>, PassFeedbackSize<n>, <alias>Size`.
- [ ] `#pragma parameter` declarations: defaults applied at startup;
      runtime overrides via `--params 'name=value,...'` get bound into
      the right push-constant offset per pass.

**Demo:** all knobs of `newpixie-crt` runtime-tunable. `image-adjustment`
produces correct gamma/saturation/contrast output instead of the all-zero
debug output we get today.

**Why this is currently the bottleneck:** without offsets, the host
guesses the layout and writes through one struct shape; the shader
declares its own. Mismatched offsets mean shader-side reads return
zero / garbage, which manifests as "shader runs but output looks wrong"
on every shader whose Push block isn't `vec4 SourceSize, OriginalSize,
OutputSize; uint FrameCount`.

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

## Phase 10 — Polish + parity sweep + libavfilter patch

- [ ] Run a representative subset of `libretro/slang-shaders` (CRT,
      scanlines, ntsc, sharpen) through both this binary and RetroArch.
      Build a perceptual-diff dashboard.
- [ ] Resolve any remaining behavioral divergences from the reference.
- [ ] Wrap the same `slang_pipeline` core into a `libavfilter` filter
      (`vf_slang.c` re-using all of `slangp.c`, `slang_compile.c`,
      `slang_pipeline.c`). The pipeline code stays the same; only the
      frame-source/sink shell changes from stdin/stdout to
      `AVFilterContext`. Submit upstream patchset; precedent:
      `vf_libplacebo`, `vf_vulkan`, `vf_ngl`.

**Demo:** `vf_slang` ships in ffmpeg HEAD as a default-built filter on
systems where Vulkan + shaderc are detected by `configure`. The
standalone binary remains as a pip-installable / homebrew-able tool for
people on older ffmpeg versions.

---

## Out of scope (for now)

- Compute shader passes (libretro slang allows them; rare in the wild).
- HLSL / cg shader support (libretro has those formats too, but they're
  legacy; slang covers everything new).
- GUI / preset editor.
- Real-time preview UI.
