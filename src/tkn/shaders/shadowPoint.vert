#version 460

// config macros, set by buildsystem:
// - SCENE: whether or not all the input for texture coordinates,
//   model and material ids are defined (as it is the case for a
//   tkn::Scene geometry setup). Otherwise, only the position
//   is expected and will simply be forwarded.
// - MULTIVIEW: whether or not multiview rendering should be used.
//   Otherwise the faceIndex must be passed via push_constant

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec3 outPos; // in global space

layout(set = 0, binding = 0, row_major) uniform Light {
	PointLight light;
};

#ifdef MULTIVIEW
	#extension GL_EXT_multiview : enable
	uint faceIndex = gl_ViewIndex;
#else // MULTIVIEW
	layout(push_constant) uniform PCR {
		uint faceIndex;
	};
#endif // MULTIVIEW

#ifdef SCENE
	layout(location = 1) in vec3 inNormal; // unused
	layout(location = 2) in vec2 inTexCoord0;
	layout(location = 3) in vec2 inTexCoord1;

	layout(location = 1) out vec2 outTexCoord0;
	layout(location = 2) out vec2 outTexCoord1;
	layout(location = 3) flat out uint outMatID;

	layout(set = 1, binding = 0, std430) buffer ModelIDs {
		uint modelIDs[];
	};

	layout(set = 1, binding = 1, row_major) buffer Models {
		ModelData models[];
	};
#endif // SCENE


void main() {
	vec4 pos = vec4(inPos, 1.0);
#ifdef SCENE
	outTexCoord0 = inTexCoord0;
	outTexCoord1 = inTexCoord1;

	uint id = modelIDs[gl_DrawID];
	outMatID = uint(models[id].normal[0][3]);

	pos = models[id].matrix * pos; // global space
#endif // SCENE
	outPos = pos.xyz / pos.w;

	gl_Position = light.proj[faceIndex] * pos;
	gl_Position.y = -gl_Position.y;
}

