#version 460

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 fragColor;

void main() {
	// float dist = inColor.a;
	// float falloff = 1 / (dist * dist);
	// NOTE: Light falloff in a perfect 2D world is just inverse linear,
	// not squared distance falloff. See node 1042
	// NOTE: no, nvm, we don't have to model light falloff at all. It
	// comes naturally with the light rays getting less dense far
	// away from the source. That's how falloff works.
	// float falloff = 1 / dist;
	fragColor = vec4(inColor.rgb, 1);
}
