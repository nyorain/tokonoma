#pragma once

#include <array>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>

// sources:
// http://orlandoaguilar.github.io/sh/spherical/harmonics/irradiance/map/2017/02/12/SphericalHarmonics.html
// https://en.wikipedia.org/wiki/Table_of_spherical_harmonics
// Peter-Pike Sloan: Stupid Spherical Harmonics Tricks
//   http://www.ppsloan.org/publications/StupidSH36.pdf
// Robin Green: Spherical Harmonic Lighting: The Gritty Details
//   http://silviojemma.com/public/papers/lighting/spherical-harmonic-lighting.pdf

// NOTE: pretty much all visualizations of spherical harmonics
// are done with z-up convention, so they are not accurate for
// graphics conventions. Otherwise the convention used does not
// make a difference.

namespace tkn {

// Third order spherical harmonics, 9 coefficients.
template<typename T>
struct SH9 {
	nytl::Vec<9, T> coeffs;
};

template<typename T1, typename T2>
auto dot(const SH9<T1>& f1, const SH9<T2>& f2) {
	return nytl::dot(f1.coeffs, f2.coeffs);
}

// Evaluates each SH base function at the given direction.
// Couldn't figure out an intuitive meaning for the returned spherical
// harmonics (a function that peaks in the given direction and otherwise
// as low as possible?).
template<typename T = float, typename P>
SH9<T> projectSH9(const nytl::Vec3<P>& dir) {
	SH9<T> sh;

	// band 0
    sh.coeffs[0] = 0.282095f;

    // band 1
    sh.coeffs[1] = 0.488603f * dir.y;
    sh.coeffs[2] = 0.488603f * dir.z;
    sh.coeffs[3] = 0.488603f * dir.x;

    // band 2
    sh.coeffs[4] = 1.092548f * dir.x * dir.y;
    sh.coeffs[5] = 1.092548f * dir.y * dir.z;
    sh.coeffs[6] = 0.315392f * (3.0f * dir.z * dir.z - 1.0f);
    sh.coeffs[7] = 1.092548f * dir.x * dir.z;
    sh.coeffs[8] = 0.546274f * (dir.x * dir.x - dir.y * dir.y);

    return sh;
}

// Convolutes the given spherical harmonics with a cosine lobe.
template<typename T>
SH9<T> convoluteCosineLobe(SH9<T> sh) {
	using nytl::constants::pi;
	constexpr auto h0 = pi;
	constexpr auto h1 = 2.0 * pi / 3.0;
	constexpr auto h2 = pi / 4.0;

	sh.coeffs[0] *= h0;

	sh.coeffs[1] *= h1;
	sh.coeffs[2] *= h1;
	sh.coeffs[3] *= h1;

	sh.coeffs[4] *= h2;
	sh.coeffs[5] *= h2;
	sh.coeffs[6] *= h2;
	sh.coeffs[7] *= h2;
	sh.coeffs[8] *= h2;

	return sh;
}

// Evaluates the given spherical harmonics at the given direction.
template<typename T, typename P>
auto eval(const SH9<T>& sh, const nytl::Vec3<P>& dir) {
	return dot(sh, projectSH9<T>(dir));
}

template<typename T, typename P>
auto evalIrradiance(const SH9<T>& sh, const nytl::Vec3<P>& dir) {
	return dot(sh, convoluteCosineLobe(projectSH9<T>(dir)));
}

} // namespace tkn
