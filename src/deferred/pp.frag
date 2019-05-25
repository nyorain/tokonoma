#version 450

#extension GL_GOOGLE_include_directive : enable
#include "fxaa.glsl"
#include "scene.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform Params {
	uint mode;
	uint flags;
	float aoFactor;
	float ssaoPow;
	uint tonemap;
	float exposure;
	uint _convolutionLods;
	float bloomStrength;
	float scatterStrength;
	float dofFocus;
	float dofStrength;
} params;

layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D ssrTex;
layout(set = 0, binding = 3) uniform sampler2D bloomTex;
layout(set = 0, binding = 4) uniform sampler2D depthTex;
layout(set = 0, binding = 5) uniform sampler2D scatterTex;

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

vec3 uncharted2tonemap(vec3 x) {
	const float W = 11.2; // whitescale
	x = uncharted2map(x);
	return x * (1.f / uncharted2map(vec3(W)));
}

// Hejl Richard tone map
// http://filmicworlds.com/blog/filmic-tonemapping-operators/
vec3 hejlRichardTonemap(vec3 color) {
    color = max(vec3(0.0), color - vec3(0.004));
    return pow((color*(6.2*color+.5))/(color*(6.2*color+1.7)+0.06), vec3(2.2));
}

// ACES tone map
// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
vec3 acesTonemap(vec3 color) {
    const float A = 2.51;
    const float B = 0.03;
    const float C = 2.43;
    const float D = 0.59;
    const float E = 0.14;
    return clamp((color * (A * color + B)) / (color * (C * color + D) + E), 0.0, 1.0);
}

vec3 tonemap(vec3 x) {
	switch(params.tonemap) {
		case 0: return x;
		case 1: return vec3(1.0) - exp(-x * params.exposure); // simple
		case 2: return uncharted2tonemap(x * params.exposure);
		case 3: return hejlRichardTonemap(x * params.exposure);
		case 4: return acesTonemap(x * params.exposure);
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

	vec2 texelSize = 1.0 / textureSize(colorTex, 0);

	// important that this comes before scattering and ssr
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
		vec3 accum = color.rgb;
		float cd = texture(depthTex, uv).r;

		for(int x = -range; x <= range; ++x) {
			for(int y = -range; y <= range; ++y) {
				if(x == 0 && y == 0) {
					continue;
				}

				vec2 off = texelSize * vec2(x, y);
				float depth = texture(depthTex, uv + off).r;
				if(cd < depth - eps) {
					continue;
				}

				float fac;
				if(depth < focus) {
					fac = clamp(0.05 + (focus - depth) / focus, 0, 1);
					fac = pow(fac, 2);
				} else {
					// fac = 1 for dist = 4
					float x = 0.25 * (depth - focus);
					fac = clamp(pow(x, 2), 0, 1);
				}
				fac = clamp(fac * params.dofStrength, 0, 1);
				fac *= pow(1.f / (abs(x) + 1), 0.2);
				fac *= pow(1.f / (abs(y) + 1), 0.2);
				total += fac;
				accum += fac * texture(colorTex, uv + off).rgb;
			}
		}

		color.rgb = accum / total;
	}


	// ssr
	if((params.flags & flagSSR) != 0) {
		vec4 refl = textureLod(ssrTex, uv, 0);
		vec2 ruv = refl.xy / textureSize(ssrTex, 0);
		if(refl.xy != vec2(0.0)) {
			float fac = refl.w;

			float centerDepth = textureLod(depthTex, ruv, 0).r;
			const float dthreshold = 0.1;

			// NOTE: this can get really expensive. We should probably
			// do this in shared memory compute shader. We can't use two-pass
			// blurring since we want to blur *every pixel with a different
			// strength*
			// TODO: when blurring, we *must* not blur over depth edges,
			// otherwise we get artefacts. That means we need the depth
			// texture here as well
			// TODO: when using high range we could just use high lod.
			// Currently don't have mipmap of lights though, could
			// generate if before pp pass...
			int range = int(clamp(2 * refl.z, 0.0, 5.0));
			// int range = 0;
			int lod = 0;
			vec3 light = vec3(0.0);
			float total = 0.0;
			for(int x = -range; x <= range; ++x) {
				for(int y = -range; y <= range; ++y) {
					vec2 off = texelSize * vec2(x, y);
					vec2 uvo = clamp(ruv + off, 0.0, 1.0);
					if(uvo != clamp(uvo, 0.0, 1.0)) {
						continue;
					}

					float sampleDepth = textureLod(depthTex, uvo, lod).r;
					float diff = abs(sampleDepth - centerDepth);
					// TODO: use gaussian filter (just use function manually
					// instead of precomputed)
					float fac = smoothstep(-dthreshold, -0.5 * dthreshold, -diff);

					light += fac * textureLod(colorTex, uvo, lod).rgb;
					total += fac;
				}
			}

			light /= total;

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
		uint bloomLevels = textureQueryLevels(bloomTex);
		vec3 bloomSum = vec3(0.0);
		for(uint i = 0u; i < bloomLevels; ++i) {
			float fac = params.bloomStrength;
			if((params.flags & flagBloomDecrease) != 0) {
				fac /= (1 + i);
			}
			bloomSum += fac * textureLod(bloomTex, uv, i).rgb;
		}

		color.rgb += bloomSum;
	}


	// apply scattering
	// TODO: blur in different pass? we don't need that strong of blur though
	// probably best to blur here but with a better blur! use linear
	// sampling, maybe the 4+1 textures access blur?
	if((params.flags & flagScattering) != 0) {
		// const float scatterFac = 1 / 25.f;
		const float scatterFac = params.scatterStrength;
		float scatter = 0.f;
		int range = 1;
		vec2 texelSize = 1.f / textureSize(scatterTex, 0);
		for(int x = -range; x <= range; ++x) {
			for(int y = -range; y <= range; ++y) {
				vec2 off = texelSize * vec2(x, y);
				scatter += texture(scatterTex, uv + off).r;
			}
		}

		int total = ((2 * range + 1) * (2 * range + 1));
		scatter /= total;
		color.rgb += scatterFac * scatter * vec3(0.9, 0.8, 0.6); // TODO: should be light color
	}

	// mark center
	vec2 dist = textureSize(colorTex, 0) * abs(uv - vec2(0.5));	
	if(max(dist.x, dist.y) < 2) {
		color.rgb = 1 - color.rgb;
	}

	fragColor = vec4(tonemap(color.rgb), 1.0);
}
