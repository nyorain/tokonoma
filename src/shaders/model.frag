#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec4 outCol;

const uint pointLight = 1u;
const uint dirLight = 2u;

struct Light {
	vec3 pd; // position for point light, direction of dir light
	uint type;
	vec3 color;
	float _; // padding
};

layout(constant_id = 0) const uint maxLightSize;

layout(set = 0, binding = 0, row_major) uniform {
	mat4 _proj;
	mat4 _model;

	Light lights[maxLightSize];
	vec3 viewPos; // camera position. For specular light
	uint numLights; // <= maxLightSize
} lights;

void main() {
	vec4 objectColor = vec4(1, 1, 1, 1);
	float ambientFac = 0.1f;
	float diffuseFac = 0.5f;
	float specularFac = 0.5f;
	float shininess = 16.f;

	vec3 col = vec3(0, 0, 0);
	for(uint i = 0; i < lights.numLights; ++i) {
		Light light = lights.lights[i];
		vec2 ldir = (light.type == pointLight) ? 
			light.pd - inPos :
			light.pd;

		float lfac = 0.f;

		// ambient
		lfac += ambientFac;

		// diffuse
		lfac += diffuseFac * max(dot(inNormal, -ldir), 0.0);
		
		// TODO: blinn
		// specular; currently just phong
		vec3 refl = reflect(-ldir, inNormal);
		vec3 vdir = normalize(inPos - light.viewPos);
		lfac += specularFac * pow(max(dot(-vdir, refl), 0.0), shininess);
		
		// combine
		col += lfac * light.color * objectColor;
	}

	outCol = vec4(col, 1.0);
}
