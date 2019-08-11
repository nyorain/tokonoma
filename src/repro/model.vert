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
layout(location = 4) flat out uint outMatID;
layout(location = 5) flat out uint outModelID;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj; // view and pojection
} scene;

layout(set = 1, binding = 0, std430) buffer ModelIDs {
	uint modelIDs[];
};

layout(set = 1, binding = 1, row_major) buffer Models {
	ModelData models[];
};

void main() {
	outTexCoord0 = inTexCoord0;
	outTexCoord1 = inTexCoord1;

	uint id = modelIDs[gl_DrawID];
	outMatID = uint(models[id].normal[0][3]);
	outModelID = uint(models[id].normal[1][3]);
	outNormal = mat3(models[id].normal) * inNormal;

	vec4 m = models[id].matrix * vec4(inPos, 1.0);
	outPos = m.xyz / m.w;

	gl_Position = scene.proj * m;
	gl_Position.y = -gl_Position.y;
}

