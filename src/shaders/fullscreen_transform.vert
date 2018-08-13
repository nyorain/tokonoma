#version 450

layout(location = 0) out vec2 uv;
layout(row_major, binding = 0) uniform Transform {
	mat4 transform;
};

// the vertex values to use
// they form a square to be drawn
const vec2[] values = {
	{-1, -1}, // 4 outlining points ...
	{1, -1},
	{1, 1},
	{-1, 1},
};

void main() {
	vec2 pos = values[gl_VertexIndex % 4];
	uv = 0.5 + 0.5 * pos;
	gl_Position = transform * vec4(pos, 0.0, 1.0);
}

