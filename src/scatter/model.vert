#version 460

layout(location = 0) in vec3 inPos;

layout(location = 0) out vec3 outPos; // world space
layout(location = 1) out vec4 outPosClip; // clip space
layout(location = 2) out vec4 outPosClipPrev; // clip space, prev frame

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp; // view and pojection
	mat4 vpPrev;
	mat4 _vpInv;
	vec3 pos;
	float _;
	vec2 jitter;
} scene;

void main() {
	outPos = inPos;

	outPosClip = scene.vp * vec4(inPos, 1.0);
	outPosClipPrev = scene.vpPrev * vec4(inPos, 1.0);

	gl_Position = outPosClip;
	gl_Position.xy += scene.jitter * gl_Position.w; // jitter in ndc space
	gl_Position.y = -gl_Position.y;
}
