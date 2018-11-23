#version 450
#extension GL_GOOGLE_include_directive : enable

#include "geometry.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outcol;

layout(set = 0, binding = 0) uniform UBO {
	vec2 mpos;
	float time;
} ubo;

float curve(float val, float slope, vec2 uv) {
	Line tangent = {vec2(uv.x, val), vec2(1, slope)};
	Line normal = {uv, normalize(vec2(-slope, 1))};
	float dst = intersectionFacs(tangent, normal).y;
	return smoothstep(-0.1, 0, dst) - smoothstep(0, 0.1, dst);
}

void main() {
	// outcol = vec4(uv * ubo.mpos, 0.5 + 0.5 * sin(ubo.time), 1.0);
	// outcol = vec4(1, 1, 1, 1);
	// outcol *= smoothstep(0, 1, uv.x);
	// outcol *= exp(-1 / uv.x);
	// outcol *= sin(uv.x);

	float x = 10 * uv.x;
	float y = 3 * uv.y - 1.5;

	outcol = vec4(curve(sin(x), cos(x), vec2(x, y)));
}

