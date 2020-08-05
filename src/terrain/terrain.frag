#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) noperspective in vec3 inBary;

layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 3) uniform sampler2D heightmap;

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

const vec3 toLight = normalize(vec3(0.5, 0.3, 0.8));

float shadow() {
	float dt;
	float shadow = 1.f;
	for(float t = 0.05; t < 1.f && shadow > 0.f; t += dt) {
		vec3 pos = inPos + t * toLight;
		float height = texture(heightmap, 0.5 + 0.5 * pos.xz).r;
		if(pos.xz == clamp(pos.xz, -1.f, 1.f) && height > pos.y) {
			shadow *= exp(-10 * (height - pos.y));
		}

		dt = 0.05 * t;
	}

	return shadow;
}

void main() {
	vec2 baseCoord = 0.5 + 0.5 * inPos.xz;

	float x0 = textureOffset(heightmap, baseCoord, ivec2(-1, 0)).r;
	float x1 = textureOffset(heightmap, baseCoord, ivec2(1, 0)).r;
	float z0 = textureOffset(heightmap, baseCoord, ivec2(0, -1)).r;
	float z1 = textureOffset(heightmap, baseCoord, ivec2(0, 1)).r;

	vec2 pixLength = 1.0 / textureSize(heightmap, 0);
	float dx = 0.5 * (x1 - x0) / pixLength.x;
	float dz = 0.5 * (z1 - z0) / pixLength.y;

	vec3 n = normalize(vec3(0, 1, 0) - dx * vec3(1, 0, 0) - dz * vec3(0, 0, 1));

	// sun lighting
	const vec3 lightColor = vec3(0.88, 0.85, 0.8);
	vec3 light = max(dot(toLight, n), 0.0) * lightColor * (0.05 + shadow());

	// ao
	float ao = 1.0;
	float height = texture(heightmap, baseCoord).r;
	for(int y = -1; y <= 1; ++y) {
		for(int x = -1; x <= 1; ++x) {
			float s = texture(heightmap, baseCoord + 30 * vec2(x, y) * pixLength).r;
			ao *= min(1.0, exp(-10 * (s - height)));
		}
	}

	const vec3 aoColor = vec3(0.45, 0.52, 0.55);
	light += 0.25 * ao * aoColor * n.y;

	// tonemap
	float exposure = 1.0;
	light = 1 - exp(-exposure * light);
	outFragColor = vec4(light, 1.0);

	// outFragColor.rgb *= outline();
}
