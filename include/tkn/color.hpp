// Implementation contains code from pbrt-v3, licensed under BSD-2.
// See the implementation in color.cpp for full license text.
// https://github.com/mmp/pbrt-v3/blob/master/src/spectrum.cpp

#pragma once

#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/mat.hpp>
#include <limits>

// All wavelengths measured in angstrom (10^-10 meter)

// TODO: add spectrum-based blackbody function.
//   move approx rgb blackbody function here

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
struct SpectralColor : SampledSpectrum<nSpectralSamples> {
	static constexpr auto lambdaStart = 400u;
	static constexpr auto lambdaEnd = 700u;

	// Conversion from RGB (linear srgb) to a spectrum cannot be solved uniquely.
	// Just tries its best to deliver a smooth spectrum.
	// Will clamp the result to positive values.
	[[nodiscard]] static SpectralColor fromRGBIllum(const nytl::Vec3f&);
	[[nodiscard]] static SpectralColor fromRGBRefl(const nytl::Vec3f&);

	// Re-samples the given spectrum of 'n' samples given in 'vals',
	// with the associated wavelengths 'lambda'. The sample wavelengths
	// (i.e. lambda) must be sorted.
	[[nodiscard]] static SpectralColor fromSamples(const float* lambda,
		const float* vals, int n);

	// The i-th sample covers range [wavelength(i), wavelength(i + 1)]
	[[nodiscard]] static float wavelength(unsigned i) {
		return nytl::mix(lambdaStart, lambdaEnd, float(i) / nSpectralSamples);
	}
};

nytl::Vec3f toXYZ(const SpectralColor&);

// Given spectrum samples 'vals' and cooresponding wavelengths 'lambda',
// both arrays with size n, computes the average value between lambdaStart
// and lambdaEnd. Basically: the integral of the spectrum in the range,
// divided by (lambdaEnd - lambdaStart).
// The values in 'lambda' must be sorted and not have duplicates.
float spectrumAverage(const float* lambda, const float* vals, int n,
        float lambdaStart, float lambdaEnd);

} // namespace tkn
