#pragma once

#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/matOps.hpp>
#include <cmath>

// NOTE: move to nytl when somewhat tested
// codes comes originally from older nytl versions

// TODO: consistent naming and lhs/rhs; zo/no conventions
// maybe use own namespaces that can just be used?
// made for vulkan. Update them!
// TODO: rotate version that only uses dot/cross

namespace tkn {

inline nytl::Vec3f multPos(const nytl::Mat4f& m, nytl::Vec3f v) {
	auto v4 = m * nytl::Vec4f{v.x, v.y, v.z, 1.f};
	return {v4[0] / v4[3], v4[1] / v4[3], v4[2] / v4[3]};
}

inline nytl::Vec3f multDir(const nytl::Mat4f& m, nytl::Vec3f v) {
	return static_cast<nytl::Mat3f>(m) * v;
}


/// Returns a matix that scales by s.
template<size_t D = 4, typename P = float, size_t R = D - 1>
nytl::SquareMat<D, P> scaleMat(const nytl::Vec<R, P>& s) {
	static_assert(R <= D);
	auto mat = nytl::identity<D, float>();
	for(std::size_t i(0); i < R; ++i) {
		mat[i][i] *= s[i];
	}

	return mat;
}

/// Returns a matix that translates by t.
template<size_t D = 4,typename P = float,  size_t R = D - 1>
nytl::SquareMat<D, P> translateMat(const nytl::Vec<R, P>& t) {
	static_assert(R <= D);
	auto mat = nytl::identity<D, float>();
	for(std::size_t i(0); i < R; ++i) {
		mat[i][D - 1] = t[i];
	}

	return mat;
}

/// Returns a matrix that rotates by rot (in 2 dimensions).
template<size_t D = 4, typename P = float>
nytl::SquareMat<D, P> rotateMat(P rot) {
	static_assert(D >= 2);
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
nytl::SquareMat<D, P> rotateMat(const nytl::Vec3<P>& vec, P angle) {
	static_assert(D >= 3);
	const P c = std::cos(angle);
	const P s = std::sin(angle);

	nytl::Vec3<P> axis = normalized(vec);
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

template<size_t D, typename P>
void rotate(nytl::SquareMat<D, P>& mat, P rot) {
	mat = rotateMat<D, P>(rot) * mat;
}

template<size_t D, typename P>
void rotate(nytl::SquareMat<D, P>& mat, const nytl::Vec3<P>& vec, P angle) {
	mat = rotateMat<D, P>(vec, angle) * mat;
}

template<size_t D, typename P, size_t R>
void translate(nytl::SquareMat<D, P>& mat, const nytl::Vec<R, P>& t) {
	mat = translateMat<D, P>(t) * mat;
}

template<size_t D, typename P, size_t R>
void scale(nytl::SquareMat<D, P>& mat, const nytl::Vec<R, P>& s) {
	mat = scaleMat<D, P>(s) * mat;
}

template<typename P>
nytl::SquareMat<4, P> frustrum3(P left, P right, P top, P bottom,
		P pnear, P far) {

	auto ret = nytl::identity<4, P>();

	ret[0][0] = (P(2) * pnear) / (right - left);
	ret[1][1] = (P(2) * pnear) / (top - bottom);
	ret[2][2] = (pnear + far) / (pnear - far);

	ret[0][2] = (right + left) / (right - left);
	ret[1][2] = (top + bottom) / (top - bottom);
	ret[3][2] = -1;

	ret[2][3] = (P(2) * far * pnear) / (pnear - far);
	return ret;
}

template<typename P>
nytl::SquareMat<4, P> frustum3Sym(P width, P height, P pnear, P pfar) {
	return frustrum3(-width / P(2), width / P(2), height / P(2),
		-height / P(2), pnear, pfar);
}

// lhs, zo
template<typename P>
nytl::SquareMat<4, P> perspective3LH(P fov, P aspect, P pnear, P pfar) {
	P const f = P(1) / std::tan(fov / P(2));

	auto ret = nytl::Mat4f {};
	ret[0][0] = f / aspect;
	ret[1][1] = f;

	ret[2][2] = pfar / (pfar - pnear);
	ret[3][2] = P(1);

	ret[2][3] = -(pfar * pnear) / (pfar - pnear);
	return ret;
}

// rhs, zo
template<typename P>
nytl::SquareMat<4, P> perspective3RH(P fov, P aspect, P pnear, P pfar) {
	P const f = P(1) / std::tan(fov / P(2));

	auto ret = nytl::Mat4f {};
	ret[0][0] = f / aspect;
	ret[1][1] = f;

	ret[2][2] = -pfar / (pfar - pnear);
	ret[3][2] = -P(1);

	ret[2][3] = -(pfar * pnear) / (pfar - pnear);
	return ret;
}


// rh_zo
// near and far both positive
template<typename P>
nytl::SquareMat<4, P> ortho3(P left, P right, P top, P bottom, P pnear, P far) {

	auto ret = nytl::Mat4f {};
	ret[0][0] = P(2) / (right - left);
	ret[1][1] = P(2) / (top - bottom);
	ret[2][2] = P(1) / (pnear - far);
	ret[3][3] = 1;

	ret[0][3] = (right + left) / (left - right);
	ret[1][3] = (top + bottom) / (bottom - top);
	ret[2][3] = pnear / (pnear - far);
	return ret;
}

template<typename P>
nytl::SquareMat<4, P> ortho3Sym(P width, P height, P pnear, P pfar) {
	return ortho3(-width / P(2), width / P(2), height / P(2),
		-height / P(2), pnear, pfar);
}

// for a left handed coordinate system
template<typename P>
nytl::SquareMat<4, P> lookAtLH(const nytl::Vec3<P>& eye,
		const nytl::Vec3<P>& center, const nytl::Vec3<P>& up) {

	const auto z = normalized(center - eye);
	const auto x = normalized(cross(up, z));
	const auto y = cross(z, x);

	auto ret = nytl::identity<4, P>();

	ret[0] = nytl::Vec4f(x);
	ret[1] = nytl::Vec4f(y);
	ret[2] = nytl::Vec4f(z);

	ret[0][3] = -dot(x, eye);
	ret[1][3] = -dot(y, eye);
	ret[2][3] = -dot(z, eye);

	return ret;
}

template<typename P>
nytl::SquareMat<4, P> lookAtRH(const nytl::Vec3<P>& eye,
		const nytl::Vec3<P>& center, const nytl::Vec3<P>& up) {

	const auto z = normalized(center - eye);
	const auto x = normalized(cross(z, up));
	const auto y = cross(x, z);

	auto ret = nytl::identity<4, P>();

	ret[0] = nytl::Vec4f(x);
	ret[1] = nytl::Vec4f(y);
	ret[2] = -nytl::Vec4f(z);

	ret[0][3] = -dot(x, eye);
	ret[1][3] = -dot(y, eye);
	ret[2][3] = dot(z, eye);

	return ret;
}


// == 2D coordinate transformations ==
// A rectangular view of the level, in level coordinates.
// TODO: could add support for rotation
struct LevelView {
	nytl::Vec2f center; // center of the view in level coords
	nytl::Vec2f size; // total size of the view in level coords
};

// Returns a matrix mapping from the given view into level space to
// normalized coordinates ([-1, 1], for rendering).
// yup: whether the y axis is going up in level coordinates.
inline nytl::Mat4f levelMatrix(const LevelView& view, bool yup = true) {
	// the matrix gets level coordinates as input and returns
	// normalized window coordinates ([-1, 1])
	auto mat = nytl::identity<4, float>();
	nytl::Vec2f scale = {
		2.f / view.size.x,
		(yup ? -2.f : 2.f) / view.size.y,
	};

	// scale
	mat[0][0] = scale.x;
	mat[1][1] = scale.y;

	// translation; has to acknowledge scale
	mat[0][3] = -view.center.x * scale.x;
	mat[1][3] = -view.center.y * scale.y;

	return mat;
}

// Returns the viewSize into the level for the given widnowSize, when
// the larger window dimension should show `longSide` in level coordinate
// units.
// windowRatio: windowSize.x / windowSize.y
inline nytl::Vec2f levelViewSize(float windowRatio, float longSide) {
	nytl::Vec2f viewSize = {longSide, longSide};
	if (windowRatio > 1.f) {
		viewSize.y *= (1 / windowRatio);
	} else {
		viewSize.x *= windowRatio;
	}
	return viewSize;
}

// Returns a matrix mapping from the given view into level space to
// normalized coordinates ([-1, 1], for rendering).
// windowRatio: windowSize.x / windowSize.y
// centerOn: level position to center on; translation. If a point
//   is drawn in this position, it will be in the mid of the center
// longSide: how many coordinate units of the level the larger window will cover.
// yup: whether the y axis is going up in level coordinates.
inline nytl::Mat4f levelMatrix(float windowRatio, nytl::Vec2f center,
		float longSide, bool yup = true) {
	return levelMatrix({center, levelViewSize(windowRatio, longSide)}, yup);
}

// Can e.g. be used to translate clicks on the window to level coordinates.
// yup: whether the level (!) coordinate system is assumed to have a y axis
//   going upwards. Window coordinates are always expected to have their
//   origin in the top left.
// NOTE: you usually don't need the matrix, see the windowToLevel function
// below.
inline nytl::Mat4f windowToLevelMatrix(nytl::Vec2ui windowSize,
		const LevelView& view, bool yup = true) {
	auto mat = nytl::identity<4, float>();
	auto vs = nytl::Vec2f {view.size.x, yup ? -view.size.y : view.size.y};

	mat[0][0] = vs.x / windowSize.x;
	mat[1][1] = vs.y / windowSize.y;

	mat[0][3] = view.center.x - 0.5f * vs.x;
	mat[1][3] = view.center.y - 0.5f * vs.y;

	return mat;
}

inline nytl::Vec2f windowToLevel(nytl::Vec2ui windowSize,
		const LevelView& view, nytl::Vec2i input, bool yup = true) {
	auto vs = nytl::Vec2f {view.size.x, yup ? -view.size.y : view.size.y};
	return {
		(input.x / float(windowSize.x) - 0.5f) * vs.x + view.center.x,
		(input.y / float(windowSize.y) - 0.5f) * vs.y + view.center.y,
	};
}

// TODO: implement
// void scaleAroundWindow(LevelView& view, nytl::Vec2ui windowPos);
// void scaleAroundLevel(LevelView& view, nytl::Vec2f levelPos);
//
// // View coordinates relative inside the view, in range [-1, 1]
// void scaleAroundView(LevelView& view, nytl::Vec2f viewPos);

} // namespace tkn