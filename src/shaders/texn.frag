#version 450

layout(set = 0, binding = 0) uniform sampler2D diffuse;
layout(set = 0, binding = 1) uniform sampler2D normals;
layout(set = 0, binding = 2) uniform UBO {
	vec3 pos;
} light;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_col;

void main() {
	vec3 lightDir = vec3(in_uv, 0) - light.pos;
	vec3 normal = texture(normals, in_uv).rgb;

	normal = normalize(normal * 2 - 1);
	lightDir = normalize(lightDir);

	out_col = texture(diffuse, in_uv) * max(dot(normal, lightDir), 0.0);
}
