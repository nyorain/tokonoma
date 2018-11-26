float random(float v) {
	return fract(sin(7.16294 * v) * 518412.142398574);
}

float random(vec2 v) {
	return random(dot(v, vec2(96.342291, -235.91764)));
}

float random(vec3 v) {
	return random(dot(v, vec3(96.342291, -235.91764, 12.63231)));
}

float random(vec4 v) {
	return random(dot(v, vec4(-0.921286, 96.342291, -235.91764, 12.63231)));
}

float valueNoise(vec2 st) {
    vec2 i = floor(st);
    vec2 f = fract(st);

    float a = random(i);
    float b = random(i + vec2(1.0, 0.0));
    float c = random(i + vec2(0.0, 1.0));
    float d = random(i + vec2(1.0, 1.0));

	vec2 u = smoothstep(0, 1, f);
	return mix(mix(a, c, u.y), mix(b, d, u.y), u.x);
}

#ifndef FBM_OCTAVES
	#define FBM_OCTAVES 8
#endif

#ifndef FBM_NOISE
	#define FBM_NOISE valueNoise
#endif

float fbm(vec2 st) {
	float sum = 0.f;
	float lacunarity = 2.0;
	float gain = 0.5;

	float amp = 0.5f; // ampliture
	float mod = 1.f; // modulation
	for(int i = 0; i < FBM_OCTAVES; ++i) {
		// sum += amp * valueNoise(2 * random(mod * amp) + mod * st);
		sum += amp * valueNoise(mod * st);
		mod *= lacunarity;
		amp *= gain;
		
	}

	return sum;
}
