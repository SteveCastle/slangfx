/*
 * slangfx-web — fullscreen blit helper.
 *
 * WebGPU has no vkCmdBlitImage; a trivial textured fullscreen triangle
 * stands in for it. Used for: presenting the final pass to the canvas
 * (aspect-fit), and generating mip chains for `mipmap_input` passes.
 *
 * Texture-space invariant used across the engine: row 0 of every texture is
 * the TOP of the image. NDC y=+1 maps to viewport row 0 in WebGPU, so the
 * fullscreen geometry samples v = (1 - ndc.y) / 2.
 */

const BLIT_WGSL = /* wgsl */ `
struct VSOut {
  @builtin(position) pos : vec4<f32>,
  @location(0) uv : vec2<f32>,
};

@vertex
fn vs(@builtin(vertex_index) i : u32) -> VSOut {
  var p = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -3.0),
    vec2<f32>( 3.0,  1.0),
    vec2<f32>(-1.0,  1.0),
  );
  var out : VSOut;
  out.pos = vec4<f32>(p[i], 0.0, 1.0);
  out.uv = vec2<f32>((p[i].x + 1.0) * 0.5, (1.0 - p[i].y) * 0.5);
  return out;
}

@group(0) @binding(0) var src : texture_2d<f32>;
@group(0) @binding(1) var smp : sampler;

@fragment
fn fs(in : VSOut) -> @location(0) vec4<f32> {
  return textureSample(src, smp, in.uv);
}
`;

export class Blitter {
  constructor(device) {
    this.device = device;
    this.module = device.createShaderModule({ label: 'slangfx blit', code: BLIT_WGSL });
    this.layout = device.createBindGroupLayout({
      entries: [
        { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: {} },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: {} },
      ],
    });
    this.pipelineLayout = device.createPipelineLayout({ bindGroupLayouts: [this.layout] });
    this.pipelines = new Map(); // format -> GPURenderPipeline
    this.linearSampler = device.createSampler({ magFilter: 'linear', minFilter: 'linear' });
    this.nearestSampler = device.createSampler({ magFilter: 'nearest', minFilter: 'nearest' });
  }

  pipelineFor(format) {
    let p = this.pipelines.get(format);
    if (!p) {
      p = this.device.createRenderPipeline({
        label: `slangfx blit ${format}`,
        layout: this.pipelineLayout,
        vertex: { module: this.module, entryPoint: 'vs' },
        fragment: { module: this.module, entryPoint: 'fs', targets: [{ format }] },
        primitive: { topology: 'triangle-list' },
      });
      this.pipelines.set(format, p);
    }
    return p;
  }

  bindGroup(view, sampler) {
    return this.device.createBindGroup({
      layout: this.layout,
      entries: [
        { binding: 0, resource: view },
        { binding: 1, resource: sampler ?? this.linearSampler },
      ],
    });
  }

  /** Draw `view` over the full target. */
  blit(encoder, view, targetView, targetFormat, { sampler, clear = true, viewport = null } = {}) {
    const pass = encoder.beginRenderPass({
      colorAttachments: [{
        view: targetView,
        loadOp: clear ? 'clear' : 'load',
        storeOp: 'store',
        clearValue: { r: 0, g: 0, b: 0, a: 1 },
      }],
    });
    pass.setPipeline(this.pipelineFor(targetFormat));
    pass.setBindGroup(0, this.bindGroup(view, sampler));
    if (viewport) pass.setViewport(viewport.x, viewport.y, viewport.w, viewport.h, 0, 1);
    pass.draw(3);
    pass.end();
  }

  /** Generate mips level 1..n by blitting each level from the previous. */
  generateMips(encoder, texture, format, mipLevelCount) {
    for (let level = 1; level < mipLevelCount; level++) {
      const srcView = texture.createView({ baseMipLevel: level - 1, mipLevelCount: 1 });
      const dstView = texture.createView({ baseMipLevel: level, mipLevelCount: 1 });
      this.blit(encoder, srcView, dstView, format, { sampler: this.linearSampler });
    }
  }
}
