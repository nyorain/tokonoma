#version 450

#extension GL_GOOGLE_include_directive : enable
#include "fxaa.glsl"
#include "scene.glsl"
#include "tonemap.glsl"

const uint flagFXAA = (1 << 0u);
const uint flagDOF = (1 << 1u);
const uint flagLens = (1 << 2u);

const uint tonemapClamp = 0u;
const uint tonemapReinhard = 1u;
const uint tonemapUncharted2 = 2u;
const uint tonemapACES = 3u;
const uint tonemapHeijlRichard = 4u;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(set = 0, binding = 1) uniform sampler2D ldepthTex;
layout(set = 0, binding = 2) uniform sampler2D lensTex;
layout(set = 0, binding = 3) uniform sampler2D dirtTex;
layout(set = 0, binding = 4) uniform Params {
	uint flags;
	uint tonemap;
	float exposure;
	float dofFocus;
	float dofStrength;
	float lensStrength;
} params;

vec3 tonemap(vec3 x) {
	switch(params.tonemap) {
		case tonemapClamp: return clamp(x, 0.0, 1.0);
		case tonemapReinhard: return vec3(1.0) - exp(-x);
		case tonemapUncharted2: return uncharted2tonemap(x);
		case tonemapACES: return acesTonemap(x);
		case tonemapHeijlRichard: return hejlRichardTonemap(x);
		default: return vec3(0.0); // invalid
	}
}

void main() {
	vec3 color;
	if((params.flags & flagFXAA) != 0) {
		color = fxaa(colorTex, gl_FragCoord.xy).rgb;
	} else {
		color = texture(colorTex, uv).rgb;
	}

	vec2 texelSize = 1.0 / textureSize(colorTex, 0);

	// TODO: first basic depth of field idea, just playing around,
	// this isn't a real thing.
	// TODO: we don't want to blur the sky but for that we need
	// to know the far plane; second condition. Slight optimization
	// as well, could probably be applied to most other pp algorithms
	// TODO: probably best to split off to own pass. Also implement
	// it correctly, with plausible coc and better blurring of everything.
	if((params.flags & flagDOF) != 0 /*&& cd >= scene.far*/) {
		const float focus = params.dofFocus;
		const int range = 3;
		const float eps = 0.01;
		float total = 1;
		vec3 accum = color;
		float cd = texture(ldepthTex, uv).r;

		for(int x = -range; x <= range; ++x) {
			for(int y = -range; y <= range; ++y) {
				// TODO: why don't we weigh the center pixel?
				if(x == 0 && y == 0) { // already added that
					continue;
				}

				vec2 off = texelSize * vec2(x, y);
				float depth = texture(ldepthTex, uv + off).r;
				if(cd < depth - eps) {
					continue;
				}

				float fac;
				if(depth < focus) {
					fac = clamp((focus - depth) / (2 * depth), 0, 1);
					// fac = pow(fac, 0.25);
				} else {
					// fac = 1 for dist = 10
					float x = 0.1 * (depth - focus);
					fac = clamp(pow(x, 2), 0, 1);
				}
				fac = clamp(fac * params.dofStrength, 0, 1);
				fac *= pow(1.f / (abs(x) + 1), 0.1);
				fac *= pow(1.f / (abs(y) + 1), 0.1);
				total += fac;
				accum += fac * texture(colorTex, uv + off).rgb;
			}
		}

		color = accum / total;
	}

	// add lens flar
	if((params.flags & flagLens) != 0) {
		vec3 dirt = params.lensStrength * texture(dirtTex, uv).rgb;
		color += dirt * texture(lensTex, uv).rgb;
	}

	// mark center
	vec2 dist = textureSize(colorTex, 0) * abs(uv - vec2(0.5));	
	if(max(dist.x, dist.y) < 2) {
		color = 1 - color;
	}

	fragColor = vec4(tonemap(color.rgb * params.exposure), 1.0);
	// fragColor = vec4(tonemap(color.rgb), 1.0);
}
