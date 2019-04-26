#version 450

layout(location = 0) in vec2 inuv;
layout(location = 0) out vec4 outcol;

layout(set = 0, binding = 0) uniform UBO {
	vec2 mpos;
	float time;
} ubo;

float ncos(float t) { return 0.5 + 0.5 * cos(t); }
float nsin(float t) { return 0.5 + 0.5 * cos(t); }

void main() {
	int pcount = 5;
	vec2 points[] = {
		0.3 + vec2(nsin(ubo.time) / 5, 0.3 * ncos(ubo.time * 0.32)),
		0.65 + 0.15 * vec2(nsin(1.5 * ubo.time), ncos(ubo.time)),
		vec2(0.46 + 0.4 * nsin(0.08 * ubo.time), 0.95 - nsin(0.12 * ubo.time)),
		vec2(0.16 + ncos(0.2 * ubo.time), 0.75 - 0.6 * nsin(1.7 * ubo.time)),
		ubo.mpos
	};

	float d = 10.f;
	int nearest = 0;
	for(int i = 0; i < pcount; ++i) {
		float dn = distance(points[i], inuv);
		if(dn < d) {
			d = dn;
			nearest = i;
		}
	}

	d = smoothstep(0.95, 1.0, sin(300 * d)); // isolines
	d = clamp(d, 0, 1);
	float p1 = pcount - 1.f;
	vec3 col = vec3(nearest / p1, 1 - nearest / p1, 1.f);
	outcol = vec4(d * col, 1.f);
	outcol.rgb = pow(outcol.rgb, vec3(2.2f)); // roughly convert to srgb
}
