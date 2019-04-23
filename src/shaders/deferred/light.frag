#version 450

#extension GL_GOOGLE_include_directive : enable

#include "scene.glsl"
#include "pbr.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

// scene
layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj;
	mat4 invProj;
	vec3 viewPos;
} scene;

// gbuffer
layout(set = 1, binding = 0, input_attachment_index = 0)
	uniform subpassInput inNormal;
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inAlbedo;

// NOTE: emission not supported yet
layout(set = 1, binding = 2, input_attachment_index = 2)
	uniform subpassInput inEmission;

// layout(set = 1, binding = 3) uniform sampler2DShadow depthTex;
layout(set = 1, binding = 3) uniform sampler2D depthTex;

layout(set = 2, binding = 0, row_major) uniform PointLightBuf {
	PointLight pointLight;
};
layout(set = 2, binding = 0, row_major) uniform DirLightBuf {
	DirLight dirLight;
};

layout(set = 2, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 2, binding = 1) uniform samplerCubeShadow shadowCube;

layout(push_constant) uniform Show {
	uint mode;
} show;

/*
// TODO: fix. Needs some serious changes for point lights
float lightScatterShadow(vec3 pos) {
	// NOTE: first attempt at light scattering
	// http://www.alexandre-pestana.com/volumetric-lights/
	// problem with this is that it doesn't work if background
	// is clear.
	vec3 rayStart = scene.viewPos;
	vec3 rayEnd = pos;
	vec3 ray = rayEnd - rayStart;

	float rayLength = length(ray);
	vec3 rayDir = ray / rayLength;
	rayLength = min(rayLength, 8.f);
	ray = rayDir * rayLength;

	// float fac = 1 / dot(normalize(-ldir), rayDir);
	// TODO
	vec3 lrdir = light.pos.xyz - scene.viewPos;
	// float div = pow(dot(lrdir, lrdir), 0.6);
	// float div = length(lrdir);
	float div = 1.f;
	float fac = mieScattering(dot(normalize(lrdir), rayDir), 0.01) / div;

	const uint steps = 20u;
	vec3 step = ray / steps;
	// offset slightly for smoother but noisy results
	// TODO: instead use per-pixel dither pattern and smooth out
	// in pp pass. Needs additional scattering attachment though
	// rayStart += 0.1 * random(rayEnd) * rayDir;
	step += 0.01 * random(step) * step;
	rayStart += 0.01 * random(rayEnd) * step;

	float accum = 0.0;
	pos = rayStart;

	// TODO: falloff over time
	// float ffac = 1.f;
	// float falloff = 0.9;
	for(uint i = 0u; i < steps; ++i) {
		// position in light space
		// vec4 pls = light.proj * vec4(pos, 1.0);
		// pls.xyz /= pls.w;
		// pls.y *= -1; // invert y
		// pls.xy = 0.5 + 0.5 * pls.xy; // normalize for texture access
// 
		// // float fac = max(dot(normalize(light.pd - pos), rayDir), 0.0); // TODO: light.position
		// // accum += fac * texture(shadowTex, pls.xyz).r;
		// // TODO: implement/use mie scattering function
		// accum += texture(shadowTex, pls.xyz).r;

		accum += pointShadow(shadowTex, light.pos, pos);
		pos += step;
	}

	// 0.25: random factor, should be configurable
	// accum *= 0.25 * fac;
	accum *= fac;
	accum /= steps;
	accum = clamp(accum, 0.0, 1.0);
	return accum;
}
*/

void main() {
	vec2 suv = 2 * uv - 1;
	suv.y *= -1.f; // flip y
	float depth = texture(depthTex, uv).r;
	vec4 pos4 = scene.invProj * vec4(suv, depth, 1.0);
	vec3 pos = pos4.xyz / pos4.w;
	// TODO: we can skip light (but not scattering) if depth == 1

	vec4 sNormal = subpassLoad(inNormal);
	vec4 sAlbedo = subpassLoad(inAlbedo);
	vec4 sEmission = subpassLoad(inEmission);

	vec3 normal = decodeNormal(sNormal.xy);
	// vec3 normal = sNormal.xyz;
	vec3 albedo = sAlbedo.xyz;

	float occlusion = sNormal.w;
	float roughness = sAlbedo.w;
	float metallic = sEmission.w;

	// debug modes
	switch(show.mode) {
	case 1:
		fragColor = vec4(albedo, 1.0);
		return;
	case 2:
		fragColor = vec4(normal, 1.0);
		return;
	case 3:
		fragColor = vec4(pos, 1.0);
		return;
	case 4:
		fragColor = vec4(vec3(pow(depth, 15)), 1.0);
		return;
	case 5:
		fragColor = vec4(vec3(occlusion), 1.0);
		return;
	case 6:
		fragColor = vec4(vec3(metallic), 1.0);
		return;
	case 7:
		fragColor = vec4(vec3(roughness), 1.0);
		return;
	default:
		break;
	}

	// TODO: where does this factor come from? make variable
	float ambientFac = 0.1 * occlusion;

	float shadow;
	vec3 ldir;
	vec3 mappedLightPos; // xy is screenspace, z is depth

	bool pcf = (dirLight.flags & lightPcf) != 0;
	if((dirLight.flags & lightDir) != 0) {
		// position on light tex
		vec4 lsPos = dirLight.proj * vec4(pos, 1.0);
		lsPos.xyz /= lsPos.w;
		lsPos.y *= -1; // invert y
		lsPos.xy = 0.5 + 0.5 * lsPos.xy; // normalize for texture access

		shadow = dirShadow(shadowMap, lsPos.xyz, int(pcf));
		ldir = normalize(dirLight.dir); // TODO: norm could be done on cpu

		// TODO: could be done on cpu!
		// mapped position of a directional light
		// manually depth clamp
		mappedLightPos = sceneMap(scene.proj, scene.viewPos - ldir);
		if(mappedLightPos.z != clamp(mappedLightPos.z, 0, 1)) {
			mappedLightPos.xy = vec2(0.0);
		}

		mappedLightPos.z = 1.f; // on far plane
	} else {
		if(pcf) {
			float radius = 0.005;
			shadow = pointShadowSmooth(shadowCube, pointLight.pos,
				pointLight.farPlane, pos, radius);
		} else {
			shadow = pointShadow(shadowCube, pointLight.pos,
				pointLight.farPlane, pos);
		}

		ldir = normalize(pos - pointLight.pos);
		mappedLightPos = sceneMap(scene.proj, pointLight.pos);
	}

	vec3 v = normalize(scene.viewPos - pos);
	vec3 light = cookTorrance(normal, -ldir, v, roughness, metallic,
		albedo);

	// TODO: attenuation
	vec3 color = max(shadow * light * dirLight.color, 0.0);
	color += ambientFac * albedo * dirLight.color;

	fragColor = vec4(color, 1.0);

	vec2 mappedFragPos = sceneMap(scene.proj, pos).xy;
	float scatter = lightScatterDepth(mappedFragPos, mappedLightPos.xy,
		mappedLightPos.z, ldir, -v, depthTex);
	fragColor.rgb += scatter * dirLight.color.rgb;
}
