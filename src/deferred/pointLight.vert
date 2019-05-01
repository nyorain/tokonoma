#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) noperspective out vec2 uv;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj;
	mat4 _invProj;
	vec3 viewPos;
} scene;

layout(set = 2, binding = 0, row_major) uniform Light {
	PointLight light;
};

void main() {
	uint id = gl_VertexIndex;
	/*
	vec3 d = abs(light.pos - scene.viewPos);
	bool inside = max(d.x, max(d.y, d.z)) <= radius;
	if(!inside) { // camera is inside view volume, reverse face order
		// id = 7 - id;
	}
	*/

	vec3 pos = vec3(
		-1.f + 2 * ((id >> 0u) & 1u),
		-1.f + 2 * ((id >> 1u) & 1u),
		-1.f + 2 * ((id >> 2u) & 1u));
	vec4 p = scene.proj * vec4(light.pos + light.radius * pos, 1.0);
	p.y = -p.y;
	gl_Position = p;

	p.xy /= p.w;
	uv = 0.5 + 0.5 * p.xy;
}
