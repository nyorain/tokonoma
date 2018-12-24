#version 450
#extension GL_GOOGLE_include_directive : enable

const float cospi6 = 0.86602540378; // cos(pi/6) or sqrt(3)/2

// total height: 2 * radius (height per row due to shift only 1.5 * radius)
// total width: cospi6 * 2 * radius
const vec3[] hexPoints = {
	{0.f, 0.f, 0.f},
	{cospi6, .5f, 1.f},
	{0.f, 1.f, 1.f},
	{-cospi6, .5f, 1.f},
	{-cospi6, -.5f, 1.f},
	{0.f, -1.f, 1.f},
	{cospi6, -.5f, 1.f},
	{cospi6, .5f, 1.f},
};

layout(location = 0) in vec2 inPos;
layout(location = 1) in uint inPlayer;
layout(location = 2) in float inStrength;
layout(location = 3) in uint inType;

layout(location = 0) out vec3 outColor;
layout(location = 1) out float outRad;
layout(location = 2) out float outFull;

layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 transform;
} ubo;

const vec3 colors[] = {
	vec3(1.0, 1.0, 0.0),
	vec3(0.0, 1.0, 1.0),
};

void main() {
	float s = (inType == 0) ? inStrength : inStrength / 10.f;
	vec3 hp = hexPoints[gl_VertexIndex % 8];
	gl_Position = ubo.transform * vec4(inPos + hp.xy, 0.0, 1.0);
	outColor = s * mix(colors[0], colors[1], float(inPlayer));
	outRad = hp.z;

	// NOTE: all buildings have 10 hp atm
	outFull = (inType == 0) ? 0.f : s;
}
