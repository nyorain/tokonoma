#version 460

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;
layout(location = 2) in float inHeight;

layout(location = 0) out vec4 fragColor;

vec3 lightDir = vec3(0, -1, 0);

void main() {
	fragColor = vec4(0.0);
	fragColor.a = 1.0;

	// fragColor = vec4(0.1 + 0.5 * vec3(dot(normalize(inNormal), -lightDir)), 1.0);
	// fragColor = vec4(0.5 + 0.5 * inNormal, 1.0);
	// fragColor.rgb += smoothstep(0.3, 0.58, inHeight);
	fragColor.rgb += 1 - 250 * exp(-15 * inHeight);
	// fragColor.rgb = mix(fragColor.rgb, inColor, 0.05);
}
