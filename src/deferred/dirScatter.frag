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
	float near, far;
} scene;

layout(set = 1, binding = 0) uniform sampler2D depthTex;
layout(set = 2, binding = 0, row_major) uniform LightBuf {
	DirLight light;
};

void main() {
	vec2 suv = 2 * uv - 1;
	suv.y *= -1.f; // flip y
	float ldepth = texture(depthTex, uv).r;
	float depth = ztodepth(ldepth, scene.near, scene.far); // TODO
	vec4 pos4 = scene.invProj * vec4(suv, depth, 1.0);
	vec3 pos = pos4.xyz / pos4.w;

	vec3 v = normalize(pos - scene.viewPos); // view direction
	vec3 viewToLight = -light.dir; // normalized on cpu
	float ldv = dot(viewToLight, v);
	if(ldv < 0) {
		scatter = 0.f;
		return;
	}

	// TODO: mapping not fragment shader dependent but relative
	// expensive, could be done in vertex shader (or cpu but
	// has to be refreshed every time viewPos changes...)
	// mapped position of a directional light
	vec3 mappedLightPos = sceneMap(scene.proj, scene.viewPos - light.dir);
	if(clamp(mappedLightPos, 0.0, 1.0) != mappedLightPos) {
		// in this case the light is behind the camera, there
		// will be no depth scattering.
		scatter = 0.f;
		return;
	}

	// lightDepth on far plane, everything in front of it
	scatter = lightScatterDepth(uv, mappedLightPos.xy,
		999.f, ldv, depthTex, ldepth);
}
