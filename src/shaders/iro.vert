#version 450
#extension GL_GOOGLE_include_directive : enable

const float cospi6 = 0.86602540378; // cos(pi/6) or sqrt(3)/2

// the x coords have to be multiplied with cospi6
// total height: 2 * radius (height per row due to shift only 1.5 * radius)
// total width: cospi6 * 2 * radius
// we start in the center so we can pass the "outRad" (normalized distance
// to center) to the fragment shader.
const vec3[] hexPoints = {
	{0.f, 0.f, 0.f},
	{1.f, .5f, 1.f},
	{0.f, 1.f, 1.f},
	{-1.f, .5f, 1.f},
	{-1.f, -.5f, 1.f},
	{0.f, -1.f, 1.f},
	{1.f, -.5f, 1.f},
	{1.f, .5f, 1.f},
};

layout(location = 0) in vec2 inPos;
layout(location = 1) in uint inPlayer;
layout(location = 2) in float inStrength;
layout(location = 3) in uint inType;
layout(location = 4) in vec2 inVel;

layout(location = 0) out vec3 outColor;
layout(location = 1) out float outRad;
layout(location = 2) out float outFull;
layout(location = 3) out vec3 outUV;

layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 transform;
} ubo;

const vec3 colors[] = {
	vec3(1.0, 1.0, 0.0),
	vec3(0.0, 1.0, 1.0),
};

void main() {
	float s = clamp((inType == 0) ? inStrength / 2.0 : inStrength / 10.f, 0, 1.0);
	vec3 hp = hexPoints[gl_VertexIndex % 8];

	// rotate for velocity
	vec2 uv = hp.xy;
	if(inType == 4 || inType == 0) {
	// if(inType == 4) {
		// angle between x axis and velocity
		float angle = -atan(inVel.y, inVel.x);
		float c = cos(angle);
		float s = sin(angle);

		// rotate by angle
		vec2 ruv = uv;
		uv.x = c * cospi6 * ruv.x - s * ruv.y;
		uv.y = s * cospi6 * ruv.x + c * ruv.y;
		uv.x /= cospi6;
	}
	// outUV = vec3(0.5 + 0.5 * uv.xy, inType == 0 ? -1.f : inType - 1);
	outUV = vec3(0.5 + 0.5 * uv.xy, inType == 0 ? 3.f : inType - 1);

	hp.x *= cospi6;
	gl_Position = ubo.transform * vec4(inPos + hp.xy, 0.0, 1.0);
	outColor = s * mix(colors[0], colors[1], float(inPlayer));
	outRad = hp.z;

	// NOTE: all buildings have max 10 hp atm
	outFull = (inType == 0) ? 1.f : s;
}
