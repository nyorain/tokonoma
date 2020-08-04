#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) noperspective in vec3 inBary;

layout(location = 0) out vec4 outFragColor;

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

void main() {
	// triangle outlines
	// outFragColor = vec4(vec3(outline()), 1.0);

	// lighting
	const vec3 toLight = normalize(vec3(0.1, 1.0, 0.3));

	vec3 dx = dFdx(inPos);
	vec3 dy = dFdy(inPos);
	vec3 n = normalize(-cross(dx, dy));

	outFragColor = vec4(dot(toLight, n) * vec3(0.8, 0.85, 0.8), 1.0);
}
