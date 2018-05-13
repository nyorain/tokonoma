#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 transform;
} ubo;

layout(set = 1, binding = 0) uniform UBO2 {
	vec4 _color;
	vec2 position;
} ubo2;

layout(location = 0) out vec2 normPos;
layout(location = 1) out vec2 uv;

const vec2[] values = {
	{-1, -1}, // 4 outlining points ...
	{1, -1},
	{1, 1},
	{-1, 1},
};

const float scale = 3.0;

void main()
{
	uv = 0.5 + 0.5 * values[gl_VertexIndex];
	vec2 pos = ubo2.position + scale * values[gl_VertexIndex];
	gl_Position = ubo.transform * vec4(pos, 0.0, 1.0);
	normPos = pos;
}
