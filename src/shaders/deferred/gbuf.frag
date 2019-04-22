#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"
#include "scene.frag.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec4 outPos; // xyz: pos, w: roughness
layout(location = 1) out vec4 outNormal; // xyz: normal, w: metallic
layout(location = 2) out vec4 outAlbedo; // rgb: albedo, w: occlusion

// material
layout(set = 1, binding = 0) uniform sampler2D albedoTex;
layout(set = 1, binding = 1) uniform sampler2D metalRoughTex;
layout(set = 1, binding = 2) uniform sampler2D normalTex;
layout(set = 1, binding = 3) uniform sampler2D occlusionTex;

layout(push_constant) uniform MaterialPcrBuf {
	MaterialPcr material;
};


// NOTE: tangent and bitangent could also be passed in for each vertex
vec3 getNormal() {
	vec3 n = normalize(inNormal);
	if((material.flags & normalMap) == 0u) {
		return n;
	}

	return tbnNormal(n, inPos, inUV, normalTex);
}

void main() {
	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}

	vec4 albedo = material.albedo * texture(albedoTex, inUV);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	vec3 normal = getNormal();
	if(!gl_FrontFacing) { // flip normal, see gltf spec
		normal *= -1;
	}

	outNormal.xyz = normal;
	outPos.xyz = inPos;
	outAlbedo.rgb = albedo.rgb;

	// as specified by gltf spec
	vec2 mr = texture(metalRoughTex, inUV).gb;
	outPos.w = material.roughness * mr.x;
	outNormal.w = material.metallic * mr.y;
	outAlbedo.w = texture(occlusionTex, inUV).r;
}
