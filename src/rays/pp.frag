#version 460

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D history;

const float exposure = 1.0;

void main() {
	vec3 col = texture(history, uv).rgb;
	col = 1.0 - exp(-exposure * col); // reinhard tonemap
	fragColor = vec4(col, 1.0);
}
