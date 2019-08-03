#pragma once

// Allows to code as glsl like as possible
#include <nytl/vecOps.hpp>
#include <nytl/matOps.hpp>
#include <nytl/math.hpp>

namespace tkn::glsl {

// TODO: with modification to nytl smoothstep and mix
// (additional template parameter for factor).
// Some overload probably still missing

using nytl::Vec;
using nytl::radians;
using nytl::degrees;
using nytl::mix;
using nytl::smoothstep;
using nytl::dot;
using nytl::cross;
using nytl::length;
using nytl::normalize;
using nytl::distance;

using std::sin;
using std::cos;
using std::tan;
using std::asin;
using std::acos;
using std::atan;
using std::pow;
using std::exp;
using std::log;
using std::exp2;
using std::log2;
using std::sqrt;
using std::abs;
using std::ceil;
using std::floor;
using std::min;
using std::max;
using std::clamp;

using namespace nytl::vec::operators;
using namespace nytl::vec::cw;
using namespace nytl::vec::cw::operators;

using vec2 = nytl::Vec2f;
using vec3 = nytl::Vec3f;
using vec4 = nytl::Vec3f;

using mat2 = nytl::Mat2f;
using mat3 = nytl::Mat3f;
using mat4 = nytl::Mat4f;

float inversesqrt(float x) {
	return 1 / sqrt(x);
}

float sign(float x) {
	return (x > 0) ? 1.f : (x < 0) ? -1.f : 0.f;
}

float fract(float x) {
	return x - floor(x);
}

float step(float edge, float x) {
	return x < edge ? 0.0 : 1.0;
}

float length(float x) {
	return x;
}

float distance(float a, float b) {
	return abs(a - b);
}

float normalize(float) {
	return 1.0;
}

float mod(float x, float y) {
	return std::fmod(x, y);
}

template<size_t D, typename T>
constexpr auto min(Vec<D, T> a, const T& val) {
	for(auto& v : a) v = min(v, val);
	return a;
}

template<size_t D, typename T>
constexpr auto max(Vec<D, T> a, const T& val) {
	for(auto& v : a) v = max(v, val);
	return a;
}

template<size_t D, typename T>
constexpr auto step(const T& edge, Vec<D, T> x) {
	for(auto& v : x) v = step(edge, v);
	return x;
}

template<size_t D, typename T>
constexpr auto step(const Vec<D, T>& edge, Vec<D, T> x) {
	for(auto i = 0u; i < D; ++i)
		x[i] = step(edge[i], x[i]);
	return x;
}

template<size_t D, typename T>
constexpr auto mod(Vec<D, T> x, const Vec<D, T>& y) {
	for(auto i = 0u; i < D; ++i)
		x[i] = std::fmod(x[i], y[i]);
	return x;
}

template<size_t D, typename T>
constexpr auto mod(Vec<D, T> x, const T& y) {
	for(auto i = 0u; i < D; ++i)
		x[i] = std::fmod(x[i], y);
	return x;
}

#define TKN_VEC_UTIL_FUNC(func) \
	template<size_t D, typename T> \
	constexpr Vec<D, T> func(Vec<D, T> vec) { \
		for(auto& val : vec) val = func(val); \
		return vec; \
	}

TKN_VEC_UTIL_FUNC(inversesqrt);
TKN_VEC_UTIL_FUNC(sign);
TKN_VEC_UTIL_FUNC(fract);

#undef TKN_VEC_UTIL_FUNC

// template<size_t D, typename T>
// Vec<D, T> inversesqrt(Vec<D, T> x) {
// 	for(auto& v : x) v = inversesqrt(v);
// 	return x;
// }
//

// template<size_t D, typename T>
// Vec<D, T> sign(Vec<D, T> x) {
// 	for(auto& v : x) v = sign(v);
// 	return x;
// }

// template<size_t D, typename T>
// Vec<D, T> fract(Vec<D, T> x) {
// 	for(auto& v : x) v = fract(v);
// 	return x;
// }

} // namespace tkn::glsl
