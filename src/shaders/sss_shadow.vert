#version 450
#extension GL_GOOGLE_include_directive : enable

layout(location = 0) in vec2 inPointA;
layout(location = 1) in vec2 inPointB;

layout(set = 0, binding = 0) uniform Light {
	vec4 _color;
	vec2 pos;
	float _r;
	float _strength;
	float bounds;
} light;

void main() {
	// NOTE: approximate points in infinity
	// we do this to rely on the pipelines clipping instead of dealing
	// with that complexity ourselves. Shouldn't hurt performance.
	const float proj = 100.0;
	vec2 p = vec2(-10, -10);
	switch(gl_VertexIndex % 4) {
	case 0:
		p = inPointA;
		break;
	case 1:
		p = inPointB;
		break;
	case 2:
		// project inPointA from light to infinity
		p = inPointA + proj * (inPointA - light.pos);
		break;
	case 3:
		// project inPointB from light to infinity
		p = inPointB + proj * (inPointB - light.pos);
		break;
	default:
		p = vec2(-5, -5); // error
		break;
	}

	float scale = 1 / light.bounds;
	gl_Position = vec4(scale * (p - light.pos), 0.0, 1.0);
}
