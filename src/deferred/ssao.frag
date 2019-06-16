#version 450

#extension GL_GOOGLE_include_directive : enable
#include "ssao.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out float outSSAO;

void main() {
	outSSAO = computeSSAO(uv);
}
