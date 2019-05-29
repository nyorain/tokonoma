#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"
#include "pbr.glsl"

// const uint flagDiffuseIBL = (1u << 0u);
// const uint flagSpecularIBL = (1u << 1u);
const uint flagEmission = (1u << 2u);

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragAmbient;

layout(set = 0, binding = 0, row_major) uniform SceneUbo {
	mat4 proj;
	mat4 invProj;
	vec3 viewPos;
	float near, far;
} scene;

layout(set = 1, binding = 0, input_attachment_index = 0) 
	uniform subpassInput inNormal;
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inAlbedo;
layout(set = 1, binding = 2, input_attachment_index = 2)
	uniform subpassInput inLinDepth;
layout(set = 1, binding = 3, input_attachment_index = 3)
	uniform subpassInput inEmission;

layout(set = 1, binding = 4) uniform samplerCube irradianceMap;
layout(set = 1, binding = 5) uniform samplerCube envMap;
layout(set = 1, binding = 6) uniform sampler2D brdfLut;

layout(set = 1, binding = 7) uniform Params {
	uint flags;
	float factor;
	float ssaoPow;
} params;

layout(push_constant) uniform PCR {
	uint filteredEnvLods;
} pcr;

void main() {
	// apply emission
	vec4 vEmission = subpassLoad(inEmission);
	fragAmbient = vec4(0.0);
	if((params.flags & flagEmission) != 0) {
		fragAmbient.rgb += vEmission.rgb;
	}

	float ld = subpassLoad(inLinDepth).r;
	float depth = ztodepth(ld, scene.near, scene.far);
	if(depth >= 1.f || params.factor <= 0.f) {
		return;
	}

	vec4 vAlbedo = subpassLoad(inAlbedo);
	vec3 albedo = vAlbedo.rgb;

	vec4 vNormal = subpassLoad(inNormal);
	vec3 normal = decodeNormal(vNormal.xy);
	float roughness = vNormal.w;
	float metallic = vNormal.z;

	// reconstruct position
	vec3 fragPos = reconstructWorldPos(uv, scene.invProj, depth);
	vec3 viewDir = normalize(fragPos - scene.viewPos);
	vec3 ambient = ao(params.flags, viewDir, normal, albedo, metallic,
		roughness, irradianceMap, envMap, brdfLut, pcr.filteredEnvLods);

	float ao = params.factor * vAlbedo.w;
	fragAmbient.rgb += ao * ambient;
}
