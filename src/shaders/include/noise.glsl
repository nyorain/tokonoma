// The random/noise/fbm functions return a number between 0 and 1
float random(float v) {
    float a = 43758.5453;
    float sn = mod(v, 3.14);
    return fract(sin(sn) * a);
}

float random(vec2 v) {
    float a = 43758.5453;
    float b = 12.9898;
    float c = 78.233;
    float dt = dot(v, vec2(b, c));
    float sn = mod(dt, 3.14);
    return fract(sin(sn) * a);
}

float random(vec3 v) {
    float a = 43758.5453;
    float b = 12.9898;
    float c = 78.233;
    float d = 4.749;
    float dt = dot(v, vec3(b, c, d));
    float sn = mod(dt, 3.14);
    return fract(sin(sn) * a);
}

float random(vec4 v) {
    float a = 43758.5453;
    float b = 12.9898;
    float c = 78.233;
    float d = 4.749;
    float e = 51.0531;
    float dt = dot(v, vec4(b, c, d, e));
    float sn = mod(dt, 3.14);
    return fract(sin(sn) * a);
}

vec2 random2(vec2 v) {
	return vec2(
		random(dot(v, vec2(15.32, 64.234))),
		random(dot(v, vec2(-35.2234, 24.23588453))));
}

vec3 random3(vec3 v) {
	return vec3(
		random(dot(v, vec3(63.45202, -14.034012, 53.35302))),
		random(dot(v, vec3(101.32, -23.990653, 29.53))),
		random(dot(v, vec3(-31.504, 83.23, 45.2))));
}

vec4 random4(vec4 v) {
	return vec4(
		random(dot(v, vec4(52.0354, -46.34512, 7.924, 13.64))),
		random(dot(v, vec4(11.652, -21.9653, 6.2401, 345.23))),
		random(dot(v, vec4(-13.4015, 36.012, -3.85, 26.2))),
		random(dot(v, vec4(-12.35014, 97.1061, 96.2, -91.34))));
}

float valueNoise(vec2 v) {
    vec2 i = floor(v);
    vec2 f = fract(v);
	vec2 u = smoothstep(0, 1, f);

    float a = random(i + vec2(0, 0));
    float b = random(i + vec2(1, 0));
    float c = random(i + vec2(0, 1));
    float d = random(i + vec2(1, 1));

	return mix(mix(a, c, u.y), mix(b, d, u.y), u.x);
}

// signed [-1, 1] gradient noise
float sgradientNoise(vec2 v) {
	vec2 i = floor(v);
	vec2 f = fract(v);
	vec2 u = smoothstep(0, 1, f);

	// random gradients, needed in range [-1, 1]
    float a = dot(-1 + 2 * random2(i + vec2(0, 0)), f - vec2(0, 0));
    float b = dot(-1 + 2 * random2(i + vec2(1, 0)), f - vec2(1, 0));
    float c = dot(-1 + 2 * random2(i + vec2(0, 1)), f - vec2(0, 1));
    float d = dot(-1 + 2 * random2(i + vec2(1, 1)), f - vec2(1, 1));

	return mix(mix(a, c, u.y), mix(b, d, u.y), u.x);
}

// unsigned [0, 1] gradient noise
float gradientNoise(vec2 v) {
	// normalize it back to [0, 1]
	return 0.5 + 0.5 * sgradientNoise(v);
}

float voronoiNoise(vec2 v) {
	vec2 cell = floor(v);
	float minDistSqr = 10.f;
	for(int x = -1; x <= 1; ++x) {
		for(int y = -1; y <= 1; ++y) {
			vec2 ocell = cell + vec2(x, y);
			vec2 point = ocell + random2(ocell);
			vec2 dist = v - point;
			float dsqr = dot(dist, dist);
			if(dsqr < minDistSqr) {
				minDistSqr = dsqr;
			}
		}
	}

	return sqrt(minDistSqr);
}

// TODO: simplex noise

#ifndef FBM_OCTAVES
	#define FBM_OCTAVES 8
#endif

#ifndef FBM_NOISE
	#define FBM_NOISE gradientNoise
#endif

float fbm(vec2 st) {
	float sum = 0.f;
	float lacunarity = 2.0;
	float gain = 0.5;

	float amp = 0.5f; // ampliture
	float mod = 1.f; // modulation
	for(int i = 0; i < FBM_OCTAVES; ++i) {
		sum += amp * FBM_NOISE(mod * st);
		mod *= lacunarity;
		amp *= gain;
		st = lacunarity * st;
	}

	return sum;
}
