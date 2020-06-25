#version 450

layout(location = 0) in vec3 inCoords;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 transform;
#ifdef LAYERED
	float layer;
#endif // LAYERED
#ifdef TONEMAP
	float exposure;
#endif // TONEMAP
} ubo;

// we sample from lod 0, the skybox might have multiple mip levels, e.g.
// specular ibl convolutions. Could use a specialization/push constant
// for that though.

#ifdef LAYERED
	layout(set = 1, binding = 0) uniform samplerCubeArray cubemap;
	vec3 sampleCube() {
		// custom linear layer interpolation
		vec3 l0 = textureLod(cubemap, vec4(inCoords, floor(ubo.layer)), 0).rgb;
		vec3 l1 = textureLod(cubemap, vec4(inCoords, ceil(ubo.layer)), 0).rgb;
		return mix(l0, l1, fract(ubo.layer));
	}

#else // LAYERED
	layout(set = 1, binding = 0) uniform samplerCube cubemap;
	vec3 sampleCube() {
		// return textureLod(cubemap, inCoords, 0).rgb;
		return texture(cubemap, inCoords).rgb;
	}
#endif // LAYERED

void main() {
	outColor = vec4(sampleCube(), 1.0);

	// for debugging positions
	// // inCoords = normalize(inCoords); // smooth
	// outColor = vec4(0.5 + 0.5 * inCoords, 1.0);
	
#ifdef TONEMAP
	outColor.rgb *= ubo.exposure;
	outColor.rgb = 1.0 - exp(-outColor.rgb);
#endif // TONEMAP
}
