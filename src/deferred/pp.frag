#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj;
	mat4 _invProj;
	vec3 _viewPos;
	float nearPlane;
	float farPlane;
} scene;

layout(set = 1, binding = 0, input_attachment_index = 0)
	uniform subpassInput inLight;
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inAlbedo;
layout(set = 1, binding = 2) uniform UBO {
	vec3 scatterLightColor;
	uint tonemap;
	float aoFactor;
	float ssaoPow;
	float exposure;
} ubo;
layout(set = 1, binding = 3) uniform sampler2D ssaoTex;
layout(set = 1, binding = 4) uniform sampler2D depthTex;
layout(set = 1, binding = 5) uniform sampler2D scatterTex;

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
	vec4 color = subpassLoad(inLight);

	// scattering
	{
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
		color.rgb += scatter * ubo.scatterLightColor;
	}

	{
		const float near = scene.nearPlane;
		const float far = scene.farPlane;

		float ao = 0.f;
		int range = 2;
		vec2 texelSize = 1.f / textureSize(ssaoTex, 0);
		float depth = textureLod(depthTex, uv, 0).r;
		float z = depthtoz(depth, near, far);
		float total = 0.f;

		// TODO: what kind of filter to use?
		// without gaussian it's a simple box filter
		// gaussian vs box
		// const float kernel[5][5] = {
		// 	{0.003765,	0.015019,	0.023792,	0.015019,	0.003765},
		// 	{0.015019,	0.059912,	0.094907,	0.059912,	0.015019},
		// 	{0.023792,	0.094907,	0.150342,	0.094907,	0.023792},
		// 	{0.015019,	0.059912,	0.094907,	0.059912,	0.015019},
		// 	{0.003765,	0.015019,	0.023792,	0.015019,	0.003765},
		// };

		for(int x = -range; x <= range; ++x) {
			for(int y = -range; y <= range; ++y) {
				vec2 off = texelSize * vec2(x, y); // spaced
				float sampleDepth = textureLod(depthTex, uv + off, 0.0).r;
				float samplez = depthtoz(sampleDepth, near, far);
				float sampleAo = textureLod(ssaoTex, uv + off, 0.0).r;

				// make sure to not blur over edges here
				float fac = smoothstep(0.0, 1.0, 1 - 20 * abs(z - samplez));
				// fac *= kernel[x + range][y + range];
				ao += fac * sampleAo;
				total += fac;
			}
		}

		// w component of albedo contains texture-based ao factor
		ao /= total;
		vec4 albedo = subpassLoad(inAlbedo);
		color.rgb += pow(ao, ubo.ssaoPow) * ubo.aoFactor * albedo.rgb * albedo.w;

		// no blur variant
		// float ao = textureLod(ssaoTex, uv, 0.0).r;
		// vec4 albedo = subpassLoad(inAlbedo);
		// color.rgb += pow(ao, ubo.ssaoPow) * ubo.aoFactor * albedo.rgb * albedo.w;
	}

	fragColor = vec4(tonemap(color.rgb), 1.0);
}
