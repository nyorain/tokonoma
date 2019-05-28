#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"
#include "scene.frag.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;
layout(location = 4) in float inLinDepth;

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

	vec2 uv = (material.normalCoords == 0u) ? inTexCoord0 : inTexCoord1;
	return tbnNormal(n, inPos, uv, normalTex);
}

void main() {
	// discarded fragments only output no emission
	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}

	vec2 auv = (material.albedoCoords == 0u) ? inTexCoord0 : inTexCoord1;
	vec4 albedo = material.albedo * texture(albedoTex, auv);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	vec3 normal = getNormal();
	if(!gl_FrontFacing) { // flip normal, see gltf spec
		normal *= -1;
	}

	outNormal.xy = encodeNormal(normal);
	outAlbedo.rgb = albedo.rgb;

	vec2 euv = (material.emissionCoords == 0u) ? inTexCoord0 : inTexCoord1;
	outEmission.xyz = material.emission * texture(emissionTex, euv).rgb;
	outEmission.w = model.id;

	vec2 ouv = (material.occlusionCoords == 0u) ? inTexCoord0 : inTexCoord1;
	outAlbedo.w = texture(occlusionTex, ouv).r;

	vec2 mruv = (material.metalRoughCoords == 0u) ? inTexCoord0 : inTexCoord1;
	vec4 mr = texture(metalRoughTex, mruv);

	// b,g components as specified by gltf spec
	// NOTE: using only half the range here (normal buf is 16f)
	outNormal.z = material.metallic * mr.b;
	outNormal.w = material.roughness * mr.g;

	outDepth = depthtoz(gl_FragCoord.z, scene.near, scene.far);
	// outDepth = inLinDepth;
}
