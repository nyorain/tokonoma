#include "particle.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in float inLifetime;
layout(location = 2) in vec3 inVelocity;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, row_major) uniform Ubo {
	UboData ubo;
};

void main() {
	gl_Position = ubo.vp * vec4(inPos, 1);

	float dist = 0.001 + distance(ubo.camPos, inPos);
	float radius = 0.5;

	radius /= clamp(5 * dist, 0.01, 100); // perspective; larger near camera

	float alpha = 0.5;
	if(radius < 1.0) {
		alpha *= radius;
		radius = 1.0;
	}

	if(radius > 5) {
		alpha *= radius / 5;
		radius = 5;
	}

	// circle of confusion
	/*
	if(dist > ubo.targetZ) {
		radius += 0.5 * pow(dist - ubo.targetZ, 2);
	} else {
		radius += 10 * pow(ubo.targetZ - dist, 2);
	}
	*/

	// float md = max(dist / ubo.targetZ, ubo.targetZ / dist);
	// md = clamp(pow(max(md - 0.5, 0.0), 2), 0.f, 999.9);
	float F = 0.1f; // focal length, 50mm
	float A = F / 1.0; // aperature
	float unitFactor = 1000.f; // how many meters are one world-space unit
	float maxBgCoc = A * F / (unitFactor * ubo.targetZ - F);
	float md = 1000 * unitFactor * (1 - ubo.targetZ / dist) * maxBgCoc;

	float coc = 1.0 * abs(md);
	alpha *= 1 / (1 + coc * coc);

	radius *= (1 + coc);
	radius = clamp(radius, 0.0, 400);

	/*
	if(radius > 400) {
		alpha *= radius / 400;
		radius = 400;
	}
	*/


#define FAST_LARGE
#ifdef FAST_LARGE
	/*
	float i = coc / 20;
	uint e2 = uint(floor(exp2(i)));

	// correct approimation: alpha *= e2. But that leads to flickering.
	alpha *= sqrt(e2);
	// alpha *= e2;
	if(gl_VertexIndex % e2 != 0) {
		radius = 0.0; // discard
	}
	*/

	// Sketch of more complex implementation with some advantages.
	// Maybe we can replicate this in a simpler way?
	float alphaFac = 1.0;
	for(uint i = 1u; i < 50; ++i) {
		// if(coc < i * 10) {
		if(radius < i * 10) {
			break; // pass test
		}

		alphaFac *= 2.0;
		if(gl_VertexIndex % uint(exp2(i)) == exp2(i - 1)) {
			radius = 0.0; // discard
			break;
		}
	}

	alpha *= min(alphaFac, 1000.0);

	/*
	if(coc > 10) {
		alpha *= 2;
		if(gl_VertexIndex % 2 == 1)	{
			radius = 0.0;
		}
	}
	if(coc > 20) {
		alpha *= 2;
		if(gl_VertexIndex % 4 == 2)	{
			radius = 0.0;
		}
	}
	if(coc > 30) {
		alpha *= 2;
		if(gl_VertexIndex % 8 == 4)	{
			radius = 0.0;
		}
	}
	if(coc > 40) {
		alpha *= 2;
		if(gl_VertexIndex % 16 == 8)	{
			radius = 0.0;
		}
	}
	*/
#endif // FAST_LARGE

	gl_PointSize = radius;

	// fade out at end of lifetime
	alpha *= smoothstep(1.0, 0.9, inLifetime);	

	// #1: color based on velocity
	// vec3 color = 0.5 + 4 * inVelocity;

	// #2: color based on velocity and lifetime
	float g = mix(0.3, 1.0, inLifetime);
	float b = mix(0.3, 1.0, 3 * length(inVelocity));
	vec3 color = vec3(1.0, g, b);

	outColor = vec4(color, alpha);
}
