#version 460

#extension GL_GOOGLE_include_directive : require
#include "precoscat.hpp"
#include "terrain.glsl"

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
layout(location = 2) in vec3 inNormal;

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

void main() {
// 	vec3 dx = dFdx(inVPos);
// 	vec3 dy = dFdy(inVPos);
	// vec3 n = normalize(-cross(dx, dy));
	vec3 n = normalize(inNormal);

	vec3 pos = normalize(inPos);

	// vec2 lod = textureQueryLod(heightmap, pos);
	// vec4 h = height(pos, scene.centerTile, heightmap);

/*
    vec2 tuv;
    uvec3 tpos = tilePos(pos, tuv);
	ivec2 toff;
    if(scene.centerTile.z == tpos.z) {
		// The easy case: both are on the same face.
        toff = ivec2(tpos.xy) - ivec2(scene.centerTile.xy);
    } else {
		// TODO
		// return vec4(1.0, 0.0, 0.0, 1.0);

		// TODO: not really sure how to store it. Maybe an extra
		// last lod just for it?
		if(opposite(tpos.z, scene.centerTile.z)) {
			// return vec4(0.0, normalize(pos));
			// return vec4(0.0);
		}

		ivec2 off;
		mat2 fm = flipUVMatrix(scene.centerTile.z, tpos.z, off);
		if(determinant(fm) > 1.01 || determinant(fm) < 0.99) {
			fragColor = vec4(0.0);
			return;
		}

		// tuv = fm * tuv + off; // TODO: use off here?
		// tuv = abs(modtrunc(fm * tuv, vec2(1)));
		// tuv = 0.5 + 0.5 * (fm * tuv);
		tuv = fract(fm * tuv);
		// toff = ivec2(trunc(fm * (tpos.xy + 0.5)) + int(nTilesPD) * off) - ivec2(scene.centerTile.xy);
		// toff = ivec2(trunc(fm * (tpos.xy + 0.5)) + nTilesPD * off);
		toff = ivec2(fm * tpos.xy) + int(nTilesPD) * off - ivec2(scene.centerTile.xy);
	}

	int m = max(abs(toff.x), abs(toff.y));
	int lod = int(ceil(log2(max(m, 1))));
*/

	// TODO
	// fragColor = vec4(1000 * h.xyz, 1.0);
	// fragColor = vec4(1000 * lod, 100 * lod, 20 * lod, 1.0);
	// fragColor = vec4(1000 - 200 * toff, 0.0, 1.0);

	// outlines
	// if(min(tuv.x, tuv.y) < 0.001 || max(tuv.x, tuv.y) > 0.99) {
	// 	fragColor = vec4(vec3(10000), 1.0);
	// }

	// return;

	vec2 tp = sph2(pos);
	float theta = tp[0];
	float phi = tp[1];

	vec3 dphi = sph_dphi(1.f, theta, phi);
	vec3 dtheta = sph_dtheta(1.f, theta, phi);
	float fac = displaceStrength;

	// float fac = 0.05;
	// vec3 n = cross(
	// 	6360 * (1 + fac * h.x) * dtheta + dot(dtheta, fac * h.yzw) * inPos, 
	// 	6360 * (1 + fac * h.x) * dphi + dot(dphi, fac * h.yzw) * inPos);
	// vec3 n = cross(
	// 	(1 + fac * h.x) * dtheta + dot(dtheta, fac * h.yzw) * pos, 
	// 	(1 + fac * h.x) * dphi + dot(dphi, fac * h.yzw) * pos);
	// n = normalize(n);

	// TODO
	// n = 0.5 + 0.5 * n;
	// fragColor = vec4(1000 * n, 1);
	// return;

	const vec3 toSun = vec3(0, 1, 0);
	// naive lighting
	// float l = max(dot(n, toSun), 0.0);
	// fragColor = vec4(vec3(l), 1);

	// atmosphere-based lighting
	float r = length(inPos);
	float mu_s = dot(inPos, toSun) / r;

	float ao = 1.0; // TODO
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
		max(dot(n, toSun), 0.0);

	// vec3 albedo = vec3(scene.atmosphere.groundAlbedo);
	vec3 albedo = vec3(0.8, 0.8, 0.8);
	vec3 refl = (1 / pi) * albedo * (skyIrradiance + sunIrradiance);
	fragColor = vec4(refl, 1);
	// fragColor = vec4(vec3(10000 * ao), 1);
}

