// Phase 5b test: pass 1 reads pass 0's output via alias (`step1`),
// NOT via the Source name. If alias resolution is wired correctly,
// double-invert via two passes still cancels back to identity.

#version 450

layout(std140, set = 0, binding = 0) uniform UBO {
    mat4 MVP;
} global;

layout(push_constant) uniform Push {
    vec4 SourceSize;
    vec4 OriginalSize;
    vec4 OutputSize;
    uint FrameCount;
} params;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vUV;
void main() { gl_Position = global.MVP * Position; vUV = TexCoord; }

#pragma stage fragment
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D step1;   // alias from preset
void main() {
    vec4 c = texture(step1, vUV);
    FragColor = vec4(1.0 - c.rgb, c.a);
}
