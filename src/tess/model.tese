#version 450

layout(quads, equal_spacing, ccw) in;

layout(location = 0) in vec3 inNormal[];
layout(location = 0) out vec3 outNormal;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp;
	vec3 pos;
} scene;

void main() {
	// triangle: barycentric interpolation
	// gl_Position = 
	// 	gl_TessCoord[0] * gl_in[0].gl_Position +
	// 	gl_TessCoord[1] * gl_in[1].gl_Position +
	// 	gl_TessCoord[2] * gl_in[2].gl_Position;

	// quad: mixing
	gl_Position = scene.vp * mix(
		mix(gl_in[0].gl_Position, gl_in[1].gl_Position, gl_TessCoord.x),
		mix(gl_in[3].gl_Position, gl_in[2].gl_Position, gl_TessCoord.x),
		gl_TessCoord.y);
	gl_Position.y = -gl_Position.y;

	outNormal = inNormal[0];
}

