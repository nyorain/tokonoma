// References and sources:
// - http://renderwonk.com/publications/gdm-2002/GDM_August_2002.pdf
// - http://nishitalab.org/user/nis/cdrom/sig93_nis.pdf
// - https://www.alanzucconi.com/2017/10/10/atmospheric-scattering-1/ and following
// - https://developer.nvidia.com/gpugems/GPUGems2/gpugems2_chapter16.html
// - https://github.com/cosmoscout/csp-atmospheres/
// - https://github.com/OpenSpace/OpenSpace/blob/master/modules/atmosphere
// - https://davidson16807.github.io/tectonics.js//2019/03/24/fast-atmospheric-scattering.html

// TODO: number of samples probably shouldn't be constant and
// instead be dependent on the depth to be sampled (to get constant
// quality). When using compute shader, maybe use max operation
// in subgroup?

const float pi = 3.1415926535897932;
// scale height: how slow density of relevant particles 
// decays when going up in atmosphere
const float rayleighH = 100;
const float planetRadius = 6000;
const float atmosphereRadius = 6500;

// rayleigh scattering coefficients roughly for RGB wavelengths, from
// http://renderwonk.com/publications/gdm-2002/GDM_August_2002.pdf
const vec3 scatteringCoeffs = vec3(6.95e-6, 1.18e-5, 2.44e-5);

// Returns the density at height h for particle scale height hscale.
// h = 0 represents sea level, the base density.
float density(float h, float hscale) {
	return exp(-h / hscale);
}

// Returns the probability of scattering in the direction given by
// cosTheta (= dot(lightDir, scatterDir)) for a particle with geometry
// factor g (e.g. 0 for rayleigh scattering).
// Can also be seen as the amount of energy lost in the given direction
// when scattering happens.
// Normalized so that the function integrated over the unit sphere
// results in 1.
float phase(float cosTheta, float g) {
	const float fac = 0.11936620731; // 3 / (8 * pi) for normalization
	float gg = g * g;
	float cc = cosTheta * cosTheta;
	return fac * ((1 - gg) * 1 + cc) / ((2 + gg) * pow(1 + gg * - 2 * g * cosTheta, 1.5));
}

// optimized version of phase(cosTheta, g = 0)
float phaseRayleigh(float cosTheta) {
	const float fac = 0.05968310365; // 3 / (16 * pi) for normalization
	return fac * (1 + cosTheta * cosTheta);
}

// planet center is assumed to be vec3(0)
float height(vec3 pos) {
	return length(pos) - planetRadius;
}

float opticalDepth(vec3 from, vec3 to) {
	const uint numSteps = 10u;
	vec3 dir = to - from;
	vec3 delta = dir / numSteps;
	float res = 0.0;
	float ds = length(delta);
	vec3 pos = from;
	for(uint i = 0u; i < numSteps; ++i) {
		res += density(height(pos), rayleighH);
		pos += delta;
	}

	return res * ds;
}

// assumes that rd is normalized
float intersectRaySphere(vec3 ro, vec3 rd, vec3 sc, float sr) {
	vec3 oc = ro - sc;
	float a = dot(oc, rd); // p/2
	float b = dot(oc, oc) - sr * sr; // q
	float c = a * a - b; // (p/2)^2 - q

	// for c == 0.0 there is exactly one intersection (tangent)
	// for c > 0.0, there are two intersections
	if(c < 0.0) {
		return -1.0;
	}

	c = sqrt(c);
	float r1 = -a - c;
	return (r1 >= 0) ? r1 : -a + c;
}

// sun here doesn't have a position but only a direction
// this approximation probably isn't a good idea when one can leave
// the atmosphere/scales unknown. Assumed to be normalized
// NOTE: we assume here that the viewer is inside the atmosphere,
// requires slight tweaks (e.g. for intersection) when that isn't the case
vec3 sampleRay(vec3 from, vec3 to, vec3 sunDir, out float totalOD) {
	const uint numSteps = 100u;
	vec3 dir = to - from;
	vec3 delta = dir / numSteps;
	float dt = length(delta);
	vec3 pos = from;
	vec3 res = vec3(0.0);
	float od = 0.0; // optical depth along primary ray

	totalOD = 0.0;
	for(uint i = 0u; i < numSteps; ++i) {
		float h = height(pos);
		if(h < 0) {
			break;
		}

		float d = density(h, rayleighH);
		od += d * dt;
		pos += delta;

		// sample incoming scattering of secondary ray at pos
		// find the end of the secondary ray
		float srs = intersectRaySphere(pos, -sunDir, vec3(0.0), atmosphereRadius);
		float sre = intersectRaySphere(pos, -sunDir, vec3(0.0), planetRadius);

		// srs > 0.f: ray leaves the atmosphere (should always be the case
		//   when the viewer is inside it
		// sre < 0.f: the ray doesn't hit the planet (this is important
		//   even if the viewer is inside the atmosphere, remember
		//   this are secondary rays!)
		if(srs >= 0.f && sre < 0.f) {
			vec3 sre = pos - srs * sunDir;
			float odsun = opticalDepth(pos, sre);

			// TODO: random ass factor
			res += d * exp(-scatteringCoeffs * 50 * (od + odsun)) * dt;
			// res += d * exp(-scatteringCoeffs * (od + odsun)) * dt;
			// totalOD += opticalDepth(pos, sre);
		} else if(srs == -1.f) {
			// res += vec3(0, 0, 0.1);
		} else if(srs < 0.f) {
			// res += vec3(0.1, 0, 0);
		} else if(sre > 0.f) {
			// res += vec3(0, 0.1, 0);
		} else {
			// res = vec3(1, 1, 1);
		}
	}

	totalOD = od;
	return res * scatteringCoeffs;
}
