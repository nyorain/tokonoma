#version 460

layout(location = 0) in vec3 uvw;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1, std430) buffer SSBO {
	vec3 coeffs[];
};

void main() {
	vec3 nrm = normalize(uvw);

	float c0 = 0.886228; // l = m = 0
	float c1 = 1.023328 * nrm.y; // l = 1, m = -1
	float c2 = 1.023328 * nrm.z; // l = 1, m = 0
	float c3 = 1.023328 * nrm.x; // l = 1, m = 1
	float c4 = 0.858085 * nrm.x * nrm.y; // l = 2, m = -2
	float c5 = 0.858085 * nrm.y * nrm.z; // l = 2, m = -1
	float c6 = 0.247708 * (3.0 * nrm.z * nrm.z - 1.0); // l = 2, m = 0
	float c7 = 0.858085 * nrm.x * nrm.z; // l = 2, m = 1
	float c8 = 0.429043 * (nrm.x * nrm.x - nrm.y * nrm.y); // l = 2, m = 2

	vec3 col =
		coeffs[0] * c0 +
		coeffs[1] * c1 +
		coeffs[2] * c2 +
		coeffs[3] * c3 +
		coeffs[4] * c4 +
		coeffs[5] * c5 +
		coeffs[6] * c6 +
		coeffs[7] * c7 +
		coeffs[8] * c8;

	outColor = vec4(0.3 * col, 1.0);
}
