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

const float bias = 0.1;

void main() {
	// the indices we are given have inverted normals (are designed
	// for a skybox where the inside is the side it's viewed from).
	// therefore, when we are outside, we have to reverse the order.
	uint id = gl_VertexIndex;
	vec3 d = abs(light.pos - scene.viewPos);

	// bias since otherwise we might clip on the near plane
	bool inside = max(d.x, max(d.y, d.z)) <= light.radius + bias;
	if(inside) {
		id = 7 - id;
	}

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
