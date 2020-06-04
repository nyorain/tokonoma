#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 0, binding = 1) uniform UBO {
	float exposure;
} ubo;

void main() {
	vec4 color = texture(tex, inUV);
	color.rgb = 1.0 - exp(-ubo.exposure * color.rgb);
	outFragColor = color;
}


