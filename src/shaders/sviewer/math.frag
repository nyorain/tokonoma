#version 450
#extension GL_GOOGLE_include_directive : enable

#include "math.glsl"
#include "color.glsl"

layout(location = 0) in vec2 inuv;
layout(location = 0) out vec4 outcol;

layout(set = 0, binding = 0) uniform UBO {
	vec2 mpos;
	float time;
	uint effect;
};

void main() {
	vec2 uv = inuv;
	float t = 0.f;

	switch(effect) {
		case 0: {
			t = impulse(mpos.x * 3, 5 * uv.x);
			break;
		} case 1: {
			vec2 s = smoothcurve(mpos, vec2(0.05), uv);
			t = pow(s.x * s.y, 4);
			break;
		} case 2: {
			t = expstep(mpos.x * 20, mpos.y * 30, 2 * uv.x);
			break;
		} case 3: {
			// TODO: feels weird
			t = gain(pow(5 * mpos.x, -1 + 2 * mpos.y), uv.x);
			break;
		} case 4: {
			t = pcurve(10 * mpos.x, 10 * mpos.y, uv.x);
			break;
		} case 5: {
			t = abs(sincn(10 * mpos.x, uv.x));
			break;
		} case 6: {
			t = parabola(10 * mpos.x, uv.x);
			break;
		}
	}

	// vec3 col = pal(t, vec3(0.5, 0.5, 0.5), vec3(0.5, 0.5, 0.5), vec3(1, 1, 1), vec3(0, 0.25, 0.5));
	vec3 col = vec3(t);
	col = pow(col, vec3(2.2)); // gamma; we output to srgb
	outcol = vec4(col, 1);
}
