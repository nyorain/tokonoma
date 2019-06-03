#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"
#include "scene.frag.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;
layout(location = 4) in float inLinDepth;

layout(location = 0) out vec4 outReflectance;
layout(location = 1) out float outRevealage;

// material
layout(set = 1, binding = 0) uniform sampler2D albedoTex;
layout(set = 1, binding = 1) uniform sampler2D metalRoughTex;
layout(set = 1, binding = 2) uniform sampler2D normalTex;
layout(set = 1, binding = 3) uniform sampler2D occlusionTex;
layout(set = 1, binding = 4) uniform sampler2D emissionTex;

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

	vec4 color = albedo;
	float z = gl_FragCoord.z;
	float weight = 
		max(min(1.0, max(max(color.r, color.g), color.b) * color.a), color.a) *
		clamp(0.03 / (1e-5 + pow(z / 200, 4.0)), 1e-2, 3e3);
 
	outReflectance = vec4(color.rgb * color.a, color.a) * weight;
	outRevealage = color.a;
}

