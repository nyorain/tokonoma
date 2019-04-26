#version 450

layout(set = 0, binding = 0) uniform sampler2D diffuse;
layout(set = 0, binding = 1) uniform sampler2D normals;
layout(set = 0, binding = 2) uniform UBO {
	vec3 pos;
	float time;
} light;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_col;

void main() {
	vec3 lpos = light.pos;
	// randomness,flickering should probably be multiplicative.
	// too large influence for light close to 0 and too small influence for
	// light far away
	lpos.z += 0.02 * cos(0.1 * light.time) * sin((0.2 + 0.5 * abs(cos(light.time))) * light.time);
	vec3 lightDir = vec3(in_uv, 0) - lpos;
	vec3 normal = texture(normals, in_uv).rgb;

	normal = normalize(normal * 2 - 1);
	lightDir = normalize(lightDir);

	out_col = texture(diffuse, in_uv) * max(dot(normal, lightDir), 0.0);
}
