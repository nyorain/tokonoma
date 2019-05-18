#version 450

layout(set = 0, binding = 1) uniform samplerCube cubemap;

layout(location = 0) in vec3 uvw;
layout(location = 0) out vec4 outColor;

void main() {
	outColor = texture(cubemap, uvw);
}
