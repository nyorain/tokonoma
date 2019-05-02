#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"
#include "scene.frag.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
// layout(location = 3) in float inLinDepth;

layout(location = 0) out vec4 outNormal; // xy: encoded normal, z: matID, w: roughness
layout(location = 1) out vec4 outAlbedo; // rgb: albedo, w: occlusion
layout(location = 2) out vec4 outEmission; // rgb: emission, w: metallic
layout(location = 3) out float outDepth;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj; // view and pojection
	mat4 _invProj;
	vec3 _viewPos;
	float near, far;
} scene;

// material
layout(set = 1, binding = 0) uniform sampler2D albedoTex;
layout(set = 1, binding = 1) uniform sampler2D metalRoughTex;
layout(set = 1, binding = 2) uniform sampler2D normalTex;
layout(set = 1, binding = 3) uniform sampler2D occlusionTex;
layout(set = 1, binding = 4) uniform sampler2D emissionTex;

layout(set = 2, binding = 0, row_major) uniform Model {
	mat4 _matrix;
	mat4 _normal;
	uint id;
} model;

layout(push_constant) uniform MaterialPcrBuf {
	MaterialPcr material;
};

vec3 getNormal() {
	vec3 n = normalize(inNormal);
	if((material.flags & normalMap) == 0u) {
		return n;
	}

	return tbnNormal(n, inPos, inUV, normalTex);
}

void main() {
	// discarded fragments only output no emission
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
	outAlbedo.rgb = albedo.rgb;
	outEmission.xyz = material.emission * texture(emissionTex, inUV).rgb;

	outNormal.z = 2.f * (model.id / 65536.0) - 1.f; // snorm format
	outAlbedo.w = texture(occlusionTex, inUV).r;

	vec4 mr = texture(metalRoughTex, inUV);
	// components as specified by gltf spec
	// NOTE: use only half the range (normal and emissions gbufs are 16f)
	outNormal.w = material.roughness * mr.g;
	outEmission.w = material.metallic * mr.b;

	outDepth = depthtoz(gl_FragCoord.z, scene.near, scene.far);
}
