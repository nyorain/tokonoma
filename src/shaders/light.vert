#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout(set = 0, binding = 0, row_major) uniform UBO {
	vec2 scale;
	vec2 translate;
} ubo;

layout(location = 0) in vec2 inPos;
layout(location = 0) out vec2 normPos;

void main()
{
	gl_Position.xy = ubo.scale * (inPos + ubo.translate);
	gl_Position.z = 0.f;
	gl_Position.w = 1.f;
	gl_Position.y = -gl_Position.y; // invert y coord for screen space
	normPos = inPos;
}
