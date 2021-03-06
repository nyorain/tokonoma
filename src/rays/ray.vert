#version 460

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec2 outPos;
layout(location = 1) out vec4 outColor;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 transform;
	vec2 jitter;
} scene;

void main() {
	gl_Position = scene.transform * vec4(inPos, 0, 1);
	// gl_Position.xy += scene.jitter * gl_Position.w;
	outPos = gl_Position.xy;
	outColor = inColor;
}
