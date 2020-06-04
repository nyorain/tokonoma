#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D tex;

void main() {
	outFragColor = clamp(texture(tex, inUV), 0.0, 1.0);
}

