#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"
#include "scene.frag.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;
layout(location = 4) in float inLinDepth;

layout(location = 5) in flat uint inMatID;
layout(location = 6) in flat uint inModelID;

layout(location = 0) out vec4 outNormal; // xy: encoded normal, z: metal, w: roughness
layout(location = 1) out vec4 outAlbedo; // rgb: albedo, w: occlusion
layout(location = 2) out vec4 outEmission; // rgb: emission, w: matID
layout(location = 3) out float outDepth;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj; // view and pojection
	mat4 _invProj;
	vec3 _viewPos;
	float near, far;
} scene;

layout(set = 1, binding = 2, std430) buffer Materials {
	Material materials[];
};

layout(set = 1, binding = 3) uniform texture2D textures[imageCount];
layout(set = 1, binding = 4) uniform sampler samplers[samplerCount];

vec4 readTex(MaterialTex tex) {
	vec2 tuv = (tex.coords == 0u) ? inTexCoord0 : inTexCoord1;
	return texture(sampler2D(textures[tex.id], samplers[tex.samplerID]), tuv);	
}

void main() {
	Material material = materials[inMatID];

	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}

	vec4 albedo = material.albedoFac * readTex(material.albedo);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	vec3 normal = normalize(inNormal);
	if((material.flags & normalMap) != 0u) {
		MaterialTex nt = material.normals;
		vec2 tuv = (nt.coords == 0u) ? inTexCoord0 : inTexCoord1;
		vec4 n = texture(sampler2D(textures[nt.id], samplers[nt.samplerID]), tuv);
		normal = tbnNormal(normal, inPos, tuv, 2 * n.xyz - 1);
	}

	if(!gl_FrontFacing) { // flip normal, see gltf spec
		normal *= -1;
	}

	outNormal.xy = encodeNormal(normal);
	outAlbedo.rgb = albedo.rgb;

	outEmission.xyz = material.emissionFac * readTex(material.emission).rgb;
	outEmission.w = inModelID;
	outAlbedo.w = readTex(material.occlusion).r;

	vec4 mr = readTex(material.metalRough);

	// b,g components as specified by gltf spec
	// NOTE: using only half the range here (normal buf is 16f)
	outNormal.z = material.metallicFac * mr.b;
	outNormal.w = material.roughnessFac * mr.g;

	outDepth = depthtoz(gl_FragCoord.z, scene.near, scene.far);
	// outDepth = inLinDepth;
}
