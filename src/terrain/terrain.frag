#version 450
#extension GL_GOOGLE_include_directive : require

#include "pbr.glsl"
#include "constants.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) noperspective in vec3 inBary;

layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp;
	vec3 viewPos;
	float _;
	vec3 toLight;
} scene;

layout(set = 0, binding = 3) uniform sampler2D heightmap;
layout(set = 0, binding = 4) uniform sampler2D shadowmap;

// globals
vec2 baseCoord = 0.5 + 0.5 * inPos.xz;
vec2 pixLength = 1.0 / textureSize(heightmap, 0);
vec3 toCam = normalize(scene.viewPos - inPos);

// functions
float min3(vec3 v) {
	return min(min(v.x, v.y), v.z);
}

float outline() {
	float bm = min3(inBary);
	float dx = dFdx(bm);
	float dy = dFdy(bm);
	float d = length(vec2(dx, dy));

	float f = 1.f;
	if(bm < d) {
		f *= mix(smoothstep(0.0, d, bm), 1.0, smoothstep(0.0, 0.5, d));
	}

	return f;
}


float shadow() {
	// return 1.0;

#ifdef SHADOW_MANUAL
	float dt;
	float shadow = 1.f;
	for(float t = 0.05; t < 1.f && shadow > 0.01f; t += dt) {
		vec3 pos = inPos + t * scene.toLight;
		float lod = clamp(t / 0.25, 0.0, 10);
		float height = textureLod(heightmap, 0.5 + 0.5 * pos.xz, lod).r;
		if(pos.xz == clamp(pos.xz, -1.f, 1.f) && height > pos.y) {
			shadow *= exp(-500 * t * (height - pos.y));
		}

		dt = 0.02 * t;
	}

	return shadow;
#else
	return texture(shadowmap, baseCoord).r;
	// return exp(-100 * texture(shadowmap, baseCoord).r);
#endif
}

vec3 computeAO(vec3 normal) {
	// return vec3(0.01);

	float ao = 1.0;
	float height = textureLod(heightmap, baseCoord, 0).r;

// #define AO_MANUAL
#ifdef AO_MANUAL
	for(int y = -1; y <= 1; ++y) {
		for(int x = -1; x <= 1; ++x) {
			float s = texture(heightmap, baseCoord + 30 * vec2(x, y) * pixLength).r;
			ao *= min(1.0, exp(-10 * (s - height)));
		}
	}
#else
	float rough1 = textureLod(heightmap, baseCoord, 2u).r;
	float rough2 = textureLod(heightmap, baseCoord, 3u).r;
	float rough3 = textureLod(heightmap, baseCoord, 4u).r;
	float rough4 = textureLod(heightmap, baseCoord, 5u).r;
	float rough5 = textureLod(heightmap, baseCoord, 6u).r;
	float rough6 = textureLod(heightmap, baseCoord, 8u).r;
	float rough7 = textureLod(heightmap, baseCoord, 10u).r;

	ao *= min(1.0, exp(-500 * (rough1 - height)));
	ao *= min(1.0, exp(-300 * (rough2 - height)));
	ao *= min(1.0, exp(-150 * (rough3 - height)));
	ao *= min(1.0, exp(-100 * (rough4 - height)));
	ao *= min(1.0, exp(-50 * (rough5 - height)));
	ao *= min(1.0, exp(-20 * (rough6 - height)));
	ao *= min(1.0, exp(-10 * (rough7 - height)));
#endif

	return 0.1 * ao * ambientColor * (0.1 + 0.9 * max(normal.y, 0.0));
}

void main() {
	float x0 = textureLodOffset(heightmap, baseCoord, 0, ivec2(-1, 0)).r;
	float x1 = textureLodOffset(heightmap, baseCoord, 0, ivec2(1, 0)).r;
	float z0 = textureLodOffset(heightmap, baseCoord, 0, ivec2(0, -1)).r;
	float z1 = textureLodOffset(heightmap, baseCoord, 0, ivec2(0, 1)).r;

	float dx = 0.5 * (x1 - x0) / pixLength.x;
	float dz = 0.5 * (z1 - z0) / pixLength.y;

	vec3 n = normalize(vec3(0, 1, 0) - dx * vec3(1, 0, 0) - dz * vec3(0, 0, 1));

	// sun lighting
	vec3 light = vec3(0.0);
	const vec3 albedo = vec3(1.0);

	vec3 ao = computeAO(n);
	// light += max(dot(scene.toLight, n), 0.0) * lightColor * 0.4 * (shadow() + 10 * ao);

	float roughness = inPos.y <= allMin ? 0.2 : 0.8;
	light += cookTorrance(n, scene.toLight, toCam,
		roughness, 0.1, albedo) * lightColor * shadow();

	// ao
	light += ao * albedo;

	// outlines?
	// light += 0.1 * (1 - outline()) * vec3(1.0, 0.2, 1.0);

	// apply
	outFragColor = vec4(light, 1.0);
}
