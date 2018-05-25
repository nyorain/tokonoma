#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D lightTex;
layout(set = 0, binding = 1) uniform UBO {
	float exposure;
	float gamma;
} ubo;

// https://learnopengl.com/Advanced-Lighting/HDR
void main() {
	vec3 scene = vec3(1, 1, 1);
	vec3 light = max(texture(lightTex, inUV).rgb, 0.f);

	vec3 hdrColor = scene * light;
	vec3 mapped = vec3(1.0) - exp(-hdrColor * ubo.exposure);
    mapped = pow(mapped, vec3(1.0 / ubo.gamma));

	outColor.rgb = mapped;
	outColor.a = 1.f;

	// outColor.rgb = scene * pow(light, vec3(1)) + 0.1 * clamped +
		// 0.5 * pow(clamped, vec3(2)) +
		// 0.5 * pow(clamped, vec3(4));

	// outColor.rgb = (0.5 * scene + 0.5 * light) * light;
	// outColor.rgb = scene * light;
}
