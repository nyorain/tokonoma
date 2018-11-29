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

vec2 hexPoint(vec2 off, uvec2 hexPos, uint vert, float radius) {
	vec2 center = off;
	center.x += 2 * cospi6 * radius * hexPos.x - 1;
	center.y += 1.5 * radius * hexPos.y - 1;

	if(hexPos.y % 2 == 1) {
		center.x += cospi6 * radius;
	}

	vec2 offset = values[gl_VertexIndex % 6];
	return center + radius * offset;
}

vec2 instanceHexPoint(vec2 off, uint perRow, float radius) {
	uint cx = gl_InstanceIndex % perRow;
	uint cy = gl_InstanceIndex / perRow;
	return hexPoint(off, uvec2(cx, cy), gl_VertexIndex, radius);
}
