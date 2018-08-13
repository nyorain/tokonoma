#version 450

layout(location = 0) out vec4 out_color;

layout(row_major, binding = 0) uniform Transform {
	mat4 transform;
	uvec2 size;
};
layout(binding = 1) uniform sampler2D img;

const float cospi6 = 0.86602540378; // cos(pi/6) or sqrt(3)/2

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
	float radius = 1.f / size.x;

	uint cx = gl_InstanceIndex % size.x;
	uint cy = gl_InstanceIndex / size.x;

	vec2 center;
	center.x = 2 * cospi6 * radius * cx;
	center.y = 1.5 * radius * cy;

	if(cy % 2 == 1) {
		center.x += cospi6 * radius;
	}

	vec2 offset = values[gl_VertexIndex % 6];

	vec2 color = texture(img, (vec2(cx, cy) + 0.5) / size).xy;
	out_color = vec4(color, 0, 1);
	gl_Position = transform * vec4(center + radius * offset, 0, 1);
}

