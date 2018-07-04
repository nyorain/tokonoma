#pragma once

#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/matOps.hpp>
#include <cmath>

// NOTE: move to nytl when somewhat tested
// codes comes originally from older nytl versions

namespace doi {

/// Returns a matix that scales by s.
template<typename P = float>
nytl::Mat4<P> scaleMat(const nytl::Vec<3, P>& s) {
	auto mat = nytl::identity<4, float>();
	for(std::size_t i(0); i < 3; ++i) {
		mat[i][i] *= s[i];
	}

	return mat;
}

/// Returns a matix that translates by t.
template<typename P = float>
nytl::Mat4<P> translateMat(const nytl::Vec<3, P>& t) {
	auto mat = nytl::identity<4, float>();
	for(std::size_t i(0); i < 3; ++i) {
		mat[i][3] = t[i];
	}

	return mat;
}

/// Returns a matrix that rotates by rot (in 2 dimensions).
template<size_t D = 4, typename P = float>
nytl::SquareMat<D, P> rotate(P rot) {
	auto mat = nytl::identity<D, P>();

	auto c = std::cos(rot);
	auto s = std::sin(rot);

	mat[0][0] = c;
	mat[0][1] = -s;
	mat[1][0] = s;
	mat[1][1] = c;

	return mat;
}

/// Returns a matrix that rotates by the given angle (in radians)
/// around the given vector.
template<size_t D = 4, typename P = float>
nytl::SquareMat<D, P> rotate(const nytl::Vec3<P>& vec, P angle) {
	const P c = std::cos(angle);
	const P s = std::sin(angle);

	nytl::Vec3<P> axis = normalize(vec);
	nytl::Vec3<P> temp = (P(1) - c) * axis;

	auto mat = nytl::identity<D, P>();
	mat[0][0] = c + temp[0] * axis[0];
	mat[0][1] = 0 + temp[0] * axis[1] + s * axis[2];
	mat[0][2] = 0 + temp[0] * axis[2] - s * axis[1];

	mat[1][0] = 0 + temp[1] * axis[0] - s * axis[2];
	mat[1][1] = c + temp[1] * axis[1];
	mat[1][2] = 0 + temp[1] * axis[2] + s * axis[0];

	mat[2][0] = 0 + temp[2] * axis[0] + s * axis[1];
	mat[2][1] = 0 + temp[2] * axis[1] - s * axis[0];
	mat[2][2] = c + temp[2] * axis[2];

	return mat;
}

template<typename P>
nytl::SquareMat<4, P> frustrum3(P left, P right, P top, P bottom,
		P pnear, P far) {

	auto ret = nytl::SquareMat<4, P>(0);

	ret[0][0] = (P(2) * pnear) / (right - left);
	ret[1][1] = (P(2) * pnear) / (top - bottom);
	ret[2][2] = -(far * pnear) / (far - pnear);

	ret[0][2] = (right + left) / (right - left);
	ret[1][2] = (top + bottom) / (top - bottom);
	ret[3][2] = -1;

	ret[2][3] = (P(-2) * far * pnear) / (far - pnear);
	return ret;
}

template<typename P>
nytl::SquareMat<4, P> frustum3Sym(P width, P height, P pnear, P pfar) {
	return perspective3(-width / P(2), width / P(2), height / P(2),
		-height / P(2), pnear, pfar);
}

template<typename P>
nytl::SquareMat<4, P> perspective3(P fov, P aspect, P pnear, P pfar) {
	P const f = P(1) / std::tan(fov / P(2));

	auto ret = nytl::Mat4<P>(0);
	ret[0][0] = f / aspect;
	ret[1][1] = f;

	ret[2][2] = -(pfar + pnear) / (pfar - pnear);
	ret[3][2] = -1;

	ret[2][3] = -(P(2) * pfar * pnear) / (pfar - pnear);
	return ret;
}


template<typename P>
nytl::SquareMat<4, P> ortho3(P left, P right, P top, P bottom, P pnear, P far) {
	auto ret = nytl::Mat4<P>(0);

	ret[0][0] = P(2) / (right - left);
	ret[1][1] = P(2) / (top - bottom);
	ret[2][2] = P(2) / (far - pnear);

	ret[0][3] = - ((right + left) / (right - left));
	ret[1][3] = - ((top + bottom) / (top - bottom));
	ret[2][3] = - ((far + pnear) / (far - pnear));
	ret[3][3] = 1;
	return ret;
}

template<typename P>
nytl::SquareMat<4, P> ortho3Sym(P width, P height, P pnear, P pfar) {
	return ortho3(-width / P(2), width / P(2), height / P(2),
		-height / P(2), pnear, pfar);
}

template<typename P>
nytl::SquareMat<4, P> lookAt(const nytl::Vec3<P>& eye,
		const nytl::Vec3<P>& center, const nytl::Vec3<P>& up) {

	const auto z = normalize(center - eye); //z
	const auto x = normalize(cross(z, up)); //x
	const auto y = cross(x, z); //y

	auto ret = nytl::identity<4, P>();

	ret.row(0) = x;
	ret.row(1) = y;
	ret.row(2) = -z;

	ret[0][3] = -dot(x, eye);
	ret[1][3] = -dot(y, eye);
	ret[2][3] = dot(z, eye);

	return ret;
}

} // namespace doi
