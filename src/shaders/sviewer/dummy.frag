#version 450
#extension GL_GOOGLE_include_directive : enable

#include "math.glsl"

#include "geometry.glsl"
#include "noise.glsl"

layout(location = 0) in vec2 inuv;
layout(location = 0) out vec4 outcol;

layout(set = 0, binding = 0) uniform UBO {
	vec2 mpos;
	float time;
	uint effect;
} ubo;

float mfbm(vec2 st) {
	// const mat2 mtx = mat2(6.4, fbm(vec2(cos(0.3 * ubo.time), 7 * sin(0.3241 * ubo.time))), 1, 2);
	const mat2 mtx = mat2( 1, 0, 0, 1);

	float sum = 0.f;
	float lacunarity = 1.1;
	float gain = 0.5;

	float amp = 0.5f; // ampliture
	float mod = 1.f; // modulation
	for(int i = 0; i < FBM_OCTAVES; ++i) {
		// sum += amp * valueNoise(2 * random(mod * amp) + mod * st);
		sum += amp * gradientNoise(st);
		// sum += amp * FBM_NOISE(mod * st);
		// mod *= lacunarity;
		amp *= gain;
		// st = lacunarity * mtx * st;
		st = cos(ubo.time) * fbm(sin(ubo.time + fbm(amp * st) * ubo.time) * sum * st) * st;
	}

	return sum;
}

void main() {
	vec2 uv = 2 * inuv - 1;
	uv = 10 * inuv;

	float d = 0.f;
	vec3 rgb = vec3(1.0);
	int counter = 0;
	if(ubo.effect == counter++) {
		d = valueNoise(uv);
	} else if(ubo.effect == counter++) {
		d = gradientNoise(uv);
	} else if(ubo.effect == counter++) {
		d = fbm(uv);
	} else if(ubo.effect == counter++) {
		float v1 = fbm(uv);
		float v2 = fbm(v1 * uv);
		d = v2;
		rgb.r = v1;
		rgb.g = v2;
		rgb.b = v1 * v2;
	} else if(ubo.effect == counter++) {
		d = fbm(2 * uv + fbm(uv + vec2(1, 1) * pow(fbm(uv), 2)));
		d = sin(fbm(d * vec2(sin(uv.x), cos(uv.y))));
		d = fbm(150 * vec2(sin(d), cos(d)) * fbm(vec2(d, -d)));
	} else if(ubo.effect == counter++) {
		d = fbm(uv + mfbm(uv + fbm(uv)));
	}

	outcol = d * vec4(rgb, 1.0);
	outcol.rgb = pow(outcol.rgb, vec3(2.2));
}
