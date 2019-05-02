#version 450

#extension GL_GOOGLE_include_directive : enable
#include "fxaa.glsl"

const uint flagSSR = (1u << 2u);
const uint flagFXAA = (1u << 4u);
const uint flagBloom = (1u << 3u);

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform Params {
	uint flags;
	uint tonemap;
	float exposure;
	uint bloomLevels;
} params;

layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D ssrTex;
layout(set = 0, binding = 3) uniform sampler2D bloomTex;

// http://filmicworlds.com/blog/filmic-tonemapping-operators/
// has a nice preview of different tonemapping operators
vec3 uncharted2map(vec3 x) {
	const float A = 0.15;
	const float B = 0.50;
	const float C = 0.10;
	const float D = 0.20;
	const float E = 0.02;
	const float F = 0.30;
	return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

vec3 uncharted2tonemap(vec3 x, float exposure) {
	const float W = 11.2; // whitescale
	x = uncharted2map(x * params.exposure);
	return x * (1.f / uncharted2map(vec3(W)));
}

vec3 tonemap(vec3 x) {
	switch(params.tonemap) {
		case 0: return x;
		case 1: return vec3(1.0) - exp(-x * params.exposure); // simple
		case 2: return uncharted2tonemap(x, params.exposure);
		default: return vec3(0.0); // invalid
	}
}

void main() {
	vec4 color;
	if((params.flags & flagFXAA) != 0) {
		color = fxaa(colorTex, gl_FragCoord.xy);
	} else {
		color = texture(colorTex, uv);
	}

	// ssr
	if((params.flags & flagSSR) != 0) {
		vec4 refl = textureLod(ssrTex, uv, 0);
		vec2 ruv = refl.xy;
		if(refl.xy != vec2(0.0)) {
			float fac = refl.w;

			// NOTE: this can get really expensive. We should probably
			// do this in shared memory compute shader. We can't use two-pass
			// blurring since we want to blur *every pixel with a different
			// strength*
			// TODO: when blurring, we *must* not blur over depth edges,
			// otherwise we get artefacts. That means we need the depth
			// texture here as well
			// int range = int(clamp(10.0 * refl.z, 0.0, 5.0));
			int range = 0;
			vec3 light = vec3(0.0);
			vec2 texelSize = 1.0 / textureSize(colorTex, 0);
			for(int x = -range; x <= range; ++x) {
				for(int y = -range; y <= range; ++y) {
					vec2 off = texelSize * vec2(x, y);
					vec2 uvo = clamp(ruv + off, 0.0, 1.0);
					light += textureLod(colorTex, uvo, 0).rgb;
				}
			}

			light /= (2 * range + 1) * (2 * range + 1);

			// make reflections weaker when near image borders
			// to avoid popping in
			vec2 sdist = 1 - 2 * abs(vec2(0.5, 0.5) - ruv);
			fac *= pow(sdist.x * sdist.y, 0.8);
			color.rgb += fac * light;
		}
	}

	// apply bloom
	// first level was already applied in combine.frag
	if((params.flags & flagBloom) != 0) {
		float bfac = 1.f;
		uint bloomLevels = params.bloomLevels;
		vec3 bloomSum = vec3(0.0);
		for(uint i = 1u; i < bloomLevels; ++i) {
			float fac = bfac / (1 + i);
			bloomSum += fac * textureLod(bloomTex, uv, i).rgb;
		}

		color.rgb += bloomSum;
	}

	fragColor = vec4(tonemap(color.rgb), 1.0);
}
