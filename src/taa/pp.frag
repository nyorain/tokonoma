#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D inTex;

const float exposure = 1.f;

void main() {
	// simple tonemap
	vec3 color = texture(inTex, uv).rgb;
	fragColor = vec4(1.0 - exp(-exposure * color), 1.0);
}
