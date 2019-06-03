#version 450

// layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, input_attachment_index = 0)
	uniform subpassInput inReflectance;
layout(set = 0, binding = 1, input_attachment_index = 0)
	uniform subpassInput inRevealage;

void main() {
	vec4 refl = subpassLoad(inReflectance);
	float reveal = subpassLoad(inRevealage).r;
	outColor = vec4(refl.rgb / max(refl.a, 1e-5), reveal);
}
