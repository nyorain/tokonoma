#version 460

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;

layout(location = 0) out vec3 outPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outTexCoord0;
layout(location = 3) out vec2 outTexCoord1;
layout(location = 4) out float outLinDepth;

layout(location = 5) flat out uint outMatID;
layout(location = 6) flat out uint outModelID;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj; // view and pojection
	mat4 _invProj;
	vec3 viewPos;
	float near, far;
} scene;

layout(set = 1, binding = 0, std430) buffer ModelIDs {
	uint modelIDs[];
};

layout(set = 1, binding = 1, row_major) buffer Models {
	ModelData models[];
};

// TODO: don't require multidraw support, implement fallback
// #if MULTIDRAW
// 	const uint modelIndex = gl_DrawID;
// #else
// 	layout(push_constant) uniform DrawID {
// 		uint modelIndex;	
// 	};
// #endif

void main() {
	uint id = modelIDs[gl_DrawID];
	// remember that matrix addressing is column major in glsl
	outMatID = uint(models[id].normal[0][3]);
	outModelID = uint(models[id].normal[1][3]);

	outNormal = mat3(models[id].normal) * inNormal;
	outTexCoord0 = inTexCoord0;
	outTexCoord1 = inTexCoord1;

	vec4 m = models[id].matrix * vec4(inPos, 1.0);
	outPos = m.xyz / m.w;

	gl_Position = scene.proj * m;

	// TODO: doesn't work like that. More performant to do it here though
	// noperspective only makes it worse
	// outLinDepth = depthtoz(gl_Position.z / gl_Position.w, scene.near, scene.far);
}
