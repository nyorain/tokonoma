#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out float luminance;
layout(set = 0, binding = 0) uniform sampler2D lightTex;
layout(push_constant) uniform PCR {
	vec3 luminance;
} pcr;

void main() {
	luminance = dot(texture(lightTex, uv).rgb, pcr.luminance);
	// see luminance.comp for reasoning
	luminance = clamp(log2(luminance), -50.f, 50.f);
}
