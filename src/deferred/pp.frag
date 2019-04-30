#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

const uint flagSSAO = (1u << 0u);
const uint flagScattering = (1u << 1u);
const uint flagSSR = (1u << 2u);
const uint flagBloom = (1u << 3u);

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj;
	mat4 invProj;
	vec3 viewPos;
	float nearPlane;
	float farPlane;
	uint mode;
} scene;

// layout(set = 1, binding = 0, input_attachment_index = 0)
// 	uniform subpassInput inLight;
layout(set = 1, binding = 0) uniform sampler2D lightTex;
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inAlbedo;
layout(set = 1, binding = 2) uniform UBO {
	vec3 scatterLightColor;
	uint tonemap;
	float aoFactor;
	float ssaoPow;
	float exposure;
	uint flags;
	uint bloomLevels;
} ubo;
layout(set = 1, binding = 3) uniform sampler2D ssaoTex;
layout(set = 1, binding = 4) uniform sampler2D depthTex;
layout(set = 1, binding = 5) uniform sampler2D scatterTex;
layout(set = 1, binding = 6) uniform sampler2D normalTex;
layout(set = 1, binding = 7) uniform sampler2D bloomTex;
layout(set = 1, binding = 8) uniform sampler2D ssrTex;

// http://filmicworlds.com/blog/filmic-tonemapping-operators/
// has a nice preview of different tonemapping operators
// currently using uncharted 2 version
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
	x = uncharted2map(x * ubo.exposure);
	return x * (1.f / uncharted2map(vec3(W)));
}

vec3 tonemap(vec3 x) {
	switch(ubo.tonemap) {
		case 0: return x;
		case 1: return vec3(1.0) - exp(-x * ubo.exposure);
		case 2: return uncharted2tonemap(x, ubo.exposure);
		default: return vec3(0.0); // invalid
	}
}

// TODO: better blurring/filter for ssao/scattering
//  or move at least light scattering to light shader? that would
//  allow it for multiple light sources (using natural hdr) as
//  well as always using the correct light color and attenuation
//  and such
void main() {
	// vec4 color = subpassLoad(inLight);
	vec4 color = texture(lightTex, uv);
	if(scene.mode >= 1 && scene.mode <= 6) {
		// light shader rendered a debug view, don't modify
		// it in any way. Just forward
		fragColor = vec4(color.rgb, 1.0);
		return;
	}

	// scattering
	if((ubo.flags & flagScattering) != 0) {
		float scatter = 0.f;
		int range = 2;
		vec2 texelSize = 1.f / textureSize(scatterTex, 0);
		for(int x = -range; x <= range; ++x) {
			for(int y = -range; y <= range; ++y) {
				vec2 off = texelSize * vec2(x, y);
				scatter += texture(scatterTex, uv + off).r;
			}
		}

		int total = ((2 * range + 1) * (2 * range + 1));
		scatter /= total;
		vec3 scatterColor = scatter * ubo.scatterLightColor;
		if(scene.mode == 7) {
			fragColor = vec4(scatterColor, 1.0);
			return;
		}

		color.rgb += scatterColor;
	}

	// ao
	vec4 albedo = subpassLoad(inAlbedo);
	float ao = ubo.aoFactor * albedo.w;

	if((ubo.flags & flagSSAO) != 0) {
		const float near = scene.nearPlane;
		const float far = scene.farPlane;

		float ssao = 0.f;
		int range = 1;
		vec2 texelSize = 1.f / textureSize(ssaoTex, 0);
		float depth = textureLod(depthTex, uv, 0).r;
		float z = depthtoz(depth, near, far);
		float total = 0.f;

		// TODO: what kind of filter to use?
		// without gaussian it's a simple box filter
		// const float kernel[5][5] = {
		// 	{0.003765,	0.015019,	0.023792,	0.015019,	0.003765},
		// 	{0.015019,	0.059912,	0.094907,	0.059912,	0.015019},
		// 	{0.023792,	0.094907,	0.150342,	0.094907,	0.023792},
		// 	{0.015019,	0.059912,	0.094907,	0.059912,	0.015019},
		// 	{0.003765,	0.015019,	0.023792,	0.015019,	0.003765},
		// }

		// TODO: probably best to bur in 2-step guassian pass
		// then also use linear sampling gauss, see gblur.frag
		for(int x = -range; x <= range; ++x) {
			for(int y = -range; y <= range; ++y) {
				vec2 off = texelSize * vec2(x, y);
				vec2 uvo = clamp(uv + off, 0.0, 1.0);
				float sampleDepth = textureLod(depthTex, uvo, 0.0).r;
				float samplez = depthtoz(sampleDepth, near, far);
				float sampleAo = textureLod(ssaoTex, uvo, 0.0).r;

				// make sure to not blur over edges here
				float fac = smoothstep(0.0, 1.0, 1 - 20 * abs(z - samplez));
				// fac *= kernel[x + range][y + range];
				ssao += fac * sampleAo;
				total += fac;
			}
		}

		// w component of albedo contains texture-based ao factor
		ssao /= total;
		ao *= pow(ssao, ubo.ssaoPow);

		// no blur variant
		// ao *= textureLod(ssaoTex, uv, 0.0).r;
	}

	vec3 aoColor = ao * albedo.rgb;
	if(scene.mode == 8) {
		fragColor = vec4(vec3(ao), 1.0);
		return;
	} else if(scene.mode == 9) {
		fragColor = vec4(aoColor, 1.0);
		return;
	}

	color.rgb += aoColor;

	// ssr
	// TODO: better to do ssr in another pass so it can include ao
	// and light scattering, which is not possible like this.
	// Also move tonemapping in that pass then; basically split
	// post processing into combining and true post processing.
	// The combine pass should also contain the bloom addition.
	if((ubo.flags & flagSSR) != 0) {
		// TODO: blur based on roughness and refl distance.
		vec4 refl = textureLod(ssrTex, uv, 0);
		vec2 ruv = refl.xy;
		if(refl.xy != vec2(0.0)) {
			vec3 light = textureLod(lightTex, ruv, 0).rgb;
			float fac = refl.z;
			// make reflections weaker when near image borders
			// to avoid plopping in
			vec2 sdist = 1 - 2 * abs(vec2(0.5, 0.5) - ruv);
			fac *= pow(sdist.x * sdist.y, 0.8);
			color.rgb += fac * light;
		}
	}

	// add bloom
	uint bloomLevels = 1;
	float bfac = 3.f;
	if((ubo.flags & flagBloom) != 0) {
		bfac = 1.f;
		bloomLevels += ubo.bloomLevels;
	}

	for(uint i = 0u; i < bloomLevels; ++i) {
		float fac = bfac / (1 + i);
		color.rgb += fac * textureLod(bloomTex, uv, i).rgb;
	}

	fragColor = vec4(tonemap(color.rgb), 1.0);
}
