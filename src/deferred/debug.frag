#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0, input_attachment_index = 0) uniform subpassInput inAlbedo;
layout(set = 0, binding = 1, input_attachment_index = 1) uniform subpassInput inNormal;
layout(set = 0, binding = 2, input_attachment_index = 2) uniform subpassInput inSSAO;
layout(set = 0, binding = 3, input_attachment_index = 3) uniform subpassInput inSSR;
layout(set = 0, binding = 4, input_attachment_index = 4) uniform subpassInput inDepth;
layout(set = 0, binding = 5) uniform sampler2D bloomTex;

void main() {
}

