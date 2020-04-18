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

// this simple forward renderer only supports one point and one
// dir light (optionally)
// directional light
layout(set = 2, binding = 0, row_major) uniform DirLightBuf {
	DirLight dirLight;
};

layout(set = 2, binding = 1) uniform sampler2DArrayShadow shadowMap;

// point light
layout(set = 3, binding = 0, row_major) uniform PointLightBuf {
	PointLight pointLight;
};
layout(set = 3, binding = 1) uniform samplerCubeShadow shadowCube;

layout(set = 4, binding = 0) uniform samplerCube envMap;
layout(set = 4, binding = 1) uniform sampler2D brdfLut;
// 9 layers, each layer contains one spherical harmonics paremter
// for all probes. rgbaf format, the alpha component of the
// first 3 layers contains the position of the probe
layout(set = 4, binding = 2) uniform sampler1DArray irradiance;

layout(set = 4, binding = 3) uniform Params {
	uint mode;
	uint probeCount;
	float aoFac;
	float maxProbeDist;
	uint envLods;
} params;

const uint modeDirLight = (1u << 0);
const uint modePointLight = (1u << 1);
const uint modeSpecularIBL = (1u << 2);
const uint modeStaticAO = (1u << 3);
const uint modeIrradiance = (1u << 4);
const uint modelAOAlbedo = (1u << 5);

vec4 readTex(MaterialTex tex) {
	vec2 tuv = (tex.coords == 0u) ? inTexCoord0 : inTexCoord1;
	return texture(sampler2D(textures[tex.id], samplers[tex.samplerID]), tuv);	
}

// TODO(perf): can be optimized by first summing up all coefficients (weighted)
// and then computing polynoms for normal. For all the light probes
// we sample from
vec3 lightProbe(vec3 pos, vec3 nrm, float coord, inout float total, inout float fmax) {
	vec4 coeffs[3];
	coeffs[0] = texture(irradiance, vec2(coord, 0));
	coeffs[1] = texture(irradiance, vec2(coord, 1));
	coeffs[2] = texture(irradiance, vec2(coord, 2));

	// TODO: use better attenuation function here
	vec3 probePos = vec3(coeffs[0].w, coeffs[1].w, coeffs[2].w);
	float dist = length(pos - probePos);
	// float fac = 1.0 - dist / params.maxProbeDist;
	// if(fac < 0) {
	// 	return vec3(0.0);
	// }
	// fac = pow(fac, 2.0);

	vec3 res = vec3(0.0);
	// float fac = 1 / (1 + dist + 10 * params.maxProbeDist * (dist * dist));
	// float fac = 1 / (1 + params.maxProbeDist * (dist * dist));
	float fac = exp(-params.maxProbeDist * dist);
	// if(fac < 0.01 && fac < fmax / 2) {
	// 	return res;
	// }

	fmax = max(fmax, fac);
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
		normal = tbnNormal(normal, inPos, tuv, 2 * n.xyz - 1);
	}

	if(!gl_FrontFacing) { // flip normal, see gltf spec
		normal *= -1;
	}

	vec3 color = vec3(0.0);

	vec3 viewDir = normalize(inPos - scene.viewPos);
	vec4 mr = readTex(material.metalRough);
	float metalness = material.metallicFac * mr.b;
	float roughness = material.roughnessFac * mr.g;
	if(bool(params.mode & modeDirLight)) {
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

		bool pcf = bool(dirLight.flags & lightPcf);
		float shadow = dirShadowIndex(dirLight, shadowMap, inPos, index, int(pcf));
		vec3 ldir = normalize(dirLight.dir);

		vec3 light = cookTorrance(normal, -ldir, -viewDir, roughness,
			metalness, albedo.xyz);
		light *= shadow;
		color.rgb += dirLight.color * light;
	}

	if(bool(params.mode & modePointLight)) {
		vec3 ldir = inPos - pointLight.pos;
		float lightDist = length(ldir);
		if(lightDist < pointLight.radius) {
			ldir /= lightDist;	
			// TODO(perf): we could probably re-use some of the values
			// from the directional light shading, just do them once
			vec3 light = cookTorrance(normal, -ldir, -viewDir, roughness,
				metalness, albedo.xyz);
			light *= defaultAttenuation(lightDist, pointLight.radius);
			// light *= attenuation(lightDist, pointLight.attenuation);
			if(bool(pointLight.flags & lightPcf)) {
				// TODO: make radius parameters configurable,
				float viewDistance = length(scene.viewPos - inPos);
				float radius = (1.0 + (viewDistance / 30.0)) / 100.0;  
				light *= pointShadowSmooth(shadowCube, pointLight.pos,
					pointLight.radius, inPos, radius);
			} else {
				light *= pointShadow(shadowCube, pointLight.pos,
					pointLight.radius, inPos);
			}

			color.rgb += pointLight.color * light;
		}
	}

	vec3 f0 = vec3(0.04);
	f0 = mix(f0, albedo.rgb, metalness);
	float ambientFac = params.aoFac * readTex(material.occlusion).r;

	// NOTE: when specular IBL isn't enabled we could put all the
	// energy into diffuse IBL or the other way around. Would look
	// somewhat weird though probably
	float cosTheta = clamp(dot(normal, -viewDir), 0, 1);
	vec3 kS = fresnelSchlickRoughness(cosTheta, f0, roughness);
	vec3 kD = 1.0 - kS;
	kD *= 1.0 - metalness;
	if(bool(params.mode & modeSpecularIBL)) {
		vec3 R = reflect(viewDir, normal);
		float lod = roughness * params.envLods;
		vec3 filtered = textureLod(envMap, R, lod).rgb;
		vec2 brdfParams = vec2(cosTheta, roughness);
		vec2 brdf = texture(brdfLut, brdfParams).rg;
		vec3 specular = filtered * (kS * brdf.x + brdf.y);
		color.rgb += ambientFac * specular;
	}

	if(bool(params.mode & modeIrradiance)) {
		vec3 acolor = vec3(0.0);
		float total = 0.0;
		float fmax = 0.0;
		for(uint i = 0u; i < params.probeCount; ++i) {
			float texCoord = float(i + 0.5f) / textureSize(irradiance, 0).x;
			acolor += lightProbe(inPos, normal, texCoord, total, fmax);
		}

		if(total > 0.0) {
			acolor /= total;
		}

		vec3 diffuse = kD * acolor;
		if(bool(params.mode & modelAOAlbedo)) {
			diffuse *= albedo.rgb;
		}

		color.rgb += ambientFac * diffuse;
	}

	if(bool(params.mode & modeStaticAO)) {
		color.rgb += ambientFac * albedo.rgb;
	}

	outCol = vec4(color, albedo.a);
	if(material.alphaCutoff == -1.f) { // alphaMode opque
		outCol.a = 1.f;
	}

	// tonemap
	// outCol.rgb = 1.0 - exp(-outCol.rgb);
}
