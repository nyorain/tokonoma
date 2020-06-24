#version 460

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 3) uniform samplerCube heightmap;

const float pi = 3.1415;

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

void main() {
	vec3 pos = normalize(inPos);
	vec4 h = texture(heightmap, pos);

	vec2 tp = sph2(pos);
	float theta = tp[0];
	float phi = tp[1];

	vec3 dphi = sph_dphi(1.f, theta, phi);
	vec3 dtheta = sph_dtheta(1.f, theta, phi);
	float fac = 0.1;
	vec3 n = cross(
		(1 + fac * h.x) * dtheta + dot(dtheta, fac * h.yzw) * pos, 
		(1 + fac * h.x) * dphi + dot(dphi, fac * h.yzw) * pos);
	n = normalize(n);

	const vec3 toLight = vec3(0, 1, 0);
	float l = max(dot(n, toLight), 0.0);
	fragColor = vec4(vec3(l), 1);
}
