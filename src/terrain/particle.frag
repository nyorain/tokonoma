#version 450

layout(location = 0) in vec3 inColor;
layout(location = 0) out vec4 outFragColor;

void main() {
	float alpha = pow(max(1 - length(2 * (0.5 - gl_PointCoord)), 0.0), 2);
	outFragColor = vec4(inColor, alpha);
}
