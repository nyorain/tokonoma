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

layout(set = 0, binding = 0) uniform UBO {
	uint mode;
	uint flags;
	float aoFactor;
	float ssaoPow;
	uint _tonemap;
	float exposure;
} params;

layout(set = 0, binding = 1, input_attachment_index = 1) uniform subpassInput inAlbedo;
layout(set = 0, binding = 2, input_attachment_index = 2) uniform subpassInput inNormal;
layout(set = 0, binding = 3, input_attachment_index = 3) uniform subpassInput inDepth;
layout(set = 0, binding = 4, input_attachment_index = 4) uniform subpassInput inSSAO;
layout(set = 0, binding = 5, input_attachment_index = 5) uniform subpassInput inSSR;
layout(set = 0, binding = 6) uniform sampler2D bloomTex;

void main() {
	vec4 albedo = subpassLoad(inAlbedo);
	vec4 normal = subpassLoad(inNormal);
	vec4 ssao = subpassLoad(inSSAO);
	vec4 ssr = subpassLoad(inSSR);
	vec4 depth = subpassLoad(inDepth);
	vec4 emission = textureLod(bloomTex, uv, 0);

	switch(params.mode) {
		case modeAlbedo:
			fragColor = vec4(subpassLoad(inAlbedo).rgb, 1.0);
			break;
		case modeNormals:
			fragColor = vec4(0.5 + 0.5 * decodeNormal(normal.xy), 1.0);
			break;
		case modeRoughness:
			fragColor = vec4(vec3(subpassLoad(inNormal).w), 1.0);
			break;
		case modeMetalness:
			fragColor = vec4(vec3(emission.w), 1.0);
			break;
		case modeAO:
		case modeAlbedoAO: {
			float ao = params.aoFactor * albedo.w;
			ao *= pow(ssao.r, params.ssaoPow);
			vec3 col = (params.mode == modeAO) ? vec3(1.0) : albedo.rgb;
			fragColor = vec4(ao * col, 1.0);
			break;
		} case modeSSR:
			fragColor = vec4(ssr.w * (ssr.xy / textureSize(bloomTex, 0)), ssr.z, 1.0);
			// fragColor = vec4(ssr.xy / textureSize(bloomTex, 0), ssr.w, 1.0);
			break;
		case modeDepth:
			fragColor = vec4(vec3(1.0 - exp(-0.1 * params.exposure * depth.r)), 1.0);
			break;
		case modeEmission:
			fragColor = vec4(emission.rgb, 1.0);
			break;
		case modeBloom: {
			uint bloomLevels = textureQueryLevels(bloomTex);
			vec3 bloomSum = vec3(0.0);
			for(uint i = 0u; i < bloomLevels; ++i) {
				float fac = 1.f / (1 + i);
				bloomSum += fac * textureLod(bloomTex, uv, i).rgb;
			}
			bloomSum = 1.0 - exp(-params.exposure * bloomSum);
			fragColor = vec4(bloomSum, 1.0);
			break;
		} default:
			fragColor = vec4(0, 0, 0, 1);
			break;
	}
}

