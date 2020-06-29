#version 460

#extension GL_GOOGLE_include_directive : require
#include "precoscat.hpp"
#include "terrain.glsl"
#include "snoise.glsl"

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _vp;
	vec3 pos;
	uint _0;
	uvec3 centerTile;
	uint _1;
	Atmosphere atmosphere;
} scene;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inVPos;
// layout(location = 2) in vec3 inNormal;

layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 3) uniform sampler2DArray heightmap;
layout(set = 0, binding = 4) uniform sampler2D transTex;
layout(set = 0, binding = 5) uniform sampler2D irradianceTex;

// Transform of coords on unit sphere to spherical coordinates (theta, phi)
vec2 sph2(vec3 pos) {
	// TODO: undefined output in that case...
	if(abs(pos.y) > 0.999) {
		return vec2(0, (pos.y > 0) ? 0.0001 : pi);
	}
	float theta = atan(pos.z, pos.x);
	float phi = atan(length(pos.xz), pos.y);
	return vec2(theta, phi);
}

// Derivation of spherical coordinates in euclidean space with respect to theta.
vec3 sph_dtheta(float radius, float theta, float phi) {
	float sp = sin(phi);
	return radius * vec3(-sin(theta) * sp, 0, cos(theta) * sp);
}

// Derivation of spherical coordinates in euclidean space with respect to phi.
vec3 sph_dphi(float radius, float theta, float phi) {
	float cp = cos(phi);
	float sp = sin(phi);
	return radius * vec3(cos(theta) * cp, -sp, sin(theta) * cp);
}

float addAO(float f0, float f1, float x) {
	// f0 = clamp(f0, -1.f, 0.f);
	// f1 = clamp(f0, 0.f, 1.f);
	return smoothstep(f0, f1, x);
}

vec3 detailNormal(vec3 pos, float lod) {
	vec3 sum = vec3(0.f);
	float lacunarity = 2.0;
	float gain = 0.65;

	float amp = 0.5f; // ampliture
	float mod = 1.f; // modulation
	for(int i = 0; i < max(3 - 2 * lod, 0.0); ++i) {
		vec3 grad;
		snoise(mod * pos, grad);
		sum += amp * mod * grad;

		mod *= lacunarity;
		amp *= gain;
	}

	return sum;
}

void main() {
	// vec3 dx = dFdx(inVPos);
	// vec3 dy = dFdy(inVPos);
	// vec3 n = normalize(-cross(dx, dy));
	// vec3 n = normalize(inNormal);

	vec3 pos = normalize(inPos);

	// vec3 opos = displace(inPos, scene.centerTile, heightmap);
	// vec3 dx = dFdx(opos);
	// vec3 dy = dFdy(opos);
	// vec3 n = normalize(-cross(dx, dy));

	// vec2 tp = sph2(pos);
	// float theta = tp[0];
	// float phi = tp[1];
	// vec3 dphi = sph_dphi(1.f, theta, phi);
	// vec3 dtheta = sph_dtheta(1.f, theta, phi);
	// float fac = displaceStrength;

	// TODO
	bool valid; // TODO: check
	vec3 worldS, worldT;
	vec3 hc = heightmapCoords(pos, scene.centerTile, 0, valid, worldS, worldT);
	float disp = texture(heightmap, hc).r;
	float x0 = textureOffset(heightmap, hc, ivec2(-1, 0)).r;
	float x1 = textureOffset(heightmap, hc, ivec2(1, 0)).r;
	float y0 = textureOffset(heightmap, hc, ivec2(0, -1)).r;
	float y1 = textureOffset(heightmap, hc, ivec2(0, 1)).r;

	float lod = hc.z;
	float nTiles = 1 + 2 * exp2(lod);
	float numFaces = nTiles / nTilesPD;
	vec2 pixLength = 2 * numFaces / textureSize(heightmap, 0).xy;	

	float dx = displaceStrength * (x1 - x0) / (2 * pixLength.x);
	float dy = displaceStrength * (y1 - y0) / (2 * pixLength.y);
	Face face = cubeFaces[scene.centerTile.z];
	// vec3 hn = normalize(face.dir + dx * face.t + dy * face.s);

	// TODO: probably not correct
	// vec3 t = cross(pos, face.s);
	// vec3 s = -cross(pos, t);
	// vec3 s = dFdx(hc.xy);
	// vec3 t = dFdy(pos);
	float mod = 1024 * 8;
	// vec3 n = normalize(pos + dx * worldS + dy * worldT + 
	// 	0.02 * detailNormal(mod * pos, lod));
	vec3 n = normalize(pos + dx * worldS + dy * worldT + 
		0.02 * detailNormal(mod * pos, lod));

	// float mod = 1024 * 16;
	// vec3 n = normalize(inNormal + 0.05 * detailNormal(mod * pos, 0));

	const vec3 toSun = vec3(0, 1, 0);
	// naive lighting
	// float l = max(dot(n, toSun), 0.0);
	// fragColor = vec4(vec3(l), 1);

	// atmosphere-based lighting
	float r = length(inPos);
	float mu_s = dot(inPos, toSun) / r;

	float ao = 0.5; // TODO
	/*
	float hlod2 = textureLod(heightmap, pos, lod.x + 2.f).r;
	float hlod3 = textureLod(heightmap, pos, lod.x + 3.f).r;
	float hlod4 = textureLod(heightmap, pos, lod.x + 4.f).r;
	float hlod5 = textureLod(heightmap, pos, lod.x + 5.f).r;
	float hlod6 = textureLod(heightmap, pos, lod.x + 6.f).r;
	float ao = 0.1;

	float f0 = -0.035 * pow(2.0, lod.x);
	float f1 = 0.015 * pow(2.0, lod.x);
	ao += 0.4 * addAO(0.5 * f0, 0.5 * f1, h.x - hlod2);
	ao += 0.4 * addAO(1 * f0, 1 * f1, h.x - hlod3);
	ao += 0.8 * addAO(2 * f0, 2 * f1, h.x - hlod4);
	ao += 1.0 * addAO(3 * f0, 3 * f1, h.x - hlod5);
	ao += 1.0 * addAO(4 * f0, 4 * f1, h.x - hlod6);
	ao /= 4;
	*/

	// Indirect irradiance (approximated if the surface is not horizontal)
	ARay toSunRay = {r, mu_s};
	vec3 skyIrradiance = getIrradiance(scene.atmosphere, irradianceTex, toSunRay) *
		(1.0 + dot(n, pos)) * 0.5 * ao;

	// Direct irradiance
	vec3 sunIrradiance = scene.atmosphere.solarIrradiance.rgb *
		transmittanceToSun(scene.atmosphere, transTex, toSunRay) *
		(max(dot(n, toSun), 0.0));

	vec3 albedo = vec3(scene.atmosphere.groundAlbedo);
	// vec3 albedo = vec3(0.3, 0.3, 0.3);
	vec3 refl = (1 / pi) * albedo * (skyIrradiance + sunIrradiance);
	fragColor = vec4(refl, 1);
}

