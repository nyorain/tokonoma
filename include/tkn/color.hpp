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

constexpr auto matXYZToSRGB = nytl::Mat3f {
	+3.2404542, -1.5371385, -0.4985314,
	-0.9692660, +1.8760108, +0.0415560,
	+0.0556434, -0.2040259, +1.0572252,
};

constexpr auto matSRGBToXYZ = nytl::Mat3f {
	0.4124564, 0.3575761, 0.1804375,
 	0.2126729, 0.7151522, 0.0721750,
 	0.0193339, 0.1191920, 0.9503041,
};

[[nodiscard]] constexpr nytl::Vec3f XYZtoSRGB(nytl::Vec3f xyz) {
	return matXYZToSRGB * xyz;
}

[[nodiscard]] constexpr nytl::Vec3f SRGBtoXYZ(nytl::Vec3f rgb) {
	return matSRGBToXYZ * rgb;
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

	// Conversion from RGB to a spectrum cannot be solved uniquely.
	// Just tries its best to deliver a smooth spectrum.
	// Will clamp the result to positive values.
	[[nodiscard]] static SpectralColor fromSRGBIllum(const nytl::Vec3f&);
	[[nodiscard]] static SpectralColor fromSRGBRefl(const nytl::Vec3f&);

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
