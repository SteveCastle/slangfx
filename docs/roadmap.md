---
title: Roadmap
---

# Roadmap

Phased plan toward a slang shader runtime that can apply
`crt/newpixie-crt.slangp` (plus most of `libretro/slang-shaders`) with
output indistinguishable from RetroArch's vulkan driver. Phases 0–7 are
done; the standalone `slangfx` binary is what you build today. Phases 8–10
are deferred work — see [Limitations]({{ '/limitations.html' | relative_url
}}) for what that means in practice.

The phases are sized for measurable, mergeable chunks. Each ends with a
demo and a checked-in test fixture.

---

## Phase 0 — Scaffold *(done)*

- [x] Repo layout, README, LICENSE, .gitignore, .gitattributes
- [x] `meson.build` (Vulkan + shaderc gated behind `-Denable_gpu=true`).
- [x] Source stubs with documented interfaces.
- [x] `docs/architecture.md`, `docs/roadmap.md`, `docs/slang_format.md`
- [x] `wrappers/slangfx.py` for ffmpeg ↔ slangfx ↔ ffmpeg orchestration.

**Demo:** `meson setup build && meson compile -C build` produces a working
`slangfx` binary. It reads RGBA frames from stdin and writes them to
stdout. `slangfx --help` prints usage. `wrappers/slangfx.py -i in.mp4
--preset foo.slangp -o out.mp4` runs end-to-end.

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

## Phase 5 — Multi-pass chain *(done, all sub-phases)*

- [x] Per-pass `pass_state` holding its own framebuffer image, render
      pass, descriptor set, pipeline, sampler.
- [x] Per-pass framebuffer dimensions resolved from `scale_type[_x|_y]`
      and `scale[_x|_y]` (source / viewport / absolute, per axis).
- [x] Per-pass sampler honors `filter_linear` and `wrap_mode`.
- [x] Source chaining: pass N's `Source` descriptor binds pass N-1's
      output view (or original input for pass 0).
- [x] **Phase 5b — alias bindings:** preset `alias<i> = name` builds an
      alias→pass-index map; reflected sampler names matching aliases
      get bound to that pass's output view.
- [x] **Phase 5c — `Pass<n>`:** numeric pass refs resolved by parsing
      digits after `Pass`, indexing `passes[n].out_view` (only for
      n < current_pass_idx; self-references rejected).
- [x] **Phase 5d — `Original` / `OriginalSize`:** sampler named
      `Original` (and `OriginalHistory0`) binds the original input
      image. `OriginalSize` already populated in push constants.

**Verified:** `alias_test.slangp` (pass 1 reads pass 0 by alias
`step1`) produces correct double-invert (red → cyan → red).
`double_invert.slangp` (Source chain) byte-perfect identity over
gradient. Real libretro `image-adjustment.slangp` (with `#include`)
runs.

---

## Phase 6 — `PassFeedback<n>` *(done)*

- [x] Detection during sampler resolution (Phase 5/Phase 7): names
      matching `PassFeedback<n>` or `<alias>Feedback` mark the producer
      pass as `is_feedback_producer` and store the producer index on
      the consumer's resolved binding.
- [x] Per feedback-producing pass: a `feedback_img` snapshot that the
      next frame's consumers sample from. Implemented as a copy
      (vkCmdCopyImage out_img → feedback_img at end of frame) instead
      of a parity-flipped ring, for code simplicity. Adds one image
      copy per producer per frame.
- [x] First-frame initialization: every feedback image is cleared to
      opaque-black + transitioned to SHADER_READ_ONLY before any pass
      runs on frame 0, so consumers reading PassFeedback<n> at frame 0
      get deterministic zeros instead of UB.
- [x] Layout transition coordination: end-of-frame snapshot transitions
      out_img → TRANSFER_SRC, feedback_img → TRANSFER_DST, runs the
      copy, then restores both. Last pass coordinates with the readback
      path so `out_img` only transitions once.
