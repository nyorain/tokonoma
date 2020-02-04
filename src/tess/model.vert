#version 460

layout(location = 0) out vec3 outNormal;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp;
} scene;

// the vertex values to use
// they form a square to be drawn
const vec2[] values = {
	{-1, -1}, // 4 outlining points ...
	{1, -1},
	{1, 1},

	// {-1, -1},
	// {1, 1},
	{-1, 1},
};

void main() {
	vec2 xz = values[gl_VertexIndex % 4];
	// gl_Position = scene.vp * vec4(xz[0], 0.0, xz[1], 1.0);
	// gl_Position.y = -gl_Position.y;
	gl_Position = vec4(xz[0], 0.0, xz[1], 1.0);
	outNormal = vec3(0, -1, 0);
}
