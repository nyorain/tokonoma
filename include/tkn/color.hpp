// Implementation contains code from pbrt-v3, licensed under BSD-2.
// See the implementation in color.cpp for full license text.
// https://github.com/mmp/pbrt-v3/blob/master/src/spectrum.cpp

#pragma once

#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/mat.hpp>
#include <limits>

// All wavelengths measured in nanometers (10^-9 meter)

namespace tkn {

// - https://en.wikipedia.org/wiki/SRGB
constexpr auto matXYZToRGB = nytl::Mat3f {
	+3.24096994, -1.53738318, -0.49861076,
	-0.96924364, +1.87596740, +0.04155506,
	+0.05563008, -0.20397696, +1.05697151
};

constexpr auto matRGBToXYZ = nytl::Mat3f {
	0.41239080, 0.35758434, 0.18048079,
 	0.21263901, 0.71516868, 0.07219232,
 	0.01933082, 0.11919478, 0.95053215,
};

// Converts from/to linear srgb.
[[nodiscard]] constexpr nytl::Vec3f XYZtoRGB(nytl::Vec3f xyz) {
	return matXYZToRGB * xyz;
}

[[nodiscard]] constexpr nytl::Vec3f RGBtoXYZ(nytl::Vec3f rgb) {
	return matRGBToXYZ * rgb;
}

// SampledSpectrum
template<std::size_t SampleCount, typename P = float>
struct SampledSpectrum {
	nytl::Vec<SampleCount, P> samples;

	SampledSpectrum& operator+=(const SampledSpectrum<SampleCount, P>& rhs) {
		samples += rhs.samples;
		return *this;
	}

	SampledSpectrum& operator-=(const SampledSpectrum<SampleCount, P>& rhs) {
		samples -= rhs.samples;
		return *this;
	}

	SampledSpectrum& operator*=(const P& fac) {
		samples *= fac;
		return *this;
	}
};

template<std::size_t S, typename P> [[nodiscard]]
SampledSpectrum<S, P> operator*(const P& f, SampledSpectrum<S, P> s) {
	return s *= f;
}

template<std::size_t S, typename P> [[nodiscard]]
SampledSpectrum<S, P> operator+(SampledSpectrum<S, P> a,
		const SampledSpectrum<S, P>& b) {
	return a += b;
}

template<std::size_t S, typename P> [[nodiscard]]
SampledSpectrum<S, P> operator-(SampledSpectrum<S, P> a,
		const SampledSpectrum<S, P>& b) {
	return a -= b;
}

template<std::size_t S, typename P>
void clamp(SampledSpectrum<S, P>& s, float low = 0.f,
		float high = std::numeric_limits<float>::infinity()) {
	nytl::vec::cw::ip::clamp(s.samples, low, high);
}

// SpectralColor
// Just a SamplesSpectrum specialization with float precision,
// a (more or less) arbitrarily chosen number of samples and wavelength range.
// The samples are equidistantly distributed over the chosen spectrum.
constexpr auto nSpectralSamples = 60;
struct SpectralColor : public SampledSpectrum<nSpectralSamples> {
	static constexpr auto nSamples = nSpectralSamples;
	static constexpr auto lambdaStart = 380;
	static constexpr auto lambdaEnd = 720;

	// Conversion from RGB (linear srgb) to a spectrum cannot be solved uniquely.
	// Just tries its best to deliver a smooth spectrum.
	// Will clamp the result to positive values.
	[[nodiscard]] static SpectralColor fromRGBIllum(const nytl::Vec3f&);
	[[nodiscard]] static SpectralColor fromRGBRefl(const nytl::Vec3f&);

	// Re-samples the given spectrum of 'n' samples given in 'vals',
	// with the associated wavelengths 'lambda'. The sample wavelengths
	// (i.e. lambda) must be sorted.
	[[nodiscard]] static SpectralColor fromSamples(const float* lambda,
		const float* vals, unsigned n);

	// As above, but instead of a separate array with wavelengths,
	// assumes n equidistant samples between lambdaMin and lambdaMax.
	[[nodiscard]] static SpectralColor fromSamples(const float* vals,
		unsigned n, float lambdaMin, float lambdaMax);

	// The i-th sample covers range [wavelength(i), wavelength(i + 1)]
	[[nodiscard]] static float wavelength(unsigned i) {
		return nytl::mix(lambdaStart, lambdaEnd, float(i) / nSpectralSamples);
	}
};

nytl::Vec3f toXYZ(const SpectralColor&);

float planck(float kelvin, float wavelength);
SpectralColor blackbody(float kevlin);

/// Simple blackbody approxmiation.
/// Converts kelvin color temparature (1000K - 40000K) to a rbg color.
/// Good enough for most purposes.
nytl::Vec3f blackbodyApproxRGB(float kelvin);

// Given spectrum samples 'vals' and cooresponding wavelengths 'lambda',
// both arrays with size n, computes the average value between lambdaStart
// and lambdaEnd. Basically: the integral of the spectrum in the range,
// divided by (lambdaEnd - lambdaStart).
// The values in 'lambda' must be sorted and not have duplicates.
float spectrumAverage(const float* lambda, const float* vals, unsigned n,
    float lambdaStart, float lambdaEnd);

// Equi-distant samples.
float spectrumAverage(const float* vals, unsigned n,
	float srcStart, float srcEnd,
    float avgStart, float avgEnd);

} // namespace tkn
