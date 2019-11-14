#version 460

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D history;

const float exposure = 1.0;

#define SIGMA 1.1
#define BSIGMA (1 / SIGMA)
#define MSIZE 5

float normpdf(in float x, in float sigma) {
	return 0.39894*exp(-0.5*x*x/(sigma*sigma))/sigma;
}

float normpdf3(in vec3 v, in float sigma) {
	return 0.39894*exp(-0.5*dot(v,v)/(sigma*sigma))/sigma;
}

vec3 colBlurred(vec2 uv) {
	vec2 texelSize = 1.f / textureSize(history, 0);
	vec3 c = texture(history, uv).rgb;

	const int kSize = (MSIZE-1)/2;
	float kernel[MSIZE];
	vec3 final_color = vec3(0.0);

	//create the 1-D kernel
	float Z = 0.0;
	for(int j = 0; j <= kSize; ++j) {
		kernel[kSize+j] = kernel[kSize-j] = normpdf(float(j), SIGMA);
	}

	vec3 cc;
	float factor;
	float bZ = 1.0/normpdf(0.0, BSIGMA);
	//read out the texels
	for (int i=-kSize; i <= kSize; ++i) {
		for (int j=-kSize; j <= kSize; ++j) {
			vec2 ouv = uv + texelSize * vec2(i, j);
			cc = texture(history, ouv).rgb;
			factor = normpdf3(cc-c, BSIGMA)*bZ*kernel[kSize+j]*kernel[kSize+i];
			Z += factor;
			final_color += factor*cc;
		}
	}

	return final_color;
}


void main() {
	// vec3 col = colBlurred(uv);
	vec3 col = texture(history, uv).rgb;
	// col = 1.0 - exp(-exposure * col); // reinhard tonemap
	fragColor = vec4(col, 1.0);
}
