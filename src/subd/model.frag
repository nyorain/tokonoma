#version 460

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;
layout(location = 2) in float inHeight;
layout(location = 3) in vec3 inPos;

layout(location = 0) out vec4 fragColor;

vec3 lightDir = vec3(0, -1, 0);

void main() {
	vec3 dx = dFdx(inPos);
	vec3 dy = dFdy(inPos);
	vec3 n = normalize(-cross(dx, dy));
	n = normalize(inNormal);

	fragColor = vec4(0.0);
	fragColor.a = 1.0;

	float l = max(dot(n, -lightDir), 0.0);
	// fragColor.rgb = 0.1 + 0.9 * vec3(l); // * vec3(0, 1, 1) + 
	// 	0.5 * vec3(max(0, dot(n, vec3(1, 0, 0)))) * vec3(1, 1, 0);
	fragColor.rgb = 0.5 + 0.5 * n;
	// fragColor.rgb += smoothstep(0.3, 0.58, inHeight);
	// fragColor.rgb += 1 - 5 * exp(-10 * inHeight);
	// fragColor.rgb = mix(fragColor.rgb, inColor, 0.1);
}
