#version 450

layout(location = 0) in vec3 inColor;
layout(location = 1) in float inRad;
layout(location = 2) in float inFull;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Type {
	vec3 borderColor;
} pc;

void main() {
	outColor = vec4(inColor, 1.0);
	if(inRad > 0.95) { // outline
		outColor = vec4(pc.borderColor, 1.0);
	} else if(inRad < inFull) {
		outColor += vec4(inColor, 1.0);
	}
}

