#version 450

#extension GL_GOOGLE_include_directive : enable
#include "pbr.glsl"
#include "scene.glsl"
#include "scene.frag.glsl"
#include "spharm.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;
layout(location = 4) in flat uint inMatID;
// layout(location = 5) in flat uint inModelID;

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

// environment/AO
layout(set = 4, binding = 0) uniform samplerCube envMap;
layout(set = 4, binding = 1) uniform sampler2D brdfLut;
layout(set = 4, binding = 2) uniform samplerCube irradianceMap;

layout(set = 4, binding = 3) uniform Params {
// layout(set = 4, binding = 2) uniform Params {
	// vec3 radianceCoeffs[9]; // stored as vec4 on cpu due to padding
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
		normal = tbnNormal(normal, inPos, tuv, 2.0 * n.xyz - 1.0);
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
		vec3 lcolor = dirLight.color;

		// color the different levels for debugging
#if 0
		switch(index) {
			case 0: lcolor = mix(lcolor, vec3(1, 1, 0), 0.8); break;
			case 1: lcolor = mix(lcolor, vec3(0, 1, 1), 0.8); break;
			case 2: lcolor = mix(lcolor, vec3(1, 0, 1), 0.8); break;
			case 3: lcolor = mix(lcolor, vec3(1, 0, 1), 0.8); break;

			case 4: lcolor = mix(lcolor, vec3(1, 0, 0), 0.8); break;
			default: lcolor = vec3(0, 0, 1); break;
		};
#endif

		bool pcf = bool(dirLight.flags & lightPcf);
		float shadow = dirShadowIndex(dirLight, shadowMap, inPos, index, int(pcf));

		vec3 ldir = normalize(dirLight.dir);
		vec3 light = cookTorrance(normal, -ldir, -viewDir, roughness,
			metalness, albedo.xyz);
		light *= shadow;
		color.rgb += lcolor * light;
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

	vec3 kD = vec3(1.0 - metalness);

	if(bool(params.mode & modeSpecularIBL)) {
		float cosTheta = clamp(dot(normal, -viewDir), 0.0, 1);
		vec3 kS = fresnelSchlickRoughness(cosTheta, f0, roughness);
		kD *= 1.0 - kS;

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
		// vec3 irradiance = evalIrradianceSH(normal, params.radianceCoeffs).rgb;
		vec3 diffuse = kD * irradiance * albedo.rgb;
		color.rgb += ambientFac * diffuse;
	}

	outCol = vec4(color, albedo.a);
	if(material.alphaCutoff == -1.f) { // alphaMode opque
		outCol.a = 1.f;
	}

	// tonemap
	// TODO: shouldn't be here but in post processing
	// destroys blending
	float exposure = 3.255e-05; // sunny16
	exposure /= 0.00001; // fp16 scale
	outCol.rgb *= exposure;

	outCol.rgb = 1.0 - exp(-outCol.rgb);
}
