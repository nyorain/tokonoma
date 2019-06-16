#version 450

#extension GL_GOOGLE_include_directive : enable
#include "pbr.glsl"
#include "scene.glsl"
#include "scene.frag.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;
layout(location = 4) in flat uint inMatID;
layout(location = 5) in flat uint inModelID;

layout(location = 0) out vec4 outCol;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj;
	vec3 viewPos; // camera position. For specular light
	float near, far;
} scene;

// material
layout(set = 1, binding = 2, std430) buffer Materials {
	Material materials[];
};

layout(set = 1, binding = 3) uniform texture2D textures[imageCount];
layout(set = 1, binding = 4) uniform sampler samplers[samplerCount];

layout(set = 2, binding = 0, row_major) uniform LightBuf {
	DirLight dirLight;
};

layout(set = 2, binding = 1) uniform sampler2DArrayShadow shadowMap;
// layout(set = 2, binding = 1) uniform samplerCubeShadow shadowCube;

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
		normal = tbnNormal(normal, inPos, tuv, n.xyz);
	}

	if(!gl_FrontFacing) { // flip normal, see gltf spec
		normal *= -1;
	}

	vec4 mr = readTex(material.metalRough);
	float metalness = material.metallicFac * mr.b;
	float roughness = material.roughnessFac * mr.g;

	float ambientFac = 0.1 * readTex(material.occlusion).r;

	float shadow;
	vec3 ldir;

	// TODO: some features missing, compare deferred renderer
	// point light: attenuation no implemented here atm
	/*
	bool pcf = (dirLight.flags & lightPcf) != 0;
	if((dirLight.flags & lightDir) != 0) {
		shadow = dirShadow(shadowMap, inLightPos, int(pcf));
		ldir = normalize(dirLight.dir);
	} else {
		ldir = normalize(inPos - pointLight.pos);
		if(pcf) {
			float radius = 0.005;
			shadow = pointShadowSmooth(shadowCube, pointLight.pos,
				pointLight.radius, inPos, radius);
		} else {
			shadow = pointShadow(shadowCube, pointLight.pos,
				pointLight.radius, inPos);
		}
	}
	*/

	// NOTE: dir light hardcoded atm
	float between;
	float linearz = depthtoz(gl_FragCoord.z, scene.near, scene.far);
	uint index = getCascadeIndex(dirLight, linearz, between);

	// color the different levels for debugging
	// switch(index) {
	// 	case 0: lcolor = mix(lcolor, vec3(1, 1, 0), 0.8); break;
	// 	case 1: lcolor = mix(lcolor, vec3(0, 1, 1), 0.8); break;
	// 	case 2: lcolor = mix(lcolor, vec3(1, 0, 1), 0.8); break;
	// 	case 3: lcolor = mix(lcolor, vec3(1, 0, 1), 0.8); break;
	// 	default: lcolor = vec3(1, 1, 1); break;
	// };

	bool pcf = (dirLight.flags & lightPcf) != 0;
	shadow = dirShadowIndex(dirLight, shadowMap, inPos, index, int(pcf));
	ldir = normalize(dirLight.dir);


	vec3 v = normalize(scene.viewPos - inPos);
	vec3 light = cookTorrance(normal, -ldir, v, roughness, metalness,
		albedo.xyz);
	light *= shadow;
	outCol.rgb = dirLight.color * light;
	outCol.rgb += ambientFac * albedo.rgb;

	outCol.a = albedo.a;
	if(material.alphaCutoff == -1.f) { // alphaMode opque
		outCol.a = 1.f;
	}
}
