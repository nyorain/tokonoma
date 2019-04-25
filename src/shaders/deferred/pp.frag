#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

// layout(set = 0, binding = 0, input_attachment_index = 0)
	// uniform subpassInput inLight;

layout(set = 0, binding = 0) uniform sampler2D lightTex;

// http://filmicworlds.com/blog/filmic-tonemapping-operators/
// has a nice preview of different tonemapping operators
// currently using uncharted 2 version
vec3 tonemap(vec3 x) {
	const float A = 0.15;
	const float B = 0.50;
	const float C = 0.10;
	const float D = 0.20;
	const float E = 0.02;
	const float F = 0.30;
	const float W = 11.2;
	return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

void main() {
	// vec4 color = subpassLoad(inLight);
	vec4 color = texture(lightTex, uv);

	// scattering
	// TODO: better blurring/filter
	float scatter = 0.f;
	int range = 1;
	vec2 texelSize = 1.f / textureSize(lightTex, 0);
	for(int x = -range; x <= range; ++x) {
		for(int y = -range; y <= range; ++y) {
			vec2 off = texelSize * vec2(x, y);
			scatter += texture(lightTex, uv + off).a;
		}
	}

	int total = ((2 * range + 1) * (2 * range + 1));
	scatter /= total;
	color.rgb += scatter * vec3(4.0, 3.5, 2.0); // TODO: should be light color

	// NOTE: not sure if needed. We have an srgb framebuffer
	// color.rgb = pow(color.rgb, vec3(1.0 / 2.2));
	fragColor = vec4(tonemap(color.rgb), 1.0);
}
