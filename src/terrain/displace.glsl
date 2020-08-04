#include "noise.glsl"
#include "snoise.glsl"

float vfbm(vec2 st) {
	float sum = 0.f;
	float lacunarity = 2.0;
	float gain = 0.5;

	float amp = 0.5f; // ampliture
	float mod = 1.f; // modulation
	for(int i = 0; i < 2; ++i) {
		sum += amp * (voronoiNoise2(mod * st)) * gradientNoise(2 * mod * st);
		mod *= lacunarity;
		amp *= gain;
		st = lacunarity * st;
	}

	return sum;
}

float fbm(vec3 pos) {
	float sum = 0.0;
	sum += snoise(pos);
	sum += 0.5 * snoise(2 * pos);
	sum += 0.25 * snoise(4 * pos);
	sum += 0.1 * snoise(8 * pos);
	sum += 0.05 * snoise(16 * pos);
	sum += 0.025 * snoise(32 * pos);
	return 0.25 * sum;
}

vec3 displace(vec3 pos) {
	pos.y += 1.0 * vfbm(2 + pos.xz);
	return pos;
}

