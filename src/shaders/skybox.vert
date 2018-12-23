#version 450

layout(location = 0) out uvw;
layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 transform;
} ubo;

void main() {
	vec3 pos = vec3(
		-1 + 2 * ((gl_VertexIndex << 0) & 1),
		-1 + 2 * ((gl_VertexIndex << 1) & 1),
		-1 + 2 * ((gl_VertexIndex << 2) & 1));
	uvw = pos;
	gl_Position = ubo.transform * vec4(pos, 1.0);
}
