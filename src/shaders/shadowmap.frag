#version 450

const uint doubleSided = (1u << 1);

layout(location = 0) in vec2 inUV;

// material
layout(set = 1, binding = 0) uniform sampler2D albedoTex;

// factors
layout(push_constant) uniform materialPC {
	vec4 albedo;
	float roughness;
	float metallic;
	uint flags;
	float alphaCutoff;
} material;

void main() {
	vec4 albedo = material.albedo * texture(albedoTex, inUV);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	// NOTE: logic here has to be that weird (two independent if statements)
	// since otherwise i seem to trigger a bug (probably in driver or llvm?)
	// on mesas vulkan-radeon 19.0.2
	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}
}
