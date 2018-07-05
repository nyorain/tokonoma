#version 450
#extension GL_GOOGLE_include_directive : enable

#include "geometry.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D lightTex;
layout(row_major, set = 0, binding = 1) uniform UBO {
	mat4 windowToLevel;
	float exposure;
	float gamma;
	float viewFac;
} ubo;

layout(set = 1, binding = 0) uniform ViewLight {
	vec4 color;
	vec2 position;
	float radius;
	float strength;
	float bounds;
} light;

layout(set = 1, binding = 1) uniform sampler2D shadowTex;

// using ideas/tutorial from
// https://learnopengl.com/Advanced-Lighting/HDR
void main() {
	vec3 scene = vec3(1, 1, 1);
	vec3 lightSum = max(texture(lightTex, inUV).rgb, 0.f);

	vec3 hdrColor = scene * lightSum;
	vec3 mapped = vec3(1.0) - exp(-hdrColor * ubo.exposure);
    mapped = pow(mapped, vec3(1.0 / ubo.gamma));

	outColor.rgb = mapped;
	outColor.a = 1.f;

	// level position
	vec2 pos = (ubo.windowToLevel * vec4(gl_FragCoord.xy, 0, 1)).xy;
	vec2 uv = (1 / light.bounds) * (pos - light.position);
	uv = 0.5 * (uv + vec2(1, 1));

	float lightFac = (1 - texture(shadowTex, uv).r);
	// lightFac *= (1 - (length(light.position - pos) / light.bounds));

	const vec3 p = ubo.viewFac * vec3(1, 0.5 / light.radius, 0.1 / (light.radius * light.radius));
	lightFac *= clamp(lightFalloff(light.position, pos, light.radius,
		0.8, p, 0, false), 0, 1);
	outColor.rgb *= lightFac;

	/*
	outColor.rgb *= pow(lightFac * (1 - shadowFac), ubo.viewFac);
	*/

	// outColor.rgb = scene * pow(light, vec3(1)) + 0.1 * clamped +
		// 0.5 * pow(clamped, vec3(2)) +
		// 0.5 * pow(clamped, vec3(4));

	// outColor.rgb = (0.5 * scene + 0.5 * light) * light;
	// outColor.rgb = scene * light;
}
