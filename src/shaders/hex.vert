#version 450

layout(location = 0) out vec4 out_color;

layout(row_major, binding = 0) uniform Transform {
	mat4 transform;
	uvec2 size;
	vec2 off;
	float radius;
};
layout(binding = 1) uniform sampler2D img;

const float cospi6 = 0.86602540378; // cos(pi/6) or sqrt(3)/2

// the vertex values to use
// they form a hexagon to be drawn
// our hexagons have sharp top/bottom and flat left/right
// every side has length radius
// total height: 2 * radius (height per row due to shift only 1.5 * radius)
// total width: cospi6 * 2 * radius
const vec2[] values = {
	{cospi6, .5f},
	{0.f, 1.f},
	{-cospi6, .5f},
	{-cospi6, -.5f},
	{0.f, -1.f},
	{cospi6, -.5f},
};

void main() {
	uint cx = gl_InstanceIndex % size.x;
	uint cy = gl_InstanceIndex / size.x;

	vec2 center = off;
	center.x += 2 * cospi6 * radius * cx - 1;
	center.y += 1.5 * radius * cy - 1;

	if(cy % 2 == 1) {
		center.x += cospi6 * radius;
	}

	vec2 offset = values[gl_VertexIndex % 6];

	vec2 color = texture(img, (vec2(cx, cy) + 0.5) / size).xy;
	out_color = vec4(color, 0, 1);
	gl_Position = transform * vec4(center + radius * offset, 0, 1);
}

