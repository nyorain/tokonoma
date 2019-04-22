#version 450

#extension GL_GOOGLE_include_directive : enable

#include "noise.glsl"
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
	uniform subpassInput inPos; // TODO: remove
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inNormal;
layout(set = 1, binding = 2, input_attachment_index = 2)
	uniform subpassInput inAlbedo;

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

// TODO: move to scene.glsl
// ripped from http://www.alexandre-pestana.com/volumetric-lights/
float mieScattering(float lightDotView, float gs) {
	float result = 1.0f - gs * gs;
	result /= (4.0f * 3.141 * pow(1.0f + gs * gs - (2.0f * gs) * lightDotView, 1.5f));
	return result;
}

/*
// purely screen space old-school light scattering rays
// couldn't we do something like this in light/shadow space as well?
float lightScatterDepth(vec3 pos) {
	vec3 lrdir = light.pos.xyz - scene.viewPos;
	// float div = pow(dot(lrdir, lrdir), 0.6);
	// float div = length(lrdir);
	float div = 1.f;
	vec3 rayDir = normalize(pos - scene.viewPos);
	float ldv = dot(normalize(lrdir), rayDir);
	float fac = mieScattering(ldv, 0.01) / div;

	// vec3 lrdir = -normalize(light.dir.xyz);
	// float fac = dot(normalize(pos - scene.viewPos), normalize(lrdir));
	// float fac = mieScattering(dot(normalize(lrdir), rayDir), 0.95);
	// fac *= fac;

	vec2 rayStart = sceneMap(scene.proj, pos).xy;
	// vec3 rayEnd = sceneMap(scene.proj, light.dir.xyz); // TODO: light.position
	// vec3 rayEnd = sceneMap(scene.proj, -light.dir.xyz); // TODO: bad
	vec3 rayEnd = sceneMap(scene.proj, light.pos.xyz); // TODO: bad
	// if(rayEnd.xy != clamp(rayEnd.xy, 0.0, 1.0)) { // check if light is in screen
		// return 0.f;
	// }
	
	// only really have an effect if light is in center
	// also makes sure that light oustide of view doesn't have effect
	vec2 drayEnd = clamp(rayEnd.xy, 0.0, 1.0);
	if(drayEnd != rayEnd.xy) {
		return 0.f;
	}

	// fac *= ldv;
	fac *= pow(drayEnd.x * (1 - drayEnd.x), 0.5);
	fac *= pow(drayEnd.y * (1 - drayEnd.y), 0.5);

	float accum = 0.f;
	uint steps = 20u;
	vec2 ray = rayEnd.xy - rayStart;
	vec2 step = ray / steps;

	// TODO: instead use per-pixel dither pattern and smooth out
	// in pp pass. Needs additional scattering attachment though
	step += 0.05 * (2 * random(step) - 1) * step;
	rayStart += 0.1 * (2 * random(rayStart) - 1) * step;
	vec2 ipos = rayStart;

	for(uint i = 0u; i < steps; ++i) {
		// the z value of the lookup position is the value we compare with
		// accum += texture(depthTex, vec3(ipos, rayEnd.z)).r;
		float depth = texture(depthTex, ipos).r;
		accum += depth <= rayEnd.z ? 0.f : 1.f;
		ipos += step;
		if(ipos != clamp(ipos, 0, 1)) {
			break;
		}
	}

	// accum *= 0.25 * fac;
	accum *= fac;
	accum /= steps; // only count completed steps?
	accum = clamp(accum, 0.0, 1.0);
	return accum;
}

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

	vec4 sPos = subpassLoad(inPos);
	vec4 sNormal = subpassLoad(inNormal);
	vec4 sAlbedo = subpassLoad(inAlbedo);

	// vec3 pos = sPos.xyz;
	vec3 normal = sNormal.xyz;
	vec3 albedo = sAlbedo.xyz;

	float roughness = sPos.w; // TODO!
	// float roughness = 1.f; // TODO!
	float metallic = sNormal.w;
	float occlusion = sAlbedo.w;

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

	// TODO: remove random factors, implement pbr
	float ambientFac = 0.1 * occlusion; // TODO
	// float diffuseFac = 0.5f;
	// float specularFac = 0.5f;
	// float shininess = 64.f;

	float shadow;
	vec3 ldir;

	bool pcf = (dirLight.flags & lightPcf) != 0;
	if((dirLight.flags & lightDir) != 0) {
		// position on light tex
		vec4 lsPos = dirLight.proj * vec4(pos, 1.0);
		lsPos.xyz /= lsPos.w;
		lsPos.y *= -1; // invert y
		lsPos.xy = 0.5 + 0.5 * lsPos.xy; // normalize for texture access

		shadow = dirShadow(shadowMap, lsPos.xyz, int(pcf));
		ldir = normalize(dirLight.dir); // TODO: norm could be done on cpu
	} else {
		ldir = normalize(pos - pointLight.pos);
		if(pcf) {
			float radius = 0.005;
			shadow = pointShadowSmooth(shadowCube, pointLight.pos,
				pointLight.farPlane, pos, radius);
		} else {
			shadow = pointShadow(shadowCube, pointLight.pos,
				pointLight.farPlane, pos);
		}
	}

	/*
	// diffuse
	float lfac = 0.0;
	lfac += diffuseFac * max(dot(normal, -ldir), 0.0);

	// blinn-phong specular
	vec3 vdir = normalize(pos - scene.viewPos);
	vec3 halfway = normalize(-ldir - vdir);
	lfac += specularFac * pow(max(dot(normal, halfway), 0.0), shininess);
	*/

	vec3 v = normalize(scene.viewPos - pos);
	vec3 light = cookTorrance(normal, -ldir, v, roughness, metallic,
		albedo);

	// TODO: attenuation
	vec3 color = max(shadow * light * dirLight.color, 0.0);
	color += ambientFac * albedo * dirLight.color;

	fragColor = vec4(color, 1.0);
	// fragColor.rgb += lightScatterDepth(pos) * light.color.rgb;
	// fragColor.rgb += lightScatterShadow(pos) * light.color.rgb;
}
