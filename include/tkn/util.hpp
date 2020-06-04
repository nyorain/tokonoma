#pragma once

#include <nytl/stringParam.hpp>
#include <nytl/vec.hpp>
#include <nytl/math.hpp>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <cmath>

// file: general utility that hasn't found a better place yet

namespace tkn {

constexpr auto fullLumEfficacy = 683.f;
constexpr auto f16Scale = 0.00001f;

template<typename T>
bool stoi(nytl::StringParam string, T& val, unsigned base = 10) {
	char* end {};
	auto str = string.c_str();
	auto v = std::strtoll(str, &end, base);
	if(end == str) {
		return false;
	}

	val = v;
	return true;
}

// Expects: height, width > 0
// Uses clampToEdge
template<typename T>
T bilerp(float s, float t, unsigned width, unsigned height, T* vals) {
	s *= (width - 1);
	t *= (height - 1);

	unsigned s0 = std::floor(s);
	unsigned t0 = std::floor(t);

	unsigned x0 = std::clamp(s0, 0u, width - 1u);
	unsigned x1 = std::clamp(s0 + 1, 0u, width - 1u);
	unsigned y0 = std::clamp(t0, 0u, height - 1u);
	unsigned y1 = std::clamp(t0 + 1, 0u, height - 1u);

	float fx = s - s0;
	float fy = t - t0;
	return nytl::mix(
		nytl::mix(vals[y0 * width + x0], vals[y0 * width + x1], fx),
		nytl::mix(vals[y1 * width + x0], vals[y1 * width + x1], fx), fy);
}

inline bool has_suffix(std::string_view str, std::string_view suffix) {
    return str.size() >= suffix.size() &&
        str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Simple blackbody approxmiation.
/// Converts kelvin color temparature (1000K - 40000K) to a rbg color.
nytl::Vec3f blackbody(unsigned kelvin);

/// Sets/Unsets the given bit in the given bitfield.
template<typename T>
T bit(T value, T bit, bool set) {
	return set ? (value | bit) : (value & ~bit);
}

} // namespace tkn
