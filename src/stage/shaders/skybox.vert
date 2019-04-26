#version 450

layout(location = 0) out vec3 uvw;
layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 transform;
} ubo;

void main() {
	vec3 pos = vec3(
		-1 + 2 * ((gl_VertexIndex >> 0) & 1),
		-1 + 2 * ((gl_VertexIndex >> 1) & 1),
		-1 + 2 * ((gl_VertexIndex >> 2) & 1));
	uvw = pos;
	gl_Position = ubo.transform * vec4(pos, 1.0);
	gl_Position.y = -gl_Position.y;

	// Requires pipeline to have lessOrEqual as depth test
	// will always project vertices on the far plane, behind
	// all other geometry, allowing to render the skybox *after*
	// the scene (small optimization but e.g. required for a deferred
	// renderer).
	gl_Position = gl_Position.xyww;
}
