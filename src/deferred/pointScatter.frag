#version 450

#extension GL_GOOGLE_include_directive : enable

#include "scene.glsl"
#include "scatter.glsl"
#include "pbr.glsl"

const bool depthScatter = false;

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
	PointLight light;
};

layout(set = 2, binding = 1) uniform samplerCubeShadow shadowCube;

float shadowMap(vec3 worldPos) {
	return pointShadow(shadowCube, light.pos, light.radius, worldPos);
}

void main() {
	float ldepth = texture(depthTex, uv).r;
	float depth = ztodepth(ldepth, scene.near, scene.far);
	vec2 suv = 2 * uv - 1;
	suv.y *= -1.f; // flip y
	vec4 pos4 = scene.invProj * vec4(suv, depth, 1.0);
	vec3 pos = pos4.xyz / pos4.w;

	vec3 v = normalize(pos - scene.viewPos); // view direction
	vec3 viewToLight = normalize(light.pos - scene.viewPos);

	// TODO: not sure what light direction is here... direction from
	// camera to light? but then we might have dot(ldir, vdir) < 0, although
	// the camera *contains* the light which leads to weird edges
	// float ldv = dot(viewToLight, v);
	float ldv = 1.0;
	// vec3 posToLight = normalize(light.pos - pos);
	// float ldv = dot(viewToLight, posToLight);

	vec3 mappedLightPos = sceneMap(scene.proj, light.pos);
	if(depthScatter) {
		// TODO: mapping not fragment shader dependent but somewhat
		// expensive, could be done in vertex shader (or cpu but
		// has to be refreshed every time viewPos changes...)
		// mapped position of a directional light
		if(clamp(mappedLightPos, 0.0, 1.0) != mappedLightPos) {
			// in this case the light is behind the camera, there
			// will be no depth scattering.
			scatter = 0.f;
			return;
		}

		float lightDepth = depthtoz(mappedLightPos.z, scene.near, scene.far);
		scatter = lightScatterDepth(uv, mappedLightPos.xy,
			lightDepth, ldv, depthTex, ldepth);
	} else {
		vec2 pixel = gl_FragCoord.xy;
		scatter = lightScatterShadow(scene.viewPos, pos, ldv, pixel);
	}

	// attentuation volume
	// TODO: document how the projection here works
	vec4 projpl = scene.invProj * vec4(suv, mappedLightPos.z, 1.0);
	projpl.xyz /= projpl.w;
	float dist = distance(light.pos, projpl.xyz);
	// scatter *= attenuation(dist, light.attenuation);
	scatter *= attenuation(dist, light.radius);
}
