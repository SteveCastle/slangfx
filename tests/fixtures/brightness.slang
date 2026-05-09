// Phase 7 test: parameter default value must be applied via reflected offset.
// brightness defaults to 0.5 — input red (255,0,0) should come out as (128,0,0).
// If reflection / push-offset writing is broken, brightness reads 0 and the
// output is black; if it works it's mid-gray red.

#version 450

layout(std140, set = 0, binding = 0) uniform UBO {
    mat4 MVP;
} global;

layout(push_constant) uniform Push {
    vec4  SourceSize;
    vec4  OriginalSize;
    vec4  OutputSize;
    uint  FrameCount;
    float brightness;
} params;

#pragma parameter brightness "Brightness" 0.5 0.0 4.0 0.05

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vUV;
void main() { gl_Position = global.MVP * Position; vUV = TexCoord; }

#pragma stage fragment
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
void main() {
    vec4 c = texture(Source, vUV);
    FragColor = vec4(c.rgb * params.brightness, c.a);
}
