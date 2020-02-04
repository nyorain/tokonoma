#version 450

layout(vertices = 4) out;

layout(location = 0) in vec3 inNormal[];
layout(location = 0) out vec3 outNormal[4];

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _vp;
	vec3 pos;
} scene;

void main() {
	if(gl_InvocationID == 0) {
		vec4 center = 0.25 * (
			gl_in[0].gl_Position +
			gl_in[1].gl_Position +
			gl_in[2].gl_Position +
			gl_in[3].gl_Position);
		float d = distance(scene.pos, center.xyz);
		float tl = clamp(1 / d, 1.0, 100.f);

		gl_TessLevelOuter[0] = tl;
		gl_TessLevelOuter[1] = tl;
		gl_TessLevelOuter[2] = tl;
		gl_TessLevelOuter[3] = tl;

		gl_TessLevelInner[0] = tl;
		gl_TessLevelInner[1] = tl;
	}

	gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
	outNormal[gl_InvocationID] = inNormal[gl_InvocationID];
}
