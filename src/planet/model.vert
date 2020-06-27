#version 460

#extension GL_GOOGLE_include_directive : enable
#include "subd.glsl"
#include "terrain.glsl"

layout(location = 0) in uvec2 inKey;
layout(location = 0) out vec3 outPos;
layout(location = 1) out vec3 outVPos;
layout(location = 2) out vec3 outNormal;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp;
	vec3 pos;
	uint _0;
	uvec3 centerTile;
} scene;

// NOTE: we render a simple cube as planet. We could replace this
// with manual calculation inside the shader.
layout(set = 0, binding = 1) buffer Vertices { vec4 vertices[]; };
layout(set = 0, binding = 2) buffer Indices { uint indices[]; };
layout(set = 0, binding = 3) uniform sampler2DArray heightmap;

// Transform of coords on unit sphere to spherical coordinates (theta, phi)
vec2 sph2(vec3 pos) {
	// TODO: undefined output in that case...
	if(abs(pos.y) > 0.999) {
		return vec2(0, (pos.y > 0) ? 0.0001 : 3.141);
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

vec3 displace(vec3 pos) {
	pos = normalize(pos);
	vec4 h4 = height(pos, scene.centerTile, heightmap);
	vec3 p = 6360 * (1 + displaceStrength * h4.r) * pos;

	vec2 tp = sph2(pos);
	float theta = tp[0];
	float phi = tp[1];

	vec3 dphi = sph_dphi(1.f, theta, phi);
	vec3 dtheta = sph_dtheta(1.f, theta, phi);
	float fac = displaceStrength;
	vec3 n = cross(
		(1 + fac * h4.x) * dtheta + dot(dtheta, fac * h4.yzw) * pos, 
		(1 + fac * h4.x) * dphi + dot(dphi, fac * h4.yzw) * pos);
	outNormal = normalize(n);

	return p;
}

void main() {
	uint idx = inKey.y;
	vec3 v_in[3] = vec3[3](
		vertices[indices[3 * idx + 0]].xyz,
		vertices[indices[3 * idx + 1]].xyz,
		vertices[indices[3 * idx + 2]].xyz
	);

	uint vid = gl_VertexIndex % 3;
	vec3 v_out[3];
	subd(inKey.x, v_in, v_out);
	vec2 bary = subd_bvecs[vid].xy;
	vec3 pos = berp(v_out, bary);
	pos = displace(pos);

	// https://www.enkisoftware.com/devlogpost-20150131-1-Normal-generation-in-the-pixel-shader
	outVPos = pos - scene.pos;

	outPos = pos;
	gl_Position = scene.vp * vec4(pos, 1.0);
}
