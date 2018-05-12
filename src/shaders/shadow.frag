#version 450
#extension GL_GOOGLE_include_directive : enable

#include "geometry.glsl"

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inPointA;
layout(location = 2) in vec2 inPointB;
layout(location = 3) in float inOpacity;
layout(location = 4) in vec2 inValue;
layout(location = 0) out float outColor;

layout(set = 1, binding = 0) uniform Light {
	vec4 _color; // unused
	vec2 position;
	float radius;
} light;

Circle lightCircle() {
	return Circle(light.position, light.radius);
}

void main() {
	if(inValue.x < 0) {
		outColor = shadowValue(lightCircle(), inPos, inPointA, inPointB);
	} else {
		outColor = interpolate(inValue);
	}

	outColor *= inOpacity;
}
