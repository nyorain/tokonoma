#version 450

#extension GL_GOOGLE_include_directive : enable

#include "ray.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform UBO {
	vec4 pos;
	vec4 dir;
	float fov; // on y coord
	float aspect; // aspect ratio: x / y
	vec2 res; // resolution
} ubo;

const vec3 up = vec3(0, 1, 0);
const vec3 lightPos = vec3(0, 5, 0);
const int maxBounce = 2;

vec4 spheres[] = {
	{0, 0, -3, 1},
	{3, 0, -4, 0.5},
	{1, 2, 3, 2},
};

Box boxes[] = {
	box(vec3(-4.0, 0.0, 0.0)),
	box(vec3(4.0, -4.0, 1.0)),
};

// TODO: fuck, recursion
vec3 trace(Ray ray, int bounce);

vec3 shade(vec3 pos, vec3 normal, vec3 view, int bounce) {
	// sum of those should be 1
	float ambientFac = 0.1;
	float diffFac = 0.4;
	float specFac = 0.4;
	float reflFac = 0.1;

	float shininess = 32;

	vec3 col = vec3(0.1); // ambient
	vec3 l = normalize(lightPos - pos);

	// diffuse
	col += diffFac * clamp(dot(normal, l), 0, 1);

	// specular: blinn phong
	vec3 h = normalize(-view + l);
	col += specFac * vec3(clamp(pow(dot(h, normal), shininess), 0, 1));

	// XXX: trace perfect reflextion
	if(bounce < maxBounce) {
		vec3 r = reflect(normal, view);
		col += reflFac * trace(Ray(pos, r), bounce + 1);
	}

	return clamp(col, 0.0, 1.0);
}

vec3 trace(Ray ray, int bounce) {
	float t;
	float mint = 1.f / 0.f; // infinity
	vec3 mincol = vec3(0, 0, 0); // default, background

	// spheres
	for(int i = 0; i < spheres.length(); ++i) {
		t = intersect(ray, spheres[i]);
		if(t > 0.0 && t < mint) {
			mint = t;

			// shade
			vec3 pos = ray.origin + t * ray.dir;
			vec3 normal = sphereNormal(spheres[i].xyz, pos);
			mincol = shade(pos, normal, ray.dir, bounce);
		}
	}

	// boxes
	for(int i = 0; i < boxes.length(); ++i) {
		vec3 normal;
		t = intersect(ray, boxes[i], normal);
		if(t > 0.0 && t < mint) {
			mint = t;

			// shade
			vec3 pos = ray.origin + t * ray.dir;
			mincol = shade(pos, normal, ray.dir, bounce);
		}
	}

	return mincol;
}

void main() {
	vec2 pixel = 1 / ubo.res;
	vec3 az = normalize(ubo.dir.xyz);
	vec3 ax = cross(az, up);
	vec3 ay = cross(ax, az);

	vec2 uv = 2 * in_uv - 1;
	uv.y = -uv.y;
	float maxy = cos(ubo.fov / 2); // TODO: or is that maxx?

	int xc = 2;
	int yc = 2;
	float count = 4 * xc * yc;
	vec3 rgb = vec3(0, 0, 0);
	for(int x = -xc; x <= xc; ++x) {
		for(int y = -yc; y <= yc; ++y) {
			vec2 muv = uv;
			muv += vec2(x, y) * pixel;
			muv.y *= maxy;
			muv.x *= ubo.aspect * maxy;
			vec3 ro = ubo.pos.xyz;
			vec3 rd = normalize(az + muv.x * ax + muv.y * ay);

			vec3 col = trace(Ray(ro, rd), 0);
			rgb += (1 / count) * col;
		}
	}

	// TODO: tone mapping
	out_color = vec4(rgb, 1);
}
