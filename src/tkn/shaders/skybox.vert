#version 450

layout(location = 0) out vec3 uvw;
layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 transform;
} ubo;

void main() {
	// Buffer-free cube generation, using triangle strip
	// see e.g. "Eric Lengyel, Game Engine Gems 3" or this discussion:
	// https://twitter.com/Donzanoid/status/616370134278606848
	int mask = (1 << gl_VertexIndex);
	float x = 2 * float((0x287a & mask) != 0) - 1;
	float y = 2 * float((0x02af & mask) != 0) - 1;
	float z = 2 * float((0x31e3 & mask) != 0) - 1;

	vec3 pos = vec3(x, y, z);
	uvw = pos;

	gl_Position = ubo.transform * vec4(pos, 1.0);
	// gl_Position.y = -gl_Position.y;

	// Requires pipeline to have lessOrEqual as depth test
	// will always project vertices on the far plane, behind
	// all other geometry, allowing to render the skybox after
	// the scene
	// gl_Position = gl_Position.xyww;

	gl_Position.z = 0.f;
}