- [ ] **Deferred: `OriginalHistory<n>`** for n>0. The resolver currently
      treats all OriginalHistory* as the current input frame; a future
      pass adds an N-deep input snapshot ring sized from the highest
      n the corpus references.

**Verified:** `feedback.slang` (accumulator with 0.7-decay multiplier).
Sequence: white frame → 5 black frames. Output: `255 → 178 → 125 → 87
→ 61 → 43`. Each frame is exactly the previous output × 0.7 rounded
down. **Multi-frame compounding accumulator works** — this is the slang
PassFeedback semantics that ffmpeg's libplacebo filter could not
represent at all.

---

## Phase 7 — Push constants + UBO completion *(done)*

- [x] **SPIR-V reflection** in `src/spv_reflect.{h,c}`: walks the
      binary directly (no external dep), tracks `OpName /
      OpMemberName / OpDecorate / OpMemberDecorate / OpTypePointer /
      OpTypeStruct / OpVariable`, emits push block (size + member name +
      offset), UBO block, and sampler bindings. ~430 LOC.
- [x] `slang_compile.c` calls reflection on the fragment SPIR-V and
      populates `slang_module.push_fields[]`, `ubo_fields[]`,
      `samplers[]`. Each `#pragma parameter` is matched by name to a
      push field; `params[i].push_offset` is set accordingly.
- [x] **shaderc preserves debug info.** Crucial subtlety: with
      `optimization_level_performance` shaderc strips OpName /
      OpMemberName, leaving reflection unable to match parameters by
      name. Now using `optimization_level_zero` plus
      `set_generate_debug_info` so names survive into emitted SPIR-V.
      Optimization can be moved to a later post-pass over the SPIR-V if
      perf becomes a concern.
- [x] Pipeline rebuilt to write the push-constant blob honoring the
      shader's reflected layout. Standard fields (`SourceSize`,
      `OriginalSize`, `OutputSize`, `FinalViewportSize`, `FrameCount`,
      `FrameDirection`, `Rotation`) are written if-and-where the shader
      declared them; `#pragma parameter` defaults are written at their
      resolved offsets.
- [x] `SLANGFX_DEBUG_REFLECT=1` env var dumps the per-shader reflection
      result to stderr for debugging.
- [ ] **Deferred to Phase 7b:** runtime overrides via
      `--params 'name=value,...'`. The plumbing is in place
      (`main.c` already has `--params`); just needs to parse the
      string + write into `push_fields[]` lookup.

**Verified:**
- Synthetic test: `tests/fixtures/brightness.slang` (parameter
  `brightness` default 0.5) — red 255 → 127 output, exactly 255 × 0.5
  rounded down. Without Phase 7 this was 0.
- Real libretro shader `image-adjustment.slangp` — all 24 push members
  reflected (SourceSize, FrameCount, plus 22 `ia_*` parameters).
  Mid-gray input is preserved through the default identity transform,
  3008/3072 output RGB bytes non-zero (vs 0/3072 in Phase 5).

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
      (a new `vf_slang.c` re-using all of `slangp.c`, `slang_compile.c`,
      `slang_pipeline.c`). The pipeline code stays the same; only the
      frame-source/sink shell changes from stdin/stdout to
      `AVFilterContext`. Submit upstream patchset; precedent:
      `vf_libplacebo`, `vf_vulkan`, `vf_ngl`.

**Demo:** `vf_slang` ships in ffmpeg HEAD as a default-built filter on
systems where Vulkan + shaderc are detected by `configure`. The
`slangfx` standalone binary remains as a pip-installable /
homebrew-able tool for people on older ffmpeg versions.

---

## Out of scope (for now)

- Compute shader passes (libretro slang allows them; rare in the wild).
- HLSL / cg shader support (libretro has those formats too, but they're
  legacy; slang covers everything new).
- GUI / preset editor.
- Real-time preview UI.
