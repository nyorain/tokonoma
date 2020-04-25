#version 460

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec3 outPos;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp; // view and pojection
	vec3 pos;
} scene;

void main() {
	outPos = scene.pos;
	gl_Position = scene.vp * vec4(inPos, 1.0);
	gl_Position.y = -gl_Position.y;
}
