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
layout(location = 6) in vec4 inClipPos;
layout(location = 7) in vec4 inClipLastPos;

layout(location = 0) out vec4 outCol;
layout(location = 1) out vec4 outVel; // in screen space, z is linear depth, a unused

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj;
	mat4 _lastProj;
	vec2 _jitter;
	float near, far;
	vec3 viewPos; // camera position. For specular light
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

float mrandom(vec4 seed4) {
	float dot_product = dot(seed4, vec4(12.9898,78.233,45.164,94.673));
	return fract(sin(dot_product) * 43758.5453);
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
		vec3 bump = 2 * texture(sampler2D(textures[nt.id], samplers[nt.samplerID]), tuv).xyz - 1;
		normal = tbnNormal(normal, inPos, tuv, bump);
	}

	if(!gl_FrontFacing) { // flip normal, see gltf spec
		normal *= -1;
	}

	vec3 color = vec3(0.0);

	vec3 viewDir = normalize(inPos - scene.viewPos);
	vec4 mr = readTex(material.metalRough);
	float metalness = material.metallicFac * mr.b;
	float roughness = material.roughnessFac * mr.g;

	bool taaShadow = false;
	if(bool(params.mode & modeDirLight)) {
		float between;
		float linearz = depthtoz(gl_FragCoord.z, scene.near, scene.far);
		uint index = getCascadeIndex(dirLight, linearz, between);

		float shadow;
		if(!taaShadow) {
			// color the different levels for debugging
			// switch(index) {
			// 	case 0: lcolor = mix(lcolor, vec3(1, 1, 0), 0.8); break;
			// 	case 1: lcolor = mix(lcolor, vec3(0, 1, 1), 0.8); break;
			// 	case 2: lcolor = mix(lcolor, vec3(1, 0, 1), 0.8); break;
			// 	case 3: lcolor = mix(lcolor, vec3(1, 0, 1), 0.8); break;
			// 	default: lcolor = vec3(1, 1, 1); break;
			// };

			bool pcf = bool(dirLight.flags & lightPcf);
			shadow = dirShadowIndex(dirLight, shadowMap, inPos, index, int(pcf));
		} else {
			// custom wip shadow sampling, mainly for TAA
			shadow = 0.0;
			vec3 spos = sceneMap(cascadeProj(dirLight, index), inPos);
			if(spos.z > 0.0 && spos.z < 1.0) {
				const vec2 poissonDisk[16] = vec2[]( 
				   vec2( -0.94201624, -0.39906216 ), 
				   vec2( 0.94558609, -0.76890725 ), 
				   vec2( -0.094184101, -0.92938870 ), 
				   vec2( 0.34495938, 0.29387760 ), 
				   vec2( -0.91588581, 0.45771432 ), 
				   vec2( -0.81544232, -0.87912464 ), 
				   vec2( -0.38277543, 0.27676845 ), 
				   vec2( 0.97484398, 0.75648379 ), 
				   vec2( 0.44323325, -0.97511554 ), 
				   vec2( 0.53742981, -0.47373420 ), 
				   vec2( -0.26496911, -0.41893023 ), 
				   vec2( 0.79197514, 0.19090188 ), 
				   vec2( -0.24188840, 0.99706507 ), 
				   vec2( -0.81409955, 0.91437590 ), 
				   vec2( 0.19984126, 0.78641367 ), 
				   vec2( 0.14383161, -0.14100790 ) 
				);

				for (int i = 0; i < 7; i++){
					// we could make the length dependent on the
					// distance behind the first sample or something... (i.e.
					// make the shadow smoother when further away from
					// shadow caster).
					float len = 2 * mrandom(vec4(gl_FragCoord.xyy + 100 * inPos.xyz, i));
					float rid = mrandom(vec4(0.1 * gl_FragCoord.yxy - 32 * inPos.yzx, i));
					int id = int(16.0 * rid) % 16;
					vec2 off = len * poissonDisk[id] / textureSize(shadowMap, 0).xy;
					shadow += 0.25 * texture(shadowMap, vec4(spos.xy + off, index, spos.z)).r;
				}
			}
		}

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

	// output velocity
	vec3 ndcCurr = inClipPos.xyz / inClipPos.w;
	vec3 ndcLast = inClipLastPos.xyz / inClipLastPos.w;
	ndcCurr.z = depthtoz(ndcCurr.z, scene.near, scene.far);
	ndcLast.z = depthtoz(ndcLast.z, scene.near, scene.far);
	vec3 vel = ndcCurr - ndcLast;
	outVel = vec4(0.5 * vel.xy, vel.z, 0.0);
}
