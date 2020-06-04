#pragma once

#include <array>
#include <vector>
#include <tkn/types.hpp>

// Utility for the hosek-wilkie sky model. The model was modified
// to use a preetham-like zenith darkening and transitions to black
// when the sky is below the horizon (the original model assumed
// the sun above the horizon).
// Can evaluate the model or bake tables that allow fast evaluation
// on the gpu with just a single texture sample.
// Unless otherwise specified, all color values are XYZ coordinates.

namespace tkn::hosekSky {
constexpr auto up = Vec3f{0, 1, 0};

struct Configuration {
	std::array<std::array<float, 9>, 3> coeffs;
	Vec3f radiance; // overall average radiance
};

struct TableEntry {
	Vec3f f;
	Vec3f g;
	float h;
	Vec3f fh;
};

struct Table {
	static constexpr auto numLevels = 8u;
	static constexpr auto numEntries = 64u;

	Vec3f f[numLevels][numEntries];
	Vec3f g[numLevels][numEntries];
	float h[numLevels][numEntries];
	Vec3f fh[numLevels][numEntries];
};

struct Sky {
	Configuration config;
	Vec3f groundAlbedo;
	float turbidity;
	Vec3f toSun;
};

Configuration bakeConfiguration(float turbidity, Vec3f ground, Vec3f toSun);
Vec3f eval(const Configuration&, float cosTheta, float gamma, float cosGamma);

Table generateTable(const Sky& sky);
Vec3f eval(const Sky& sky, const Table&, Vec3f dir, float roughness);

// Vec3f sunRadianceRGB(float cosTheta, float turbidity);
// Vec3f sunIrradianceRGB(float cosTheta, float turbidity);

} // namespace tkn
