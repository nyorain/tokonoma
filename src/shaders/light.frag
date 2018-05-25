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
	float shadowFac = texture(shadowTex, inUV).r;
	outColor = light.color;
	outColor.a *= lightFac;

	// - different segment types (effects can also be combined) -
	// 1. normal opaque segment
	outColor.a *= (1 - shadowFac);

	// 2. glass/filter (could be in any color)
	// outColor.rg *= (1 - shadowFac);

	// 3. amplifier
	// outColor.rgb *= (1 + 5 * shadowFac);

	// 4. color twister
	// float r = outColor.r;
	// outColor.r += 0.2 * shadowFac * outColor.b;
	// outColor.g -= 0.2 * shadowFac * r;
	// outColor.b += 0.2 * shadowFac * r;
	// outColor.a *= 1 - 0.5 * shadowFac;

	// 5. light eater
	// outColor.a *= (1 - 5 * shadowFac);
	
	// 6. specific color eater
	// outColor.rgb *= 1 - shadowFac;
	// outColor.rg -= 5 * shadowFac;

	// - for debugging/testing -
	// outColor.a += 0.01;

	// perform some additional color interpolation creating
	// light effects for light shafts, sunrise-like
	// outColor.r *= pow(1 + shadowFac, 0.5);
	// outColor.b *= pow(1 - shadowFac, 0.5);

	// debug methods
	/* outColor = clamp(outColor, 0.01, 1); */
	// outColor = vec4(1, 1, 1, 1);
}
