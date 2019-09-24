#version 450

layout(location = 0) out vec3 cubePos;

layout(set = 0, binding = 0, row_major) uniform Camera {
	mat4 VP;
	vec3 pos;
} camera;

void main() {
	// Buffer-free cube generation, using triangle strip
	// see e.g. "Eric Lengyel, Game Engine Gems 3" or this discussion:
	// https://twitter.com/Donzanoid/status/616370134278606848
	int mask = (1 << gl_VertexIndex);
	float x = 2 * float((0x287a & mask) != 0) - 1;
	float y = 2 * float((0x02af & mask) != 0) - 1;
	float z = 2 * float((0x31e3 & mask) != 0) - 1;
	cubePos = vec3(x, y, z);

	// camera.VP contains the camera position as translation, which
	// we don't need here so offset position by the camera pos.
	// Can also be thought of as if the skybox is fixed around the
	// camera and moving with it.
	gl_Position = camera.VP * vec4(camera.pos + cubePos, 1.0);
	gl_Position.y = -gl_Position.y;

	// Requires pipeline to have lessOrEqual as depth test.
	// Will always project vertices on the far plane (z = 1), behind
	// all other geometry, allowing to render the skybox after
	// the scene.
	gl_Position = gl_Position.xyww;
}
