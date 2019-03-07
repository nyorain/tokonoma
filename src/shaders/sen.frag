#version 450

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

float sphereIntersection(vec3 ro, vec3 rd, vec3 ce, float ra) {
    vec3 oc = ro - ce;
    float b = dot(oc, rd);
    float c = dot(oc, oc) - ra * ra;
    float h = b*b - c;
    if(h < 0.0) { // no intersection
		return -1.0;
	}
	return -b - sqrt(h);
}

vec3 sphereNormal(vec3 ce, vec3 pos) {
	return normalize(pos - ce);
}

vec3 trace(vec3 ro, vec3 rd) {
	// scene
	vec3 sphereCenter = vec3(0, 0, -3.f);
	float t = sphereIntersection(ro, rd, sphereCenter, 0.5f);
	out_color = vec4(0, 0, 0, 1);
	if(t > 0) {
		vec3 pos = ro + t * rd;
		vec3 normal = sphereNormal(sphereCenter, pos);
		return vec3(0.8 * clamp(dot(normal, normalize(lightPos - pos)), 0, 1));
	} else {
		return vec3(0);
	}
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

			vec3 col = trace(ro, rd);
			rgb += (1 / count) * col;
		}
	}

	out_color = vec4(rgb, 1);
}
