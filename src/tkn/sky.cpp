// Paper source:
// An Analytic Model for Full Spectral Sky-Dome Radiance,
//   Lukas Hosek and Alexander Wilkie, 2012, SIGGGRAPH
//
// See https://cgg.mff.cuni.cz/projects/SkylightModelling/.
// - Used Hosek & Wilkie's reference implementation (version 1.4a).
//   License: 3-clause BSD, see ArHosekSkyModelData_CIEXYZ.h for the full license
// - For the table-based implementation and for additional understanding,
//   used the implementation by Andrew Willmott (June 2020, Unilicense), see
//   https://github.com/andrewwillmott/sun-sky.
//   A lot of the code here is more or less directly copied from his
//   great implementation.

#include <tkn/sky.hpp>
#include <tkn/util.hpp>
#include <nytl/math.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/approx.hpp>
#include <dlg/dlg.hpp>

// definitions included elsewhere, we just need the extern fwd-decls below
// #include <tkn/sky/ArHosekSkyModelData_CIEXYZ.h>

extern const double datasetXYZ1[2][10][6][9];
extern const double datasetXYZ2[2][10][6][9];
extern const double datasetXYZ3[2][10][6][9];

extern const double datasetXYZRad1[2][10][6];
extern const double datasetXYZRad2[2][10][6];
extern const double datasetXYZRad3[2][10][6];

