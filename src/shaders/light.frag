#version 450
#extension GL_GOOGLE_include_directive : enable

#include "geometry.glsl"

layout(set = 1, binding = 0) uniform UBO {
	vec4 color;
	vec2 position;
	float radius;
	float strength;
} light;

layout(set = 1, binding = 1) uniform sampler2D shadowTex;

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
	float lightFac = lightFalloff(light.position, inPos, light.radius,
		light.strength);
	float shadowFac = (1 - texture(shadowTex, inUV).r);
	outColor = light.color;
	outColor.a *= lightFac * shadowFac;

	// perform some additional color interpolation creating
	// light effects for light shafts, sunrise-like
	// outColor.g *= 0.5 + 0.5 * lightFac;
	// outColor.r *= 0.8 + 0.2 * lightFac;
	// outColor.b *= (1 - lightFac);

	// debug methods
	/* outColor = clamp(outColor, 0.01, 1); */
	// outColor = vec4(1, 1, 1, 1);
}
