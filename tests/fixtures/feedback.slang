// Phase 6 test: an accumulator that mixes the current frame with a
// decayed copy of THIS pass's previous-frame output (PassFeedback0).
// If feedback works, a bright frame leaves a fading trail in subsequent
// frames; if not, output always equals the current input.

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
layout(set = 0, binding = 2) uniform sampler2D Source;
layout(set = 0, binding = 3) uniform sampler2D PassFeedback0;
void main() {
    vec3 cur  = texture(Source,         vUV).rgb;
    vec3 prev = texture(PassFeedback0,  vUV).rgb;
    FragColor = vec4(max(cur, prev * 0.7), 1.0);
}
