const float oneOverPi = 0.31830988618;

float sh0(vec3 nrm) { return 0.886228; }
float sh1(vec3 nrm) { return 1.023328 * nrm.y; }
float sh2(vec3 nrm) { return 1.023328 * nrm.z; }
float sh3(vec3 nrm) { return 1.023328 * nrm.x; }
float sh4(vec3 nrm) { return 0.858085 * nrm.x * nrm.y; }
float sh5(vec3 nrm) { return 0.858085 * nrm.y * nrm.z; }
float sh6(vec3 nrm) { return 0.247708 * (3.0 * nrm.z * nrm.z - 1.0); }
float sh7(vec3 nrm) { return 0.858085 * nrm.x * nrm.z; }
float sh8(vec3 nrm) { return 0.429043 * (nrm.x * nrm.x - nrm.y * nrm.y); }

vec3 evalSH(vec3 nrm, vec3 coeffs[9]) {
	return oneOverPi *
		coeffs[0] * sh0(nrm) +
		coeffs[1] * sh1(nrm) +
		coeffs[2] * sh2(nrm) +
		coeffs[3] * sh3(nrm) +
		coeffs[4] * sh4(nrm) +
		coeffs[5] * sh5(nrm) +
		coeffs[6] * sh6(nrm) +
		coeffs[7] * sh7(nrm) +
		coeffs[8] * sh8(nrm);
}
