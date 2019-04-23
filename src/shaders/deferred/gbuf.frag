#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"
#include "scene.frag.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec4 outNormal; // xy: encoded normal, z: matID, w: occlusion
layout(location = 1) out vec4 outAlbedo; // rgb: albedo, w: roughness

// NOTE: emission not supported yet
layout(location = 2) out vec4 outEmission; // rgb: emission, w: metallic

// material
layout(set = 1, binding = 0) uniform sampler2D albedoTex;
layout(set = 1, binding = 1) uniform sampler2D metalRoughTex;
layout(set = 1, binding = 2) uniform sampler2D normalTex;
layout(set = 1, binding = 3) uniform sampler2D occlusionTex;

layout(set = 2, binding = 0, row_major) uniform Model {
	mat4 _matrix;
	mat4 _normal;
	// uint id; // TODO
} model;

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

	outNormal.xy = encodeNormal(normal);
	// outNormal.xyz = normal;
	outAlbedo.rgb = albedo.rgb;
	outEmission.xyz = vec3(0.0); // TODO!

	// outNormal.y = 2.f * (1.f / model.id) - 1.f; // snorm format
	// NOTE: occlusion uses only half the range (normal gbuf is snorm)
	// but since it only uses 8 bits and we have a 16 bit format
	// that should be ok. we waste some space here though!
	outNormal.w = texture(occlusionTex, inUV).r;

	// as specified by gltf spec
	vec4 mr = texture(metalRoughTex, inUV);
	outAlbedo.w = material.roughness * mr.g;
	outEmission.w = material.metallic * mr.b;
}
