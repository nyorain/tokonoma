#version 460

#extension GL_GOOGLE_include_directive : require
#include "precoscat.hpp"

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _vp;
	vec3 pos;
	uint _;
	Atmosphere atmosphere;
} scene;

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 3) uniform samplerCube heightmap;
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
	vec3 pos = normalize(inPos);

	vec2 lod = textureQueryLod(heightmap, pos);
	vec4 h = textureLod(heightmap, pos, lod.x);

	vec2 tp = sph2(pos);
	float theta = tp[0];
	float phi = tp[1];

	vec3 dphi = sph_dphi(1.f, theta, phi);
	vec3 dtheta = sph_dtheta(1.f, theta, phi);
	float fac = 0.03;
	vec3 n = cross(
		(1 + fac * h.x) * dtheta + dot(dtheta, fac * h.yzw) * pos, 
		(1 + fac * h.x) * dphi + dot(dphi, fac * h.yzw) * pos);
	n = normalize(n);

	const vec3 toSun = vec3(0, 1, 0);
	// naive lighting
	// float l = max(dot(n, toSun), 0.0);
	// fragColor = vec4(vec3(l), 1);

	// atmosphere-based lighting
	float r = length(inPos);
	float mu_s = dot(inPos, toSun) / r;

	float hlod2 = textureLod(heightmap, pos, lod.x + 2.f).r;
	float hlod3 = textureLod(heightmap, pos, lod.x + 3.f).r;
	float hlod4 = textureLod(heightmap, pos, lod.x + 4.f).r;
	float hlod5 = textureLod(heightmap, pos, lod.x + 5.f).r;
	float hlod6 = textureLod(heightmap, pos, lod.x + 6.f).r;
	float ao = 0.1;

	float f0 = -0.05 * pow(2.0, lod.x);
	float f1 = 0.01 * pow(2.0, lod.x);
	ao += 0.4 * addAO(0.5 * f0, 0.5 * f1, h.x - hlod2);
	ao += 0.4 * addAO(1 * f0, 1 * f1, h.x - hlod3);
	ao += 0.8 * addAO(2 * f0, 2 * f1, h.x - hlod4);
	ao += 1.0 * addAO(3 * f0, 3 * f1, h.x - hlod5);
	ao += 1.0 * addAO(4 * f0, 4 * f1, h.x - hlod6);
	ao /= 4;

	// Indirect irradiance (approximated if the surface is not horizontal)
	ARay toSunRay = {r, mu_s};
	vec3 skyIrradiance = getIrradiance(scene.atmosphere, irradianceTex, toSunRay) *
		(1.0 + dot(n, pos)) * 0.5 * ao;

	// Direct irradiance
	vec3 sunIrradiance = scene.atmosphere.solarIrradiance.rgb *
		transmittanceToSun(scene.atmosphere, transTex, toSunRay) *
		max(dot(n, toSun), 0.0);

	vec3 refl = (1 / pi) * vec3(scene.atmosphere.groundAlbedo) * (skyIrradiance + sunIrradiance);
	fragColor = vec4(refl, 1);
	// fragColor = vec4(vec3(10000 * ao), 1);
}
