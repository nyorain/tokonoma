#version 450

layout(location = 0) out vec4 fragColor;

layout(constant_id = 0) const float r = 1.0;
layout(constant_id = 1) const float g = 1.0;
layout(constant_id = 2) const float b = 1.0;
layout(constant_id = 3) const float a = 1.0;

void main() {
	// constant color
	fragColor = vec4(r, g, b, a);
}
