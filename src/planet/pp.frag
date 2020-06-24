#version 460

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D colorTex;

void main() {
	const float exposure = 1.f;

	vec4 col = texture(colorTex, inUV);
	col.rgb = 1 - exp(-exposure * col.rgb);
	fragColor = col;
}
