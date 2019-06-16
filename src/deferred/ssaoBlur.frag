#version 450

#extension GL_GOOGLE_include_directive : enable
#include "ssaoBlur.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out float blurred;

void main() {
	blurred = ssaoBlur(uv);
}
