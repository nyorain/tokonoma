float sh0(vec3 nrm) { return 0.282095; }
float sh1(vec3 nrm) { return 0.488603 * nrm.y; }
float sh2(vec3 nrm) { return 0.488603 * nrm.z; }
float sh3(vec3 nrm) { return 0.488603 * nrm.x; }
float sh4(vec3 nrm) { return 1.092548 * nrm.x * nrm.y; }
float sh5(vec3 nrm) { return 1.092548 * nrm.y * nrm.z; }
float sh6(vec3 nrm) { return 0.315392 * (3.0 * nrm.z * nrm.z - 1.0); }
float sh7(vec3 nrm) { return 1.092548 * nrm.x * nrm.z; }
float sh8(vec3 nrm) { return 0.546274 * (nrm.x * nrm.x - nrm.y * nrm.y); }

vec3 evalSH(vec3 nrm, vec3 coeffs[9]) {
	return coeffs[0] * sh0(nrm) +
		coeffs[1] * sh1(nrm) +
		coeffs[2] * sh2(nrm) +
		coeffs[3] * sh3(nrm) +
		coeffs[4] * sh4(nrm) +
		coeffs[5] * sh5(nrm) +
		coeffs[6] * sh6(nrm) +
		coeffs[7] * sh7(nrm) +
		coeffs[8] * sh8(nrm);
}

vec3 evalIrradianceSH(vec3 nrm, vec3 coeffs[9]) {
	const float pi = 3.1415926535897932;
	const float h0 = pi;
	const float h1 = 2.0 * pi / 3.0;
	const float h2 = pi / 4.0;
	return h0 * coeffs[0] * sh0(nrm) +
		h1 * coeffs[1] * sh1(nrm) +
		h1 * coeffs[2] * sh2(nrm) +
		h1 * coeffs[3] * sh3(nrm) +
		h2 * coeffs[4] * sh4(nrm) +
		h2 * coeffs[5] * sh5(nrm) +
		h2 * coeffs[6] * sh6(nrm) +
		h2 * coeffs[7] * sh7(nrm) +
		h2 * coeffs[8] * sh8(nrm);
}
