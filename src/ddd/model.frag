#version 450

#extension GL_GOOGLE_include_directive : enable
#include "pbr.glsl"
#include "scene.glsl"
#include "spharm.glsl"
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

layout(set = 3, binding = 0) uniform samplerCube envMap;
layout(set = 3, binding = 1) uniform sampler2D brdfLut;
// 9 layers, each layer contains one spherical harmonics paremter
// for all probes. rgbaf format, the alpha component of the
// first 3 layers contains the position of the probe
layout(set = 3, binding = 2) uniform sampler1DArray irradiance;

layout(set = 3, binding = 3) uniform Params {
	uint mode;
	uint probeCount;
	float aoFac;
	float maxProbeDist;
} params;

const uint modeLight = (1u << 0);
const uint modeSpecularIBL = (1u << 1);
const uint modeIrradiance = (1u << 2);
const uint modeStaticAO = (1u << 3);
const uint modelAOAlbedo = (1u << 4);

vec4 readTex(MaterialTex tex) {
	vec2 tuv = (tex.coords == 0u) ? inTexCoord0 : inTexCoord1;
	return texture(sampler2D(textures[tex.id], samplers[tex.samplerID]), tuv);	
}

// TODO: can be optimized by first summing up all coefficients (weighted)
// and then computing polynoms for normal. For all the light probes
// we sample from
// TODO: use better attenuation function here
vec3 lightProbe(vec3 pos, vec3 nrm, float coord, inout float total) {
	vec4 coeffs[3];
	coeffs[0] = texture(irradiance, vec2(coord, 0));
	coeffs[1] = texture(irradiance, vec2(coord, 1));
	coeffs[2] = texture(irradiance, vec2(coord, 2));

	vec3 probePos = vec3(coeffs[0].w, coeffs[1].w, coeffs[2].w);
	float dist = length(pos - probePos);
	/*
	float fac = 1.0 - dist / params.maxProbeDist;
	if(fac < 0) {
		return vec3(0.0);
	}

	fac = pow(fac, 2.0);
	*/

	vec3 res = vec3(0.0);
	// float fac = 1 / (1 + dist + 10 * params.maxProbeDist * (dist * dist));
	float fac = exp(-params.maxProbeDist * dist);
	if(fac < 0.001) {
		return res;
	}

	res += sh0(nrm) * coeffs[0].xyz;
	res += sh1(nrm) * coeffs[1].xyz;
	res += sh2(nrm) * coeffs[2].xyz;

	res += sh3(nrm) * texture(irradiance, vec2(coord, 3)).rgb;
	res += sh4(nrm) * texture(irradiance, vec2(coord, 4)).rgb;
	res += sh5(nrm) * texture(irradiance, vec2(coord, 5)).rgb;
	res += sh6(nrm) * texture(irradiance, vec2(coord, 6)).rgb;
	res += sh7(nrm) * texture(irradiance, vec2(coord, 7)).rgb;
	res += sh8(nrm) * texture(irradiance, vec2(coord, 8)).rgb;

	total += fac;
	return oneOverPi * fac * res;
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

	vec3 color = vec3(0.0);
	if(bool(params.mode & modeLight)) {
		vec4 mr = readTex(material.metalRough);
		float metalness = material.metallicFac * mr.b;
		float roughness = material.roughnessFac * mr.g;

		float shadow;
		vec3 ldir;

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
		color.rgb += dirLight.color * clamp(light, 0, 1);
	}

	if(bool(params.mode & modeSpecularIBL)) {
		// TODO
	}

	float ambientFac = params.aoFac * readTex(material.occlusion).r;
	if(bool(params.mode & modeIrradiance)) {
		vec3 acolor = vec3(0.0);
		float total = 0.0;
		for(uint i = 0u; i < params.probeCount; ++i) {
			float texCoord = float(i + 0.5f) / textureSize(irradiance, 0).x;
			acolor += lightProbe(inPos, normal, texCoord, total);
		}

		if(total > 0.001) {
			acolor /= total;
		}

		vec3 a = ambientFac * acolor;
		if(bool(params.mode & modelAOAlbedo)) {
			a *= albedo.rgb;
		}
		color.rgb += a;
	}

	if(bool(params.mode & modeStaticAO)) {
		color.rgb += ambientFac * albedo.rgb;
	}

	outCol = vec4(color, albedo.a);
	if(material.alphaCutoff == -1.f) { // alphaMode opque
		outCol.a = 1.f;
	}

	// TODO: tonemap?
}
