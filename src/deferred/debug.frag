#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

const uint modeAlbedo = 1u;
const uint modeNormals = 2u;
const uint modeRoughness = 3u;
const uint modeMetalness = 4u;
const uint modeAO = 5u;
const uint modeAlbedoAO = 6u;
const uint modeSSR = 7u;
const uint modeDepth = 8u;
const uint modeEmission = 9u;
const uint modeBloom = 10u;
const uint modeLuminance = 11u;

layout(set = 0, binding = 0) uniform sampler2D inAlbedo;
layout(set = 0, binding = 1) uniform sampler2D inNormal;
layout(set = 0, binding = 2) uniform sampler2D inDepth;
layout(set = 0, binding = 3) uniform sampler2D inSSAO;
layout(set = 0, binding = 4) uniform sampler2D inSSR;
layout(set = 0, binding = 5) uniform sampler2D inEmission;
layout(set = 0, binding = 6) uniform sampler2D inBloom;
layout(set = 0, binding = 7) uniform sampler2D inLuminance;

layout(set = 0, binding = 8) uniform UBO {
	uint mode;
} params;

void main() {
	vec4 albedo = texture(inAlbedo, uv);
	vec4 normal = texture(inNormal, uv);
	vec4 ssao = texture(inSSAO, uv);
	vec4 ssr = texture(inSSR, uv);
	vec4 depth = texture(inDepth, uv);
	vec4 emission = texture(inEmission, uv);

	// NOTE: from other passes, could be passed in as parameters here
	float exposure = 1.0;
	float aoFactor = 1.0;
	float ssaoPow = 3.0;
	float bloomStrength = 3.0;

	switch(params.mode) {
		case modeAlbedo:
			fragColor = vec4(albedo.rgb, 1.0);
			break;
		case modeNormals:
			fragColor = vec4(0.5 + 0.5 * decodeNormal(normal.xy), 1.0);
			break;
		case modeRoughness:
			fragColor = vec4(vec3(normal.w), 1.0);
			break;
		case modeMetalness:
			fragColor = vec4(vec3(emission.w), 1.0);
			break;
		case modeAO:
		case modeAlbedoAO: {
			float ao = aoFactor * albedo.w;
			ao *= pow(ssao.r, ssaoPow);
			vec3 col = (params.mode == modeAO) ? vec3(1.0) : albedo.rgb;
			fragColor = vec4(ao * col, 1.0);
			break;
		} case modeSSR:
			fragColor = vec4(ssr.w * (ssr.xy / textureSize(inBloom, 0)), ssr.z, 1.0);
			// fragColor = vec4(ssr.xy / textureSize(bloomTex, 0), ssr.w, 1.0);
			break;
		case modeDepth:
			fragColor = vec4(vec3(1.0 - exp(-0.1 * exposure * depth.r)), 1.0);
			break;
		case modeEmission:
			fragColor = vec4(emission.rgb, 1.0);
			break;
		case modeBloom: {
			uint bloomLevels = textureQueryLevels(inBloom);
			vec3 bloomSum = vec3(0.0);
			for(uint i = 0u; i < bloomLevels; ++i) {
				float fac = bloomStrength;
				// fac /= (1 + i); // decrease?
				bloomSum += fac * textureLod(inBloom, uv, i).rgb;
			}
			// tonemapping needed here
			bloomSum = 1.0 - exp(-exposure * bloomSum);
			fragColor = vec4(bloomSum, 1.0);
			break;
		} case modeLuminance: {
			float lum = exp2(texture(inLuminance, uv).r);
			fragColor = vec4(vec3(lum), 1.0);
			break;
		} default:
			fragColor = vec4(0, 0, 0, 1);
			break;
	}
}

