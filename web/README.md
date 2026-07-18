# slangfx-web

Run libretro-format **slang shader** chains in the browser, on **WebGPU**,
with the shader toolchain running as **WebAssembly**. This is the web/wasm
counterpart of the native Vulkan `slangfx` binary, packaged as a reusable ES
module (`slangfx-web`) plus a proof-of-concept UI that mirrors
`wrappers/slangfx_live.py`.

All 37 bundled presets under [`shaders/`](../shaders) compile and render in
Chrome — including multi-pass chains, half-res blur passes, `PassFeedback`
motion effects, and float framebuffers.

## Try it

Hosted demo (deployed from `main` by `.github/workflows/pages.yml`):
**<https://SteveCastle.github.io/slangfx/web/demo/>**

Locally:

```bash
cd web
npm install                      # once — installs @webgpu/glslang
npm run manifest                 # regenerate demo/effects.json after adding shaders
npm run serve                    # → http://localhost:8788/web/demo/
```

Open the URL in Chrome/Edge 113+ (any browser with WebGPU), drop in a video
or image, and stack effects from the **Add layer…** menu. Sliders update
live with no rebuild; PNG frame export and WebM recording are built in.

Run the offline compile test (no browser needed — the same wasm toolchain
runs under Node):

```bash
npm test        # compiles every bundled preset .slang → SPIR-V → WGSL
```

## Architecture — and where WebGPU and WASM each fit

The native engine is C on Vulkan. Browsers have no Vulkan, and WebAssembly
has no GPU access of its own — *any* GPU work from wasm goes through
WebGPU, which is deliberately Vulkan-shaped (bind groups ≈ descriptor sets,
render passes, explicit pipelines). So compiling the C pipeline to wasm
would buy nothing: the ~2k lines of Vulkan calls would just become the same
calls into WebGPU. **WebGPU is the port target for the runtime; WASM is the
port target for the toolchain.**

What actually runs as WebAssembly is the hard, battle-tested part — the
shader compiler chain, the same components the native build uses via
shaderc:

```
.slangp preset ──► parser (JS port of slangp.c)
.slang source ──► preprocess (JS port of slang_compile.c preprocessing
                  + WebGPU rewrites, see below)
        GLSL 450 ──► glslang.wasm ──► SPIR-V ──► twgsl/tint.wasm ──► WGSL
                          │
                          └──► SPIR-V reflection (JS port of spv_reflect.c)
                               → uniform offsets used every frame
```

Shaders compile **in the browser at runtime** — arbitrary `.slangp` presets
work without any offline build step, exactly like the native binary.

The runtime (`src/engine.js`) reimplements `slang_pipeline.c` semantics on
WebGPU: per-pass framebuffers sized by slangp scale rules, `Source` /
`Original` / `Pass<n>` / alias / `PassFeedback<n>` binding, standard
uniforms (`SourceSize`, `OutputSize`, `FrameCount`, `Time`, …) written at
reflected offsets, feedback snapshot copies, mip-chain generation for
`mipmap_input`, and GPU-side layer chaining (layer *i+1* samples layer
*i*'s output texture directly — one command submit per frame, no copies,
mirroring the native `slang_chain_run`).

### GLSL → WGSL rewrites

WebGPU/WGSL differs from Vulkan GLSL in ways the preprocessor bridges
before glslang ever sees the source:

| Vulkan feature | WebGPU translation |
|---|---|
| `layout(push_constant) uniform Push {…}` | std140 UBO in **bind group 1** (own group → no binding collisions) |
| combined `sampler2D` | split into `texture2D X_tex` (group 0, declared binding) + `sampler X_smp` (**bind group 2**, declaration order); builtin calls get `sampler2D(X_tex, X_smp)` at point of use, user-function params/args become pairs, `#define` sampler aliases are resolved |
| identifiers reserved in WGSL (`macro`, …) | renamed `X_sfx` consistently through pragmas + reflection |
| `textureSample` in non-uniform control flow (WGSL uniformity rule) | automatic retry compiling `texture()` as explicit base-level `textureLod()` |

### Fidelity limitations vs native

- `clamp_to_border` wrap falls back to `clamp-to-edge` (WebGPU has no
  border color).
- `R16_UNORM` / `R32_SFLOAT`-family framebuffer formats fall back to
  `rgba16float` (not renderable/filterable in baseline WebGPU).
- `OriginalHistory1+` binds the current frame (same as the native engine
  today).
- First-frame feedback clears to opaque black (matches native).

## Embedding in your own app

The engine is UI-free and canvas-optional (headless rendering + pixel
readback work without one). Everything is plain ESM — no bundler required.

```js
import { SlangFx, loadToolchain } from 'slangfx-web';

const toolchain = await loadToolchain();          // wasm modules, cached singleton
const fx = await SlangFx.create({
  canvas,                                          // optional — omit for headless
  toolchain,
  readFile: async (p) => (await fetch('/' + p)).text(),   // how shaders load
});

await fx.setSourceSize(video.videoWidth, video.videoHeight);
await fx.addLayer('shaders/soft-crt/soft-crt.slangp');
await fx.addLayer('shaders/motion-trails/motion-trails.slangp');
fx.setParam(0, 'scan_strength', 0.3);              // live, no rebuild

(function tick() {
  fx.render(video, video.currentTime);             // upload + all layers + present
  requestAnimationFrame(tick);
})();
```

| API | Purpose |
|---|---|
| `SlangFx.create({canvas?, device?, toolchain, readFile, readImage?})` | make an engine; pass your own `GPUDevice` to share one |
| `setSourceSize(w, h)` | size the chain input (rebuilds layers) |
| `addLayer(path)` / `removeLayer(i)` / `moveLayer(i, ±1)` / `toggleLayer(i, on)` / `clearLayers()` | layer stack (structural — rebuilds) |
| `getLayerInfo()` | per-layer params with `{name, desc, min, max, step, default, value}` for building UIs |
| `setParam(layer, name, value)` / `resetParams(layer)` | live parameter updates |
| `render(source?, timeSec?)` | one frame; `source` is any `copyExternalImageToTexture` source (video element, ImageBitmap, canvas, VideoFrame) |
| `readPixels()` / `exportPNG()` | RGBA readback / PNG blob of the processed frame |
| `finalTexture` / `inputTexture` | raw `GPUTexture`s for custom integration |

Lower-level pieces are exported too (`parsePreset`, `compileSlang`,
`reflectSpirv`, …) for tooling like the Node test harness in
`tools/test-compile.mjs`.

## Layout

```
web/
├── src/            the slangfx-web ES module
│   ├── engine.js       WebGPU multi-pass runtime (port of slang_pipeline.c)
│   ├── compiler.js     slang → WGSL orchestration
│   ├── preprocess.js   include flatten + pragma split + WebGPU rewrites
│   ├── slangp.js       .slangp parser (port of slangp.c)
│   ├── spv-reflect.js  SPIR-V reflection (port of spv_reflect.c)
│   ├── blit.js         present/mipmap blit helper
│   ├── toolchain.js    browser wasm loader
│   └── index.js        public exports
├── vendor/         glslang.{js,wasm} (BSD/Apache, from @webgpu/glslang)
│                   twgsl.{js,wasm}  (BSD-3, tint build from Babylon.js CDN)
├── demo/           proof-of-concept UI (index.html, app.js, effects.json)
└── tools/          serve.mjs, test-compile.mjs, build-manifest.mjs
```
