#version 460

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;

layout(location = 0) out vec2 outTexCoord0;
layout(location = 1) out vec2 outTexCoord1;
layout(location = 2) flat out uint outMatID;

layout(set = 0, binding = 0, row_major) uniform LightBuf {
	DirLight light;
};

layout(set = 1, binding = 0, std430) buffer ModelIDs {
	uint modelIDs[];
};

layout(set = 1, binding = 1, row_major) buffer Models {
	ModelData models[];
};

// MULTIVIEW defined from buildsystem. Both shader versions are generated.
// We decide at runtime which one to use based on whether the device
// supports the multiview feature.
#ifdef MULTIVIEW
	#extension GL_EXT_multiview : enable
	uint cascadeIndex = gl_ViewIndex;
#else // MULTIVIEW
	layout(push_constant) uniform PCR {
		uint cascadeIndex;
	};
#endif // MULTIVIEW


void main() {
	outTexCoord0 = inTexCoord0;
	outTexCoord1 = inTexCoord1;

	uint id = modelIDs[gl_DrawID];
	outMatID = uint(models[id].normal[3][0]);

	vec4 wpos = models[id].matrix * vec4(inPos, 1.0);
	gl_Position = light.cascadeProjs[cascadeIndex] * wpos;
	gl_Position.y = -gl_Position.y;

	// TODO: only needed if there is no depth clamping
	// probably best enable/disable this via specialization constant,
	// so the driver can even optimize it out if not needed
	// gl_Position.z = max(gl_Position.z, 0.0); // clamp to near plane
}

