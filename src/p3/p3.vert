layout(location = 0) in vec3 inPos;
layout(location = 1) in float inLifetime;
layout(location = 2) in vec3 inVelocity;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, row_major) uniform Ubo {
	mat4 vp;
	vec3 camPos;
	float dt;
	vec3 attrPos;
	float targetZ;
} ubo;

void main() {
	gl_Position = ubo.vp * vec4(inPos, 1);

	float dist = distance(ubo.camPos, inPos);
	float radius = 1;

	if(dist > ubo.targetZ) {
		radius += 0.5 * pow(dist - ubo.targetZ, 2);
	} else {
		radius += 10 * pow(ubo.targetZ - dist, 2);
	}

	// radius += 5 * max(max(dist / ubo.targetZ, ubo.targetZ / dist) - 0.5, 0.0);

	radius = clamp(radius, 2, 200);
	gl_PointSize = radius;

	float alpha = 0.5 / (radius * radius);

#define FAST_LARGE
#ifdef FAST_LARGE
	float i = radius / 20;
	uint e2 = uint(floor(exp2(i)));
	alpha *= e2;
	if(gl_VertexIndex % e2 != 0) {
		gl_PointSize = 0.0; // discard
	}

	// Sketch of more complex implementation with some advantages.
	// Maybe we can replicate this in a simpler way?
	// if(radius > 10) {
	// 	alpha *= 2;
	// 	if(gl_VertexIndex % 2 == 1)	{
	// 		gl_PointSize = 0.0;
	// 	}
	// }
	// if(radius > 20) {
	// 	alpha *= 2;
	// 	if(gl_VertexIndex % 4 == 2)	{
	// 		gl_PointSize = 0.0;
	// 	}
	// }
	// if(radius > 30) {
	// 	alpha *= 2;
	// 	if(gl_VertexIndex % 8 == 4)	{
	// 		gl_PointSize = 0.0;
	// 	}
	// }
	// if(radius > 40) {
	// 	alpha *= 2;
	// 	if(gl_VertexIndex % 16 == 8)	{
	// 		gl_PointSize = 0.0;
	// 	}
	// }
#endif // FAST_LARGE

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