namespace tkn::hosekSky {
namespace {

using nytl::constants::pi;
using std::sqrt, std::pow, std::exp;
constexpr auto halfPi = pi / 2.0;
constexpr auto twoPi = 2.0 * pi;

template<typename T>
inline T evalQuintic(const T w[6], const T data[6]) {
	return w[0] * data[0]
		 + w[1] * data[1]
		 + w[2] * data[2]
		 + w[3] * data[3]
		 + w[4] * data[4]
		 + w[5] * data[5];
}

template<typename T>
inline void evalQuintic(const T w[6], const T data[6][9], T coeffs[9]) {
	for(auto i = 0u; i < 9; i++) {
		coeffs[i] = w[0] * data[0][i]
			+ w[1] * data[1][i]
			+ w[2] * data[2][i]
			+ w[3] * data[3][i]
			+ w[4] * data[4][i]
			+ w[5] * data[5][i];
	}
}

template<typename T>
inline void findQuinticWeights(T s, T w[6]) {
	float s1 = s;
	float s2 = s1 * s1;
	float s3 = s1 * s2;
	float s4 = s2 * s2;
	float s5 = s2 * s3;

	float is1 = 1.0f - s1;
	float is2 = is1 * is1;
	float is3 = is1 * is2;
	float is4 = is2 * is2;
	float is5 = is2 * is3;

	w[0] = is5;
	w[1] = is4 * s1 *  5.0f;
	w[2] = is3 * s2 * 10.0f;
	w[3] = is2 * s3 * 10.0f;
	w[4] = is1 * s4 *  5.0f;
	w[5] =       s5;
}

float findHosekCoeffs(
		const double dataset9[2][10][6][9], // albedo x 2, turbidity x 10, quintics x 6, weights x 9
		const double datasetR[2][10][6],    // albedo x 2, turbidity x 10, quintics x 6
		float turbidity,
		float albedo,
		float solarElevation,
		std::array<float, 9>& coeffs) {

	const int tbi = std::clamp(int(std::floor(turbidity)), 1, 9);
	const double tbf = turbidity - tbi;
	const double s = std::pow(solarElevation / halfPi, (1.0f / 3.0f));

	double quinticWeights[6];
	findQuinticWeights(s, quinticWeights);

	double ic[4][9];

	evalQuintic(quinticWeights, dataset9[0][tbi - 1], ic[0]);
	evalQuintic(quinticWeights, dataset9[1][tbi - 1], ic[1]);
	evalQuintic(quinticWeights, dataset9[0][tbi    ], ic[2]);
	evalQuintic(quinticWeights, dataset9[1][tbi    ], ic[3]);

	double ir[4] = {
		evalQuintic(quinticWeights, datasetR[0][tbi - 1]),
		evalQuintic(quinticWeights, datasetR[1][tbi - 1]),
		evalQuintic(quinticWeights, datasetR[0][tbi    ]),
		evalQuintic(quinticWeights, datasetR[1][tbi    ]),
	};

	double cw[4] = {
		(1.0f - albedo) * (1.0f - tbf),
		albedo          * (1.0f - tbf),
		(1.0f - albedo) * tbf,
		albedo          * tbf,
	};

	for(int i = 0; i < 9; i++) {
		coeffs[i] = cw[0] * ic[0][i]
				  + cw[1] * ic[1][i]
				  + cw[2] * ic[2][i]
				  + cw[3] * ic[3][i];
	}

	return cw[0] * ir[0] + cw[1] * ir[1] + cw[2] * ir[2] + cw[3] * ir[3];
}

// Hosek:
// (1 + A e ^ (B / cos(t))) (1 + C e ^ (D g) + E cos(g) ^ 2   + F mieM(g, G)  + H cos(t)^1/2 + (I - 1))
//
// These bits are the same as Preetham, but do different jobs in some cases
// A: sky gradient, carries white -> blue gradient
// B: sky tightness
// C: sun, carries most of sun-centred blue term
// D: sun tightness, higher = tighter
// E: rosy hue around sun
//
// Hosek-specific:
// F: mie term, does most of the heavy lifting for sunset glow
// G: mie tuning
// H: zenith gradient
// I: constant term balanced with H

// Notes:
// A/B still carries some of the "blue" base of sky, but much comes from C/D
// C/E minimal effect in sunset situations, carry bulk of sun halo in sun-overhead
// F/G sunset glow, but also takes sun halo from yellowish to white overhead
float evalHosekCoeffs(const std::array<float, 9>& coeffs, float cosTheta,
		float gamma, float cosGamma) {

	dlg_assert(cosTheta >= 0.f);
	dlg_assert(cosTheta <= 1.f);
	dlg_assert(gamma >= 0.f);
	dlg_assert(gamma <= 2 * pi);
	dlg_assert(std::cos(gamma) == nytl::approx(cosGamma));

	// Ordering of coeffs:
	// AB I CDEF HG
	// 01 2 3456 78
	const float expM = exp(coeffs[4] * gamma); // D g
	const float rayM = cosGamma * cosGamma; // Rayleigh scattering
	const float mieG = coeffs[8]; // G
	const float mieM = (1.0f + rayM) / pow((1.0f + mieG * mieG - 2.0f * mieG * cosGamma), 1.5f);
	const float zenith = sqrt(cosTheta); // vertical zenith gradient

	const float f0 = 1.0f + coeffs[0] * exp(coeffs[1] / (cosTheta + 0.01f)); // A, B
	const float f1 = 1.0f
				 + coeffs[3] * expM     // C
				 + coeffs[5] * rayM     // E
				 + coeffs[6] * mieM     // F
				 + coeffs[7] * zenith   // H
				 + (coeffs[2] - 1.0f);  // I
	return f0 * f1;
}

float zenithLuminance(float thetaS, float T) {
    float chi = (4.0f / 9.0f - T / 120.0f) * (pi - 2.0f * thetaS);
    float Lz = (4.0453f * T - 4.9710f) * std::tan(chi) - 0.2155f * T + 2.4192f;
    Lz *= 1000.0; // conversion from kcd/m^2 to cd/m^2
    return Lz;
}

// ZH routines from SHLib
const float kZH_Y_0 = sqrt( 1 / (   4 * pi));  //         1
const float kZH_Y_1 = sqrt( 3 / (   4 * pi));  //         z
const float kZH_Y_2 = sqrt( 5 / (  16 * pi));  // 1/2     (3 z^2 - 1)
const float kZH_Y_3 = sqrt( 7 / (  16 * pi));  // 1/2     (5 z^3 - 3 z)
const float kZH_Y_4 = sqrt( 9 / ( 256 * pi));  // 1/8     (35 z^4 - 30 z^2 + 3)
const float kZH_Y_5 = sqrt(11 / ( 256 * pi));  // 1/8     (63 z^5 - 70 z^3 + 15 z)
const float kZH_Y_6 = sqrt(13 / (1024 * pi));  // 1/16    (231 z^6 - 315 z^4 + 105 z^2 - 5)

void calcCosPowerSatZH7(float n, float zcoeffs[7]) {
	zcoeffs[0] =   1.0f / (n + 1);
	zcoeffs[1] =   1.0f / (n + 2);
	zcoeffs[2] =   3.0f / (n + 3) -   1.0f / (n + 1);
	zcoeffs[3] =   5.0f / (n + 4) -   3.0f / (n + 2);
	zcoeffs[4] =  35.0f / (n + 5) -  30.0f / (n + 3) +   3.0f / (n + 1);
	zcoeffs[5] =  63.0f / (n + 6) -  70.0f / (n + 4) +  15.0f / (n + 2);
	zcoeffs[6] = 231.0f / (n + 7) - 315.0f / (n + 5) + 105.0f / (n + 3) - 5.0f / (n + 1);

	// apply norm constants
	zcoeffs[0] *= twoPi * kZH_Y_0;
	zcoeffs[1] *= twoPi * kZH_Y_1;
	zcoeffs[2] *= twoPi * kZH_Y_2;
	zcoeffs[3] *= twoPi * kZH_Y_3;
	zcoeffs[4] *= twoPi * kZH_Y_4;
	zcoeffs[5] *= twoPi * kZH_Y_5;
	zcoeffs[6] *= twoPi * kZH_Y_6;

	// [0]: 2pi sqrtf( 1 / (   4 * vl_pi)) / 1
	// we'll multiply by alpha = sqrtf(4.0f * vl_pi / (2 * i + 1)) in convolution, leaving 2pi.
}

template<class T>
void convolveZH7WithZH7Norm(const float brdfCoeffs[7], const T zhCoeffsIn[7], T zhCoeffsOut[7]) {
	zhCoeffsOut[0] = zhCoeffsIn[0];
	for(auto i = 1u; i < 7; i++) {
		float invAlpha = sqrtf(2.0f * i + 1);
		zhCoeffsOut[i] = (brdfCoeffs[i] / (invAlpha * brdfCoeffs[0])) * zhCoeffsIn[i];
	}
}

template<class T>
void addZH7Sample(float z, T c, T zhCoeffs[7]) {
	float z2 = z * z;
	float z3 = z2 * z;
	float z4 = z2 * z2;
	float z5 = z2 * z3;
	float z6 = z3 * z3;

	zhCoeffs[0] += kZH_Y_0 * c;
	zhCoeffs[1] += kZH_Y_1 * z * c;
	zhCoeffs[2] += kZH_Y_2 * (3 * z2 - 1) * c;
	zhCoeffs[3] += kZH_Y_3 * (5 * z3 - 3 * z) * c;
	zhCoeffs[4] += kZH_Y_4 * (35 * z4 - 30 * z2 + 3) * c;
	zhCoeffs[5] += kZH_Y_5 * (63 * z5 - 70 * z3 + 15 * z) * c;
	zhCoeffs[6] += kZH_Y_6 * (231 * z6 - 315 * z4 + 105 * z2 - 5) * c;
}

template<class T>
T evalZH7(float z, const T zhCoeffs[7]) {
	float z2 = z * z;
	float z3 = z2 * z;
	float z4 = z2 * z2;
	float z5 = z2 * z3;
	float z6 = z3 * z3;

	T c;

	c  = kZH_Y_0 * zhCoeffs[0];
	c += kZH_Y_1 * z * zhCoeffs[1];
	c += kZH_Y_2 * (3 * z * z - 1) * zhCoeffs[2];
	c += kZH_Y_3 * (5 * z3 - 3 * z) * zhCoeffs[3];
	c += kZH_Y_4 * (35 * z4 - 30 * z2 + 3) * zhCoeffs[4];
	c += kZH_Y_5 * (63 * z5 - 70 * z3 + 15 * z) * zhCoeffs[5];
	c += kZH_Y_6 * (231 * z6 - 315 * z4 + 105 * z2 - 5) * zhCoeffs[6];

	return c;
}

// Windowing a la Peter-Pike Sloan
inline float windowScale(int n, float gamma) {
	float nt = float(n * (n + 1));
	return 1.0f / (1.0f + gamma * nt * nt);
}

template<class T>
void applyZH7Windowing(float gamma, T coeffs[7]) {
	for(auto i = 0u; i < 7; i++) {
		coeffs[i] *= windowScale(i, gamma);
	}
}

template<class T>
void findZH7FromThetaTable(unsigned tableSize, const T table[], T zhCoeffs[7]) {
	float dt = 1.0f / (tableSize - 1);
	float t = 0.0f;

	float w = twoPi * 2 * dt; // 2pi dz = 2pi 2 dt
	for(auto i = 0u; i < 7; i++) {
		zhCoeffs[i] = {};
	}

	for(auto i = 0u; i < tableSize; i++) {
		addZH7Sample(2 * t - 1, w * table[i], zhCoeffs);
		t += dt;
	}
}

template<class T>
void generateThetaTableFromZH7(const T zhCoeffs[7], unsigned tableSize, T table[]) {
	float dt = 1.0f / (tableSize - 1);
	float t = 0.0f;

	for(unsigned i = 0; i < tableSize; i++) {
		table[i] = evalZH7(2 * t - 1, zhCoeffs);
		t += dt;
	}
}

// remap cosGamma to concentrate table around the sun location
inline float mapGamma(float g) {
	return sqrt(0.5f * (1.0f - g));
}

inline float unmapGamma(float g) {
	return 1 - 2 * g * g;
}

template<class T>
void findZH7FromGammaTable(unsigned tableSize, const T table[], T zhCoeffs[7]) {
	float dg = 1.0f / (tableSize - 1);
	float g = 0.0f;

	float w = twoPi * 4 * dg; // 2pi dz = 2pi -4 g dg
	for(auto i = 0u; i < 7; i++) {
		zhCoeffs[i] = {};
	}

	for(auto i = 0u; i < tableSize; i++) {
		addZH7Sample(unmapGamma(g), w * g * table[i], zhCoeffs);
		g += dg;
	}
}

template<class T>
void generateGammaTableFromZH7(T zhCoeffs[7], unsigned tableSize, T table[]) {
	float dg = 1.0f / (tableSize - 1);
	float g = 0.0f;

	for(auto i = 0u; i < tableSize; i++) {
		table[i] = evalZH7(unmapGamma(g), zhCoeffs);
		g += dg;
	}
}

constexpr float kThetaW = 0.01f;  // windowing gamma to use for theta table
constexpr float kGammaW = 0.002f; // windowing gamma to use for gamma table

constexpr float kThetaWHosek = 0.05f;  // windowing gamma to use for Hosek theta table
constexpr float kGammaWHosek = 0.005f; // windowing gamma to use for gamma table

constexpr float kRowPowers[Table::numLevels - 1] =
	{1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 96.0f};

std::array<std::array<Vec3f, Table::numEntries>, 2> thetaGammaTables(
		const Configuration& config) {
	std::array<std::array<Vec3f, Table::numEntries>, 2> res;
	auto& thetas = res[0];
	auto& gammas = res[1];

	float dt = 1.0f / (Table::numEntries - 1);
    float t = 0.0f;

    for(auto i = 0u; i < Table::numEntries; i++) {
        float cosTheta = t;
        float cosGamma = unmapGamma(t);
        float gamma = std::acos(cosGamma);
        float rayM = cosGamma * cosGamma;

        for(auto j = 0u; j < 3; j++) {
			auto& coeffs = config.coeffs[j];
            thetas[i][j] = -coeffs[0] * exp(coeffs[1] / (cosTheta + 0.01f));

            float expM = exp(coeffs[4] * gamma);
			float d = pow((1.0f + coeffs[8] * coeffs[8] - 2.0f * coeffs[8] * cosGamma), 1.5f);
            float mieM = (1.0f + rayM) / d;

            gammas[i][j] = coeffs[3] * expM + coeffs[5] * rayM + coeffs[6] * mieM;
        }

        t += dt;
    }

	return res;
}

} // anon namespace

Configuration bakeConfiguration(float turbidity, Vec3f ground, Vec3f toSun) {
	float elevation = std::asin(toSun.y);
	float ce = std::max(elevation, 0.f);

	Configuration res;
	res.radiance.x = findHosekCoeffs(datasetXYZ1, datasetXYZRad1, turbidity, ground.x, ce, res.coeffs[0]);
	res.radiance.y = findHosekCoeffs(datasetXYZ2, datasetXYZRad2, turbidity, ground.y, ce, res.coeffs[1]);
	res.radiance.z = findHosekCoeffs(datasetXYZ3, datasetXYZRad3, turbidity, ground.z, ce, res.coeffs[2]);

	res.radiance *= fullLumEfficacy; // assume full luminance efficacy
	if(toSun.y < 0.0) {
		constexpr auto duskSpeed = 50.f;
		auto s = std::max(1.f + duskSpeed * toSun.y, 0.f);
		auto is = 1.f - s;

		// Emulate Preetham darkening
		// Take C/E/F which control sun term to zero
        for(auto& coeff : res.coeffs) {
            coeff[3] *= s;
            coeff[5] *= s;
            coeff[6] *= s;

            // Take horizon term H to zero, as it's an orange glow at this point
            coeff[7] *= s;

            // Take I term back to 1
            coeff[2] *= s;
            coeff[2] += is;
        }

		float czl = zenithLuminance(std::acos(toSun.y), turbidity);
		float mzl = zenithLuminance(halfPi, turbidity);
		res.radiance *= czl / mzl;
	}

	return res;
}

Vec3f eval(const Configuration& config, float cosTheta, float gamma, float cosGamma) {
	return nytl::vec::cw::multiply(config.radiance, Vec3f{
		evalHosekCoeffs(config.coeffs[0], cosTheta, gamma, cosGamma),
		evalHosekCoeffs(config.coeffs[1], cosTheta, gamma, cosGamma),
		evalHosekCoeffs(config.coeffs[2], cosTheta, gamma, cosGamma),
	});
}

Table generateTable(const Sky& sky) {
	constexpr auto tableSize = Table::numEntries;
	auto [thetas, gammas] = thetaGammaTables(sky.config);
	Table res;

	// The BRDF tables cover the entire sphere, so we must resample theta from
	// the Perez/Hosek tables which cover a hemisphere.
    Vec3f thetaTable[Table::numEntries];

    // Fill top hemisphere of table
    for(auto i = 0u; i < tableSize / 2; i++) {
        thetaTable[tableSize / 2 + i] = thetas[2 * i];
	}

    // Fill lower hemisphere with term that evaluates close to 0,
	// to avoid below-ground luminance leaking in.
    // TODO: modify this using ground albedo
    float lowerHemi = 0.999f;  // 0.999 as theta table is used as (1 - F)
    for(auto i = 0u; i < tableSize / 2; i++) {
        thetaTable[i] = {lowerHemi, lowerHemi, lowerHemi};
	}

    // Project tables into ZH coefficients
    Vec3f zhCoeffsTheta[7];
    Vec3f zhCoeffsGamma[7];

    // theta table works better if we operate on something proportional to real luminance
    Vec3f biasedThetaTable[tableSize];
    for(auto i = 0u; i < tableSize; i++) {
        biasedThetaTable[i] = thetaTable[i] + Vec3f{1.f, 1.f, 1.f};
	}

    findZH7FromThetaTable(Table::numEntries, biasedThetaTable, zhCoeffsTheta);
    findZH7FromGammaTable(Table::numEntries, gammas.data(), zhCoeffsGamma);

    // row 0 is the original unconvolved signal
    // Firstly, fill -ve z with reflected +ve z, ramped to 0 at z = -1.
	// This avoids discontinuities at the horizon.
    for(auto i = 0u; i < tableSize / 2; i++) {
        thetaTable[i] = thetas[tableSize - 1 - 2 * i];

        // Ramp luminance down
        float s = (i + 0.5f) / (tableSize / 2);
        s = sqrt(s);
        thetaTable[i] = s * thetaTable[i] + Vec3f{1 - s, 1 - s, 1 - s};
    }

    for(auto i = 0u; i < tableSize; i++) {
        res.f[0][i] = thetaTable[i];
        res.g[0][i] = gammas[i];
    }

    // Construct H term table -- just the zenith part as we can potentially store as 4th component
    for(auto i = 0u; i < tableSize / 2; i++) {
        res.h[0][i] = 0.f;
	}

    for(auto i = 0u; i < tableSize / 2; i++) {
        float cosTheta = i / float(tableSize / 2 - 1); // XXX
        float zenith = sqrtf(cosTheta);

        res.h[0][tableSize / 2 + i] = zenith;
    }

    // Calculate FH as we get slightly better results convolving the full term
    // rather than approximating, at the cost of an extra table
    for(auto i = 0u; i < tableSize; i++) {
        res.fh[0][i] = res.h[0][i] * thetaTable[i];
	}

    float zhCoeffsH[7];
    findZH7FromThetaTable(tableSize, res.h[0], zhCoeffsH);

    Vec3f zhCoeffsFH[7];
    findZH7FromThetaTable(tableSize, res.fh[0], zhCoeffsFH);

    // rows 1..n-1 are successive convolutions
    for(auto r = 1u; r < Table::numLevels; r++) {
        int rs = Table::numLevels - r - 1;
        float s = kRowPowers[rs];

        float csCoeffs[7];
        calcCosPowerSatZH7(s, csCoeffs);

        Vec3f zhCoeffsThetaConv[7];
        convolveZH7WithZH7Norm(csCoeffs, zhCoeffsTheta, zhCoeffsThetaConv);
        Vec3f zhCoeffsGammaConv[7];
        convolveZH7WithZH7Norm(csCoeffs, zhCoeffsGamma, zhCoeffsGammaConv);

        float zhCoeffsThetaConvH [7];
        convolveZH7WithZH7Norm(csCoeffs, zhCoeffsH , zhCoeffsThetaConvH);
        Vec3f zhCoeffsThetaConvFH[7];
        convolveZH7WithZH7Norm(csCoeffs, zhCoeffsFH, zhCoeffsThetaConvFH);

        // Scale up to full windowing at full spec power...
        float rw = sqrtf(rs / float(Table::numLevels - 2));

        applyZH7Windowing(kThetaWHosek * rw, zhCoeffsThetaConv);
        applyZH7Windowing(kGammaWHosek * rw, zhCoeffsGammaConv);
        applyZH7Windowing(kThetaWHosek * rw, zhCoeffsThetaConvH);
        applyZH7Windowing(kThetaWHosek * rw, zhCoeffsThetaConvFH);

        // Generate convolved tables from ZH
        generateThetaTableFromZH7(zhCoeffsThetaConv,   tableSize, res.f[r]);
        generateGammaTableFromZH7(zhCoeffsGammaConv,   tableSize, res.g[r]);
        generateThetaTableFromZH7(zhCoeffsThetaConvH,  tableSize, res.h[r]);
        generateThetaTableFromZH7(zhCoeffsThetaConvFH, tableSize, res.fh[r]);

        for(auto i = 0u; i < tableSize; i++) {
            res.f[r][i] -= Vec3f{1.f, 1.f, 1.f};

        #ifdef HOSEK_G_FIX
            // Ringing on the Hosek G term leads to blue spots opposite the sun
            // in sunset situatons. Trying to solve this completely via windowing
            // leads to excessive blurring, so we also compensate by scaling down
            // the far pole in this situation.
            // TODO: investigate further, not sure this is worth it.
            float g = i / float(kTableSize - 1);
            float cosGamma = UnmapGamma(g);
            if (cosGamma < -0.6f)
                mBRDFGammaTable[r][i] *= ClampUnit(1.0f - sqr(-0.6f - cosGamma) * 1.5f * rw);
        #endif
        }
    }

	return res;
}

Vec3f eval(const Sky& sky, const Table& table, Vec3f dir, float r) {
	using namespace nytl::vec::cw;

	float cosTheta = dir.y;
    float cosGamma = dot(sky.toSun, dir);

    float t = 0.5f * (cosTheta + 1);
    float g = mapGamma(cosGamma);

	auto& coeffs = sky.config.coeffs;
	Vec3f cH{coeffs[0][7], coeffs[1][7], coeffs[2][7]};
    Vec3f cI{coeffs[0][2] - 1.0f, coeffs[1][2] - 1.0f, coeffs[2][2] - 1.0f};

    Vec3f F = bilerp(t, r, table.numEntries, table.numLevels, *table.f);
    Vec3f G = bilerp(g, r, table.numEntries, table.numLevels, *table.g);
    Vec3f H = bilerp(t, r, table.numEntries, table.numLevels, *table.h) * cH;
    Vec3f FH = bilerp(t, r, table.numEntries, table.numLevels, *table.fh);
	FH = multiply(FH, cH);

	H += cI;
	FH += multiply(F, cI);

	auto one = Vec3f{1.0, 1.0, 1.0};
	auto xyz = multiply((one - F), (one + G)) + H - FH;
	return multiply(sky.config.radiance, max(xyz, 0.f));
}

// TODO: these two functions seem to produce incorrect/unplausible
// results. While sun irradiance (low turbidity, high elevation)
// can be expected to be up to (or even more than) 100k, the rgb
// components returned are more like ~20k here. Not sure where the
// error is, I guess the data uses other units/is incorrect for
// a general purpose.
// See https://github.com/andrewwillmott/sun-sky/issues/2

/*
// From Preetham. Was used by Hosek as sun source, so can be used in conjunction
// with either.
const Vec3f sunRadianceLUT[16][16] = {
	{{39.4028, 1.98004, 5.96046e-08}, {68821.4, 29221.3, 3969.28}, {189745, 116333, 43283.4}, {284101, 199843, 103207}, {351488, 265139, 161944}, {400584, 315075, 213163}, {437555, 353806, 256435}, {466261, 384480, 292823}, {489140, 409270, 323569}, {507776, 429675, 349757}, {523235, 446739, 372260}, {536260, 461207, 391767}, {547379, 473621, 408815}, {556978, 484385, 423827}, {565348, 493805, 437137}, {572701, 502106, 449002}},
	{{34.9717, 0.0775114, 0}, {33531, 11971.9, 875.627}, {127295, 71095, 22201.3}, {216301, 142827, 66113.9}, {285954, 205687, 115900}, {339388, 256990, 163080}, {380973, 298478, 205124}, {414008, 332299, 241816}, {440780, 360220, 273675}, {462869, 383578, 301382}, {481379, 403364, 325586}, {497102, 420314, 346848}, {510615, 434983, 365635}, {522348, 447795, 382333}, {532628, 459074, 397255}, {541698, 469067, 410647}},
	{{10.0422, 0, 0.318865}, {16312.8, 4886.47, 84.98}, {85310.4, 43421.5, 11226.2}, {164586, 102046, 42200.5}, {232559, 159531, 82822.4}, {287476, 209581, 124663}, {331656, 251771, 163999}, {367569, 287173, 199628}, {397168, 317025, 231420}, {421906, 342405, 259652}, {442848, 364181, 284724}, {460784, 383030, 307045}, {476303, 399483, 326987}, {489856, 413955, 344876}, {501789, 426774, 360988}, {512360, 438191, 375548} },
	{{2.3477, 5.96046e-08, 0.129991}, {117.185, 30.0648, 0}, {57123.3, 26502.1, 5565.4}, {125170, 72886.2, 26819.8}, {189071, 123708, 59081.9}, {243452, 170892, 95209.2}, {288680, 212350, 131047}, {326303, 248153, 164740}, {357842, 278989, 195638}, {384544, 305634, 223657}, {407381, 328788, 248954}, {427101, 349038, 271779}, {444282, 366866, 292397}, {459372, 382660, 311064}, {472723, 396734, 328012}, {484602, 409337, 343430}},
	{{0.383395, 0, 0.027703}, {58.0534, 12.8383, 0}, {38221.6, 16163.6, 2681.55}, {95147.4, 52043, 16954.8}, {153669, 95910.9, 42062}, {206127, 139327, 72640.8}, {251236, 179082, 104653}, {289639, 214417, 135896}, {322383, 245500, 165343}, {350467, 272796, 192613}, {374734, 296820, 217644}, {395864, 318050, 240533}, {414400, 336900, 261440}, {430773, 353719, 280544}, {445330, 368800, 298027}, {458337, 382374, 314041}},
	{{0.0560895, 0, 0.00474608}, {44.0061, 8.32402, 0}, {25559, 9849.99, 1237.01}, {72294.8, 37148.7, 10649}, {124859, 74345.6, 29875.8}, {174489, 113576, 55359.1}, {218617, 151011, 83520.3}, {257067, 185252, 112054}, {290413, 216016, 139698}, {319390, 243473, 165842}, {344686, 267948, 190241}, {366896, 289801, 212852}, {386513, 309371, 233736}, {403942, 326957, 252998}, {419513, 342823, 270764}, {433487, 357178, 287149}},
	{{0.00811136, 0, 0.000761211}, {38.0318, 6.09287, 0}, {17083.4, 5996.83, 530.476}, {54909.7, 26508.7, 6634.5}, {101423, 57618.7, 21163.3}, {147679, 92573, 42135.2}, {190207, 127327, 66606.4}, {228134, 160042, 92352.6}, {261593, 190061, 117993}, {291049, 217290, 142758}, {317031, 241874, 166258}, {340033, 264051, 188331}, {360490, 284081, 208945}, {378771, 302212, 228135}, {395184, 318667, 245976}, {409974, 333634, 262543}},
	{{0.00118321, 0, 0.000119328}, {34.5228, 4.62524, 0}, {11414.1, 3646.94, 196.889}, {41690.9, 18909.8, 4091.39}, {82364.6, 44646.9, 14944.8}, {124966, 75444.4, 32024.3}, {165467, 107347, 53075.4}, {202437, 138252, 76076.7}, {235615, 167214, 99627}, {265208, 193912, 122858}, {291580, 218327, 145272}, {315124, 240580, 166611}, {336208, 260851, 186761}, {355158, 279331, 205696}, {372256, 296206, 223440}, {387729, 311636, 240030}},
	{{0.000174701, 0, 1.84774e-05}, {31.4054, 3.4608, 0}, {7624.24, 2215.02, 48.0059}, {31644.8, 13484.4, 2490.1}, {66872.4, 34589.1, 10515}, {105728, 61477.4, 24300.5}, {143926, 90494.6, 42256.1}, {179617, 119420, 62635.3}, {212200, 147105, 84088.4}, {241645, 173041, 105704}, {268159, 197064, 126911}, {292028, 219187, 147374}, {313550, 239512, 166913}, {333008, 258175, 185447}, {350650, 275321, 202953}, {366683, 291081, 219433}},
	{{2.61664e-05, 0, 2.86102e-06}, {27.3995, 2.42835, 5.96046e-08}, {391.889, 104.066, 0}, {24013.1, 9611.97, 1489.37}, {54282.4, 26792.1, 7366.53}, {89437, 50090, 18406.3}, {125174, 76280.7, 33609.8}, {159354, 103145, 51538.2}, {191098, 129407, 70945.4}, {220163, 154409, 90919.4}, {246607, 177864, 110847}, {270613, 199690, 130337}, {292410, 219912, 149156}, {312229, 238614, 167173}, {330289, 255902, 184328}, {346771, 271876, 200589}},
	{{3.93391e-06, 0, 4.76837e-07}, {21.8815, 1.51091, 0}, {106.645, 26.2423, 0}, {18217.8, 6848.77, 869.811}, {44054, 20748.7, 5134.5}, {75644.5, 40807, 13913.2}, {108852, 64293.6, 26704.2}, {141364, 89082.8, 42380.1}, {172081, 113831, 59831.4}, {200579, 137777, 78179.7}, {226776, 160529, 96794.7}, {250759, 181920, 115250}, {272686, 201910, 133270}, {292739, 220530, 150685}, {311103, 237847, 167398}, {327934, 253933, 183349}},
	{{6.55651e-07, 0, 1.19209e-07}, {15.4347, 0.791314, 0}, {67.98, 15.4685, 0}, {13818.5, 4877.71, 490.832}, {35746.5, 16065.3, 3556.94}, {63969.8, 33240.3, 10492.5}, {94648, 54185.5, 21192.5}, {125394, 76932.4, 34825.1}, {154946, 100125, 50435.6}, {182726, 122930, 67203.7}, {208530, 144877, 84504.4}, {232352, 165726, 101891}, {254283, 185376, 119059}, {274458, 203811, 135807}, {293024, 221062, 152009}, {310113, 237169, 167579}},
	{{5.96046e-08, 0, 0}, {9.57723, 0.336247, 0}, {52.9113, 11.1074, 0}, {10479.8, 3472.19, 262.637}, {29000.9, 12436.5, 2445.87}, {54089.5, 27073.4, 7891.84}, {82288.3, 45662.7, 16796.5}, {111218, 66434.7, 28595.3}, {139508, 88064, 42494.5}, {166453, 109678, 57749.2}, {191743, 130747, 73756.6}, {215288, 150968, 90064.3}, {237114, 170191, 106348}, {257311, 188355, 122384}, {275989, 205455, 138022}, {293255, 221507, 153152} },
	{{0, 0, 0}, {5.37425, 0.109694, 0}, {44.9811, 8.68891, 5.96046e-08}, {7946.76, 2470.32, 128.128}, {23524.7, 9625.27, 1666.58}, {45729.5, 22047.9, 5917.85}, {71535.2, 38477.1, 13293.2}, {98636.4, 57365.7, 23460.6}, {125598, 77452, 35785}, {151620, 97851, 49607}, {176299, 117990, 64359}, {199469, 137520, 79594.4}, {221098, 156245, 94979.6}, {241228, 174066, 110274}, {259937, 190947, 125309}, {277307, 206875, 139956}},
	{{0, 0, 0}, {2.83079, 0.0199037, 0}, {40.0718, 7.10214, 0}, {6025.35, 1756.45, 51.1916}, {19080.1, 7447.79, 1122.67}, {38657, 17952.9, 4422.16}, {62181.1, 32419.5, 10503.8}, {87471.2, 49531.4, 19230.6}, {113069, 68115.1, 30117.9}, {138102, 87295.1, 42596.4}, {162092, 106474, 56143.2}, {184805, 125266, 70327.1}, {206156, 143438, 84812.9}, {226144, 160857, 99349.8}, {244814, 177459, 113755}, {262220, 193206, 127887}},
	{{0, 0, 0}, {1.43779, 0, 0.00738072}, {36.6245, 5.93644, 0}, {4568.17, 1248.02, 9.13028}, {15473.4, 5761.51, 745.266}, {32674.7, 14616.6, 3291.16}, {54045.1, 27313.1, 8284.85}, {77563.8, 42764.4, 15747.9}, {101783, 59900.8, 25332.8}, {125782, 77874.7, 36561.6}, {149022, 96078.4, 48962}, {171213, 114101, 62125.3}, {192218, 131678, 75721.7}, {211998, 148648, 89495.8}, {230564, 164920, 103255}, {247950, 180437, 116847}}
};

Vec3f sunRadianceRGB(float cosTheta, float turbidity) {
    if(cosTheta < 0.0f) {
        return {0.f, 0.f, 0.f};
	}

    float s = cosTheta;
    float t = (turbidity - 2.0f) / 10.0f; // useful range is 2-12
    Vec3f sun = bilerp(s, t, 16, 16, *sunRadianceLUT);

    // convert from watts to candela at 540e12 hz.
	// Really, we should be weighting by luminous efficiency curve
    sun *= fullLumEfficacy;
    return sun;
}

Vec3f sunIrradianceRGB(float cosTheta, float turbidity) {
	// Calculate the solid angle of the sun from earth.
	// Good explanation at https://math.stackexchange.com/questions/73238
	constexpr float sunDiameter = 1.392f;
	constexpr float sunDistance = 149.6f;
	constexpr float angularRadius = 0.5f * sunDiameter / sunDistance;
	// cos(asin(x)) == sqrt(1 - x * x)
	const float sunCosAngle = sqrt(1.0f - angularRadius * angularRadius); // = 0.999989
	const float sunSolidAngle = twoPi * (1.0f - sunCosAngle); // = 6.79998e-05 steradians

    return sunSolidAngle * sunRadianceRGB(cosTheta, turbidity);
}
*/

} // namespace tkn::hosekSky
