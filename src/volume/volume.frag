#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0, row_major) uniform Camera {
	mat4 _proj;
	vec3 viewPos;
} camera;

const vec3 lightDir = normalize(vec3(0.1, -1.0, 0.2));
const vec3 lightCol = vec3(1.0);
const vec3 objectCol = vec3(0.9, 0.6, 0.5);
const float kd = 0.9;
const float ks = 0.05;
const float shine = 8.f;
const float ambient = 0.05;

void main() {
	vec3 N = normalize(inNormal);
	vec3 L = -lightDir;
	float diffuse = clamp(kd * dot(L, N), 0, 1);
	
	vec3 V = camera.viewPos - inPos;
	vec3 H = normalize(L + V);
	float spec = ks * pow(clamp(dot(H, N), 0, 1), shine);

	fragColor = vec4((ambient + diffuse + spec) * lightCol * objectCol, 1.0);
}
