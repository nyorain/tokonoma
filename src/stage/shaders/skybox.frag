#version 450

layout(location = 0) in vec3 uvw;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform samplerCube cubemap;

void main() {
	// use lod 0, the skybox might have multiple mip levels, e.g.
	// specular ibl convolutions
	outColor = textureLod(cubemap, uvw, 0);
	outColor.a = 1.0;

	// simple tonemap, obviously only needed if the skybox is rendered
	// after tonemap step (shouldn't be case)
	// float exposure = 1.0;
	// outColor = 1.0 - exp(-outColor * exposure);
}
