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
layout(set = 4, binding = 2) uniform samplerCube irradianceMap;

layout(set = 4, binding = 3) uniform Params {
	uint mode;
	float aoFac;
	uint envLods;
} params;

const uint modeDirLight = (1u << 0);
const uint modePointLight = (1u << 1);
const uint modeSpecularIBL = (1u << 2);
const uint modeIrradiance = (1u << 3);

vec4 readTex(MaterialTex tex) {
	vec2 tuv = (tex.coords == 0u) ? inTexCoord0 : inTexCoord1;
	return texture(sampler2D(textures[tex.id], samplers[tex.samplerID]), tuv);	
}

float dither17(vec2 pos, vec2 FrameIndexMod4) {
	return fract(dot(vec3(pos.xy, FrameIndexMod4), uvec3(2, 7, 23) / 17.0f));
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
	float cosTheta = clamp(dot(normal, -viewDir), 0.0, 1);
	vec3 kS = fresnelSchlickRoughness(cosTheta, f0, roughness);
	// vec3 kS = vec3(0.5);
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
		vec3 irradiance = texture(irradianceMap, normal).rgb;
		vec3 diffuse = kD * irradiance * albedo.rgb;
		color.rgb += ambientFac * diffuse;
	}

	outCol = vec4(color, albedo.a);
	if(material.alphaCutoff == -1.f) { // alphaMode opque
		outCol.a = 1.f;
	}

	/*
	// TODO WIP stochastic transparency
	// only really useful for TAA
	if(material.alphaCutoff == 0.f) { // alphaMode blend 
		// TODO: use blue noise/dither pattern
		// if(random(10 * inPos - 5 * outCol.rgb) > outCol.a) {
		// 	discard;
		// }
		vec2 xy = gl_FragCoord.xy + 4 * random2(1529 * inPos.xy);
		if(dither17(xy, mod(xy, vec2(4))) - albedo.a < 0) {
			discard;
		}

		outCol.a = 1.f;
	}
	*/

	// tonemap
	// outCol.rgb = 1.0 - exp(-outCol.rgb);
}
