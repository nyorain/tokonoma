#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform Params {
	float exposure;
} params;

void main() {
	float accum = texture(sceneTex, uv).r;

	// tonemap
	// float col = 1 - exp(-params.exposure * accum);
	float col = exp(-params.exposure * accum);
	fragColor = vec4(vec3(col), 1.0);
}
