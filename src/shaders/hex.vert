#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout(location = 0) in vec2 color;
layout(location = 0) out vec4 col;
layout(location = 1) out vec2 uv;

layout(row_major, set = 0, binding = 0) uniform UBO {
	mat4 transform;
	uint perRow;
} ubo;

const float cospi6 = 0.86602540378; // cos(pi/6) or sqrt(3)/2
const float radius = 1.f; // from center to top corner

// the vertex values to use
// they form a hexagon to be drawn
// our hexagons have sharp top/bottom and flat left/right
// every side has length 1
// total height: 2
// total width: cospi6 * 2
const vec2[] values = {
	{cospi6, .5f},
	{0.f, 1.f},
	{-cospi6, .5f},
	{-cospi6, -.5f},
	{0.f, -1.f},
	{cospi6, -.5f},
};

void main() {
	uint cx = gl_InstanceIndex % ubo.perRow;
	uint cy = gl_InstanceIndex / ubo.perRow;

	// the first hexagon has its center at (0,0)
	vec2 center;
	center.x = 2 * cospi6 * radius * cx;
	center.y = 1.5 * radius * cy;

	if(cy % 2 == 1) {
		center.x += cospi6 * radius;
	}

	vec2 offset = values[gl_VertexIndex % 6];

	uv = offset;
	// col = vec4(color, 0, 1);
	// col = vec4(0.2, 0.2, 0.2, 1);
	col = vec4(0, 0, 0.5, 1) + color.y * vec4(1, 1, 1, 1);
	gl_Position = ubo.transform * vec4(center + radius * offset, 0.0, 1.0);
}
