#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec4 outCol;

const uint pointLight = 1;
const uint dirLight = 2;

struct Light {
	vec3 pd; // position for point light, direction of dir light
	uint type; // point or dir
	vec3 color;
	float _; // padding
};

layout(constant_id = 0) const uint maxLightSize = 8;

layout(set = 0, binding = 0, row_major) uniform Lights {
	mat4 _proj;
	mat4 _model;

	Light lights[maxLightSize];
	vec3 viewPos; // camera position. For specular light
	uint numLights; // <= maxLightSize
} lights;

void main() {
	vec3 normal = normalize(inNormal);
	vec3 objectColor = vec3(1, 1, 1);
	float ambientFac = 0.1f;
	float diffuseFac = 0.5f;
	float specularFac = 0.5f;
	float shininess = 64.f;

	vec3 col = vec3(0.1, 0.1, 0.1);
	for(uint i = 0; i < lights.numLights; ++i) {
		Light light = lights.lights[i];
		vec3 ldir = (light.type == pointLight) ?
			inPos - light.pd :
			light.pd;
		ldir = normalize(ldir);

		float lfac = ambientFac; // ambient
		lfac += diffuseFac * max(dot(normal, -ldir), 0.0); // diffuse

		// TODO: blinn
		// specular; currently just phong
		vec3 refl = reflect(ldir, normal);
		vec3 vdir = normalize(inPos - lights.viewPos);
		lfac += specularFac * pow(max(dot(-vdir, refl), 0.0), shininess);

		// combine
		col += lfac * light.color * objectColor;
	}

	outCol = vec4(col, 1.0);
}
