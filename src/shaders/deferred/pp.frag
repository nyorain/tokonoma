#version 450

layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0, input_attachment_index = 0)
	uniform subpassInput inLight;

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
	vec4 color = subpassLoad(inLight);

	// NOTE: not sure if needed. We have an srgb framebuffer
	// color.rgb = pow(color.rgb, vec3(1.0 / 2.2));
	fragColor = vec4(tonemap(color.rgb), 1.0);
}
