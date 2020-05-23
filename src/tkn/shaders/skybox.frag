#version 450

layout(location = 0) in vec3 uvw;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform samplerCube cubemap;

void main() {
	// use lod 0, the skybox might have multiple mip levels, e.g.
	// specular ibl convolutions
	outColor = textureLod(cubemap, uvw, 0);
	outColor.a = 1.0;

	// for debugging positions
	// // uvw = normalize(uvw); // smooth
	// outColor = vec4(0.5 + 0.5 * uvw, 1.0);

	// TODO: make optional via specialization constant?
	// simple tonemap, obviously only needed if the skybox is rendered
	// after tonemap step (shouldn't be case)
	float exposure = 1.0;
	outColor = 1.0 - exp(-outColor * exposure);
}
