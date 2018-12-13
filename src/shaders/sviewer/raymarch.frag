#version 450

// we treat all coordinates as in range [-1, 1]
// with y axis going upwards
// z axis is (as in opengl/vulkan convention) coming out of screen

layout(location = 0) in vec2 inuv;
layout(location = 0) out vec4 outcol;

layout(set = 0, binding = 0) uniform UBO {
	vec2 mpos;
	float time;

	float _; // alignment
	vec3 camPos; // camera position that should be used for 3d stuff
} ubo;

struct Ray {
	vec3 origin;
	vec3 dir;
};

vec3 at(Ray ray, float t) {
	return ray.origin + t * ray.dir;
}

vec3 mapCube(vec3 p, vec3 center, float halflength) {
	p = (p - center) / halflength;
	return 0.05 * float(p == clamp(p, -1.f, 1.f)) * vec3(0.5 * p + 1);
}

vec3 mapSphere(vec3 p, vec3 center, float radius) {
	// sphere with radius 0 at position (0, 0, -4)
	p -= center;
	return 0.01 * float(length(p) < radius) * vec3(1);
}

vec3 scene(vec3 p) {
	return mapCube(p, vec3(0, 0, -4), 0.5) +
		mapSphere(p, vec3(2, 3, -12), 2);
}

void main() {
	vec2 mpos = 2 * ubo.mpos - 1;
	mpos.y *= -1;

	vec2 uv = 2 * inuv - 1;
	uv.y *= -1;

	Ray ray = {ubo.camPos, normalize(vec3(mpos + uv, -1.5))};

	vec3 col = vec3(0, 0, 0);
	float t = 0.f;

	// TODO(idea): use higher order integral approximation?
	// raymarch over map
	float step = 0.01;
	for(int i = 0; i < 500; ++i) {
		vec3 pos = at(ray, t);
		col += step * scene(pos);
		t += step;
	}

	outcol = vec4(col, 1.f);
}