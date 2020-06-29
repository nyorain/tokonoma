#pragma once

#include <nytl/vecOps.hpp>
#include <nytl/matOps.hpp>

namespace tkn {

template<typename T>
auto det(const nytl::Mat2<T>& m) {
	return m[0][0] * m[1][1] - m[0][1] * m[1][0];
}

template<typename T>
auto det(const nytl::Mat3<T>& m) {
	// using R = decltype(m[0][0] * m[0][0]  - m[0][0] * m[0][0]);
	// auto res = R(0.0);

	// #1
	// for(auto i = 0u; i < 3; ++i) {
	// 	auto prod = R(1.0);
	// 	for(auto j = 0u; j < 3; ++j) {
	// 		prod *= m[j][(i + j) % 3];
	// 	}
	// 	res += prod;
	// 	prod = R(1.0);
	// 	for(auto j = 0u; j < 3; ++j) {
	// 		prod *= m[j][(i + 2 * j) % 3];
	// 	}
	// 	res -= prod;
	// }

	// #2
	// res += m[0][0] * m[1][1] * m[2][2];
	// res += m[0][1] * m[1][2] * m[2][0];
	// res += m[0][2] * m[1][0] * m[2][1];
	// res -= m[0][2] * m[1][1] * m[2][0];
	// res -= m[0][1] * m[1][0] * m[2][2];
	// res -= m[0][0] * m[1][2] * m[2][1];

	// return res;

	// #3
	return m[0][0] * (m[1][1] * m[2][2] - m[2][1] * m[1][2]) -
		m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

template<typename T>
auto inverse(const nytl::Mat2<T>& m) {
	auto t = nytl::Mat2<T>{
		m[0][0], -m[1][0],
		-m[0][1], m[1][1],
	};
	return (T(1.0) / det(m)) * t;
}

// https://stackoverflow.com/questions/983999/simple-3x3-matrix-inverse-code-c,
// https://mathworld.wolfram.com/images/equations/MatrixInverse/NumberedEquation4.gif
template<typename T>
auto inverseWithDet(const nytl::Mat3<T>& m, const T& det) {
	auto id = T(1.0) / det;
	return nytl::Mat3<T>{
		id * (m[1][1] * m[2][2] - m[1][2] * m[2][1]),
		id * (m[0][2] * m[2][1] - m[0][1] * m[2][2]),
		id * (m[0][1] * m[1][2] - m[0][2] * m[1][1]),

		id * (m[1][2] * m[2][0] - m[2][2] * m[1][0]),
		id * (m[0][0] * m[2][2] - m[0][2] * m[2][0]),
		id * (m[0][2] * m[1][0] - m[0][0] * m[1][2]),

		id * (m[1][0] * m[2][1] - m[1][1] * m[2][0]),
		id * (m[0][1] * m[2][0] - m[0][0] * m[2][1]),
		id * (m[0][0] * m[1][1] - m[0][1] * m[1][0]),
	};
}

template<typename T>
auto inverse(const nytl::Mat3<T>& m) {
	return inverseWithDet(m, det(m));
}

template<typename T>
auto inverseAndDet(const nytl::Mat3<T>& m, T& outDet) {
	outDet = det(m);
	return inverseWithDet(m, outDet);
}

} // namespace tkn
