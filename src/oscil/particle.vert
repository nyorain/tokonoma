#version 450

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform Params {
	float idstep;
	float amp;
	uint frameCount;
	float alpha;
};

layout(set = 0, binding = 1) buffer Audio {
	float stereo[];
};

// Evaluate cubic b-spline via De Boor.
// https://en.wikipedia.org/wiki/De_Boor%27s_algorithm
// We do this so the result is smoother.
layout(constant_id = 0u) const int p = 3;

float t(int i) {
	return clamp(i, 0, frameCount - 1);
}

vec2 c(int i) {
	int id = clamp(i, 0, int(frameCount) - 1);
	return vec2(stereo[2 * id + 0], stereo[2 * id + 1]);
}

vec2 splineAudio(float x) {
	int k = int(floor(x));

	vec2 d[p + 1];
	for(int j = 0; j < p + 1; ++j) {
		d[j] = c(j + k - p);
	}

	for(int r = 1; r < p + 1; ++r) {
		for(int j = p; j > r - 1; --j) {
			float alpha = (x - t(j + k - p)) / (t(j + 1 + k - r) - t(j + k - p));
			d[j] = mix(d[j - 1], d[j], alpha);
		}
	}

	return d[p];
}

void main() {
	outColor = vec4(0.1, 1.0, 0.1, alpha);

	float at = min(gl_VertexIndex * idstep, frameCount - 1);
	vec2 xy = splineAudio(at);

	// we flip y since that how the orientation of an oscilloscope in XY
	// mode works.
	xy.y = -xy.y;

	gl_Position = vec4(amp * xy, 0.0, 1.0);
	gl_PointSize = 1.f;
}
