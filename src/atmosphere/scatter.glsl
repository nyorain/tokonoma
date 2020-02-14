// References and sources:
// - http://renderwonk.com/publications/gdm-2002/GDM_August_2002.pdf
// - http://nishitalab.org/user/nis/cdrom/sig93_nis.pdf
// - https://www.alanzucconi.com/2017/10/10/atmospheric-scattering-1/ and following
// - https://developer.nvidia.com/gpugems/GPUGems2/gpugems2_chapter16.html
// - https://github.com/cosmoscout/csp-atmospheres/
// - https://github.com/OpenSpace/OpenSpace/blob/master/modules/atmosphere
// - https://davidson16807.github.io/tectonics.js//2019/03/24/fast-atmospheric-scattering.html

// number of samples probably shouldn't be constant and
// instead be dependent on the depth to be sampled (to get constant
// quality). When using compute shader, maybe use max operation
// in subgroup?
// Tried to do that, does help a little but not too much.

const float pi = 3.1415926535897932;
// scale height: how slow density of relevant particles 
// decays when going up in atmosphere
const float rayleighH = 7994;
const float mieH = 1200;
const float planetRadius = 6300000;
const float atmosphereRadius = 6400000;

// rayleigh scattering coefficients roughly for RGB wavelengths, from
// http://renderwonk.com/publications/gdm-2002/GDM_August_2002.pdf
// TODO: g values for mie?
const vec3 rayleighScatteringRGB = vec3(6.95e-6, 1.18e-5, 2.44e-5);

// different fog strengths
const vec3 mieScatteringRGB0 = vec3(0, 0, 0);
const vec3 mieScatteringRGB1 = vec3(2e-5, 3e-5, 4e-5);
const vec3 mieScatteringRGB2 = vec3(8e-5, 1e-4, 1.2e-4);
const vec3 mieScatteringRGB3 = vec3(9e-4, 1e-3, 1.1e-3);
const vec3 mieScatteringRGB4 = vec3(1e-2, 1e-2, 1e-2); // 1e-5 as in paper for B?
const vec3 mieScatteringRGB = mieScatteringRGB1;
const float mieG = -0.9f;

// Returns the density at height h for particle scale height hscale.
// h = 0 represents sea level, the base density.
// The higher the scale, the faster the density goes to zero for
// higher heights h.
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
	float gg = g * g;

	// from csp atmosphere and gpugems, normalized
	const float fac = 0.11936620731; // 3 / (8 * pi) for normalization
	float cc = cosTheta * cosTheta;
	return fac * ((1 - gg) * (1 + cc)) / ((2 + gg) * pow(1 + gg - 2 * g * cosTheta, 1.5));
	
	// from the 2002 GDM paper. More colorful, maybe because it's
	// not properly normalized?
	// return (1 - g) * (1 - g) / (4 * pi * pow(1 + gg  - 2 * g * cosTheta, 1.5));
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

float opticalDepth(vec3 from, vec3 to, float hscale) {
	// const uint numSteps = 1 + uint(clamp(2 * length(to - from) / (atmosphereRadius - planetRadius), 0, 4));
	const uint numSteps = 4u;

	vec3 dir = to - from;
	vec3 delta = dir / numSteps;
	float res = 0.0;
	float ds = length(delta);
	vec3 pos = from;
	for(uint i = 0u; i < numSteps; ++i) {
		res += density(height(pos), hscale);
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

float intersectRaySphereBack(vec3 ro, vec3 rd, vec3 sc, float sr) {
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
	return (r1 >= 0) ? -a + c : r1;
}

struct Inscatter {
	vec3 rayleigh;
	vec3 mie;
};

// sun here doesn't have a position but only a direction
// this approximation probably isn't a good idea when one can leave
// the atmosphere/scales unknown. Assumed to be normalized
// NOTE: we assume here that the viewer is inside the atmosphere,
// requires slight tweaks (e.g. for intersection) when that isn't the case
Inscatter sampleRay(vec3 from, vec3 to, vec3 sunDir, float noise) {
	// const uint numSteps = 50u;
	// const uint numSteps = 5 + uint(clamp(45 * length(to - from) / (atmosphereRadius - planetRadius), 0, 100));
	// const uint numSteps = 50;
	// const uint numSteps = 5 + uint(clamp(20 * length(to - from) / (atmosphereRadius - planetRadius), 0, 20));
	const uint numSteps = 20;

	vec3 dir = to - from;
	vec3 delta = dir / numSteps;
	float dt = length(delta);
	vec3 pos = from;
	pos += 0.05 * noise * delta;

	vec3 resR = vec3(0.0);
	vec3 resM = vec3(0.0);

	float odR = 0.0; // optical depth along primary ray, rayleigh
	float odM = 0.0; // optical depth along primary ray, mie

	for(uint i = 0u; i < numSteps; ++i) {
		float h = max(height(pos), 0);
		// if(h < 0) {
		// 	break;
		// }

		pos += delta;

		float dR = density(h, rayleighH);
		float dM = density(h, mieH);
		odR += dR * dt;
		odM += dM * dt;

		// sample incoming scattering of secondary ray at pos
		// find the end of the secondary ray
		float srs = intersectRaySphere(pos, -sunDir, vec3(0.0), atmosphereRadius);
		float sre = intersectRaySphere(pos, -sunDir, vec3(0.0), planetRadius);

		// srs > 0.f: ray leaves the atmosphere (should always be the case
		//   when the viewer is inside it
		// sre < 0.f: the ray doesn't hit the planet (this is important to check
		//   even if the viewer is inside the atmosphere, remember
		//   this are secondary rays!)
		if(srs >= 0.f && sre < 0.f) {
			vec3 atmosLeave = pos - srs * sunDir;
			float odsunR = opticalDepth(pos, atmosLeave, rayleighH);
			float odsunM = opticalDepth(pos, atmosLeave, mieH);

			// those two are equivalent
			// vec3 outScatter = exp(-rayleighScatteringRGB * (odR + odsunR)) * exp(-mieScatteringRGB * (odM + odsunM));
			vec3 outd = rayleighScatteringRGB * (odR + odsunR) + mieScatteringRGB * (odM + odsunM);
			vec3 outScatter = exp(-outd);

			resR += dR * outScatter * dt;
			resM += dM * outScatter * dt;

			// incorrect: we have to consider both out-scattering for
			// each in-scattering effect
			// resR += dR * exp(-rayleighScatteringRGB * (odR + odsunR)) * dt;
			// resM += dM * exp(-mieScatteringRGB * (odM + odsunM)) * dt;

		// The branches below only exist for debugging. They are
		// supposed to be no-ops
		} /*else if(srs == -1.f) {
			// no scattering: ray already outside atmosphere.
			// this should never happen though
			// res += vec3(0, 0, 0.1);
		} else if(srs < 0.f) {
			// no scattering: ray already outside atmosphere.
			// this should never happen though
			// res += vec3(0.1, 0, 0);
		} else if(sre > 0.f) {
			// ray hits earth before going to the sun (e.g. if sun is
			// on the other side of the planet)
			// res += vec3(0, 0.1, 0);
		} else {
			// weird case
			// res = vec3(1, 1, 1);
		}*/
	}

	resM *= mieScatteringRGB;
	resR *= rayleighScatteringRGB;
	return Inscatter(resR, resM);
}
