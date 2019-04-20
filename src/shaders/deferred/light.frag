#version 450

#extension GL_GOOGLE_include_directive : enable

#include "noise.glsl"

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
	uniform subpassInput inPos;
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inNormal;
layout(set = 1, binding = 2, input_attachment_index = 2)
	uniform subpassInput inAlbedo;
// layout(set = 1, binding = 3) uniform sampler2DShadow depthTex;
layout(set = 1, binding = 3) uniform sampler2D depthTex;

layout(set = 2, binding = 0, row_major) uniform Light {
	mat4 matrix; // from global to light space
	vec3 pd; // position for point light, direction of dir light
	uint type; // point or dir
	vec3 color;
	uint pcf;
} light;

layout(set = 2, binding = 1) uniform sampler2DShadow shadowTex;

layout(push_constant) uniform Show {
	uint mode;
} show;

const uint pointLight = 1;
const uint dirLight = 2;

// samples the shadow texture multiple times to get smoother shadow
// pos: position in light space
// returns (1 - shadow), i.e. the light factor
float shadowpcf(vec3 pos, int range) {
	if(pos.z > 1.0) {
		return 1.0;
	}

	vec2 texelSize = 1.f / textureSize(shadowTex, 0);
	float sum = 0.f;
	for(int x = -range; x <= range; ++x) {
		for(int y = -range; y <= range; ++y) {
			// sampler has builtin comparison
			vec3 off = vec3(texelSize * vec2(x, y),  0);
			sum += texture(shadowTex, pos + off).r;
		}
	}

	float total = ((2 * range + 1) * (2 * range + 1));
	return sum / total;
}

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
	rayLength = min(rayLength, 4.f);
	ray = rayDir * rayLength;

	vec3 lrdir = (light.type == pointLight) ?
		light.pd - scene.viewPos :
		-light.pd;
	float fac = dot(normalize(lrdir), rayDir);

	// offset slightly for smoother but noisy results
	// TODO: instead use per-pixel dither pattern and smooth out
	// in pp pass. Needs additional scattering attachment though
	rayStart += 0.1 * random(rayEnd) * rayDir;

	const uint steps = 25u;
	vec3 step = ray / steps;
	float accum = 0.0;
	pos = rayStart;
	for(uint i = 0u; i < steps; ++i) {
		// position in light space
		vec4 pls = light.matrix * vec4(pos, 1.0);
		pls.xyz /= pls.w;
		pls.y *= -1; // invert y
		pls.xy = 0.5 + 0.5 * pls.xy; // normalize for texture access
		// float fac = max(dot(normalize(light.pd - pos), rayDir), 0.0); // TODO: light.position
		// accum += fac * texture(shadowTex, pls.xyz).r;
		// TODO: implement/use mie scattering function
		accum += texture(shadowTex, pls.xyz).r;
		pos += step;
	}

	// 0.25: random factor, should be configurable
	accum *= 0.25 * fac;
	accum /= steps;
	accum = clamp(accum, 0.0, 1.0);
	return accum;
}

vec3 sceneMap(vec3 pos) {
	vec4 pos4 = scene.proj * vec4(pos, 1.0);
	vec3 mapped = pos4.xyz / pos4.w;
	mapped.y *= -1; // invert y
	mapped.xy = 0.5 + 0.5 * mapped.xy; // normalize for texture access
	return mapped;
}

// purely screen space old-school light scattering rays
// couldn't we do something like this in light/shadow space as well?
float lightScatterDepth(vec3 pos) {
	vec3 lrdir = (light.type == pointLight) ?
		light.pd - scene.viewPos :
		-light.pd;
	float fac = dot(normalize(pos - scene.viewPos), normalize(lrdir));
	if(fac < 0) {
		return 0.f;
	}

	// fac *= fac;

	vec2 rayStart = sceneMap(pos).xy;
	vec3 rayEnd = sceneMap(light.pd); // TODO: light.position
	// rayEnd.xy = clamp(rayEnd.xy, 0.0, 1.0);
	float accum = 0.f;
	uint steps = 25u;
	vec2 ray = rayEnd.xy - rayStart;
	vec2 step = ray / steps;

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
	accum *= 0.1 * fac;
	accum /= steps; // only done steps?
	accum = clamp(accum, 0.0, 1.0);
	return accum;
}

void main() {
	vec2 suv = 2 * uv - 1;
	suv.y *= -1.f; // flip y
	float depth = texture(depthTex, uv).r;
	vec4 pos4 = scene.invProj * vec4(suv, depth, 1.0);
	vec3 pos = pos4.xyz / pos4.w;
	// TODO: we can skip light (but not scattering) if depth == 1

	// vec4 sPos = subpassLoad(inPos);
	vec4 sNormal = subpassLoad(inNormal);
	vec4 sAlbedo = subpassLoad(inAlbedo);

	// vec3 pos = sPos.xyz;
	vec3 normal = sNormal.xyz;
	vec3 albedo = sAlbedo.xyz;

	// float roughness = sPos.w;
	float roughness = 1.f; // TODO!
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
	float diffuseFac = 0.5f;
	float specularFac = 0.5f;
	float shininess = 64.f;

	// position of texel in light space
	float lfac = 0.0;
	vec3 ldir = (light.type == pointLight) ?
		pos - light.pd :
		light.pd;
	ldir = normalize(ldir);

	// diffuse
	lfac += diffuseFac * max(dot(normal, -ldir), 0.0);

	// blinn-phong specular
	vec3 vdir = normalize(pos - scene.viewPos);
	vec3 halfway = normalize(-ldir - vdir);
	lfac += specularFac * pow(max(dot(normal, halfway), 0.0), shininess);

	// shadow
	vec4 lsPos = light.matrix * vec4(pos, 1.0);
	lsPos.xyz /= lsPos.w;
	lsPos.y *= -1; // invert y
	lsPos.xy = 0.5 + 0.5 * lsPos.xy; // normalize for texture access
	lfac *= shadowpcf(lsPos.xyz, int(light.pcf));

	// ambient always added, indepdnent from shadow
	lfac += ambientFac;
	fragColor = vec4(lfac * light.color * albedo, 1.0);

	// fragColor.rgb += lightScatterShadow(pos) * light.color;
	fragColor.rgb += lightScatterDepth(pos) * light.color;
}
