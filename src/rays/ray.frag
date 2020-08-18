#include "shared.glsl"

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 1, row_major) uniform Scene {
	UboData scene;
};

void main() {
	// float dist = inColor.a;
	// float falloff = 1 / (dist * dist);
	// NOTE: Light falloff in a perfect 2D world is just inverse linear,
	// not squared distance falloff. See node 1042
	// NOTE: no, nvm, we don't have to model light falloff at all. It
	// comes naturally with the light rays getting less dense far
	// away from the source. That's how falloff works.
	// float falloff = 1 / dist;
	// fragColor = vec4(inColor.rgb, 1);

	/*
	if(inColor.a < 0.00001) {
		fragColor = vec4(0, 0, 1, 1);	
		return;
	}

	float fac = 1.f / inColor.a;
	fragColor = vec4(fac * inColor.rgb, 1);
	*/

	vec3 color = inColor.rgb;
	float alpha = 1.f;
	if(scene.rayTime > -1.f) {
		float speed = 0.5;
		float dist = inColor.a / speed;
		alpha *= smoothstep(scene.rayTime - 0.05f, scene.rayTime, dist) *
			(1 - smoothstep(scene.rayTime, scene.rayTime + 0.05f, dist));
		// alpha *= (1 - smoothstep(scene.rayTime - 0.05, scene.rayTime, dist));

		alpha /= 0.1 * speed;
	} else {
		alpha = 1 / pow(inColor.a, 1);
	}

	if(alpha < 0.001) {
		discard;
	}

	fragColor = vec4(alpha * color, 1.f);
}
