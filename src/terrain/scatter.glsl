// References:
// - https://www.alanzucconi.com/2017/10/10/atmospheric-scattering-1/ and following
// - http://nishitalab.org/user/nis/cdrom/sig93_nis.pdf
// - https://developer.nvidia.com/gpugems/GPUGems2/gpugems2_chapter16.html
// - https://github.com/cosmoscout/csp-atmospheres/
// - https://github.com/cosmoscout/csp-atmospheres/
// - https://github.com/OpenSpace/OpenSpace/blob/master/modules/atmosphere
// - https://davidson16807.github.io/tectonics.js//2019/03/24/fast-atmospheric-scattering.html

const float pi = 3.1415926535897932;
// scale height: how slow density of relevant particles 
// decays when going up in atmosphere
const float rayleighH = 8500;

// Returns the density at height h for particle scale height hscale.
// h = 0 represents sea level, the base density.
float density(float h, float hscale) {
	return exp(-h / hscale);
}

// Returns the probability of scattering in the direction given by
// cosTheta (dot(lightDir, scatterDir)) for a particle with geometry
// factor g (e.g. 0 for rayleigh scattering).
// Can also be seen as the amount of energy lost in the given direction
// when scattering happens.
// Normalized so that the function integrated over the unit sphere
// results in 1.
float phase(float cosTheta, float g) {
	const float fac = 0.11936620731; // 3 / (8 * pi) for normalization
	float gg = g * g;
	float cc = cosTheta * cosTheta;
	return fac * ((1 - gg) * 1 + cc) / ((2 + gg) * pow(1 + gg * - 2 * g * c, 1.5));
}

// optimized version of phase(cosTheta, g = 0)
float phaseRayleigh(float cosTheta) {
	const float fac = 0.05968310365; // 3 / (16 * pi) for normalization
	return fac * (1 + cosTheta * cosTheta);
}
