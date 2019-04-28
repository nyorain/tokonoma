#version 450

#extension GL_GOOGLE_include_directive : enable

#include "scene.glsl"
#include "pbr.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out float scatter;

// scene
layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj;
	mat4 invProj;
	vec3 viewPos;
} scene;

layout(set = 1, binding = 0) uniform sampler2D depthTex;
layout(set = 2, binding = 0, row_major) uniform LightBuf {
	PointLight light;
};

void main() {
	vec2 suv = 2 * uv - 1;
	suv.y *= -1.f; // flip y
	float depth = texture(depthTex, uv).r;
	vec4 pos4 = scene.invProj * vec4(suv, depth, 1.0);
	vec3 pos = pos4.xyz / pos4.w;

	vec3 v = normalize(pos - scene.viewPos); // view direction
	vec3 viewToLight = normalize(light.pos - scene.viewPos);
	float ldv = dot(viewToLight, v);
	if(ldv < 0) {
		scatter = 0.f;
		return;
	}

	// TODO: mapping not fragment shader dependent but relative
	// expensive, could be done in vertex shader (or cpu but
	// has to be refreshed every time viewPos changes...)
	// mapped position of a directional light
	vec3 mappedLightPos = sceneMap(scene.proj, light.pos);
	if(mappedLightPos.z < 0) {
		// in this case the light is behind the camera, there
		// will be no depth scattering.
		scatter = 0.f;
		return;
	}

	scatter = lightScatterDepth(uv, mappedLightPos.xy,
		mappedLightPos.z, ldv, depthTex);

	// attentuation volume
	// TODO: document how the projection here works
	vec4 projpl = scene.invProj * vec4(suv, mappedLightPos.z, 1.0);
	projpl.xyz /= projpl.w;
	float dist = distance(light.pos, projpl.xyz);
	scatter *= attenuation(dist, light.attenuation);
}
