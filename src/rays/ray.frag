#version 460

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 fragColor;

void main() {
	float dist = inColor.a;
	// float falloff = 1 / (dist * dist);
	// Light falloff in a perfect 2D world is just inverse linear,
	// not squared distance falloff. See noden 1042
	float falloff = 1 / dist;
	fragColor = vec4(falloff * inColor.rgb, 1);
}
