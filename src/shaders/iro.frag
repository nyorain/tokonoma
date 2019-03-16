#version 450

layout(location = 0) in vec3 inColor;
layout(location = 1) in float inRad;
layout(location = 2) in float inFull;
layout(location = 3) in vec3 inUV;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Type {
	vec3 borderColor;
} pc;

layout(set = 0, binding = 1) uniform sampler2DArray textures;


void main() {
	outColor = vec4(inColor, 1.0);
	if(inRad > 0.95) { // outline
		outColor = vec4(pc.borderColor, 1.0);
	} else if(inRad > inFull) {
		outColor *= 0.5;
	}

	if(inUV.z != -1.0) {
		// TODO: factor shouldn't be here...
		outColor.rgb *= (1.0 - 2.0 * texture(textures, inUV).a);
	}
}

