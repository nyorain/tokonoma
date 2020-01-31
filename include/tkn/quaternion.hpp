// Simple, lightweight and independent Quaternion implementation.
// Mostly put together from snippets and implementation notes found on the internet.

#pragma once

#include <cmath> // std::sin

#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>

namespace tkn {

/// Represents a mathematical quaternion, useful for representing 3D rotations.
/// See https://en.wikipedia.org/wiki/Quaternion for the mathematical background.
class Quaternion {
public:
	double w = 1.f;
	double x = 0.f, y = 0.f, z = 0.f;

public:
	/// Constructs a Quaternion from a axis (given by x,y,z) and the angle (in radians)
	/// to rotate around the axis.
	static Quaternion axisAngle(double ax, double ay, double az, double angle) {
		auto ha = std::sin(angle / 2);
		return {std::cos(angle / 2), ax * ha, ay * ha, az * ha};
	}

	/// Constructs a Quaternion by given angles (in degrees) to rotate around
	/// the x,y,z axis.
	static Quaternion eulerAngle(double rx, double ry, double rz) {
		double cx = std::cos(rx * 0.5);
		double sx = std::sin(rx * 0.5);
		double cy = std::cos(ry * 0.5);
		double sy = std::sin(ry * 0.5);
		double cz = std::cos(rz * 0.5);
		double sz = std::sin(rz * 0.5);

		// return {
		// 	cx * cy * cz + sx * sy * sz,
		// 	sx * cy * cz - cx * sy * sz,
		// 	cx * sy * cz + sx * cy * sz,
		// 	cx * cy * sz - sx * sy * cz
		// };
		// return {
		// 	cx * cy * cz + sx * sy * sz,
		// 	cx * sy * cz - sx * cy * sz,
		// 	cx * cy * sz + sx * sy * cz,
		// 	sx * cy * cz - cx * sy * sz
		// };
		return {
			cx * cy * cz + sx * sy * sz,
			sx * cy * cz - cx * sy * sz,
			sx * cy * sz + cx * sy * cz,
			cx * cy * sz - sx * sy * cz,
		};
	}

public:
	/// Default-constructs the Quaternion to a zero rotation.
	Quaternion() noexcept = default;

	Quaternion& operator+=(const Quaternion& rhs) {
		x += rhs.x;
		y += rhs.y;
		z += rhs.z;
		w += rhs.w;
		return *this;
	}

	// hamilton product of quaternions
	Quaternion& operator*=(const Quaternion& q) {
		// lhs and rhs misnamed
		const auto rhs = *this; // copy since this will be changed
		const auto lhs = q; // might be the same as *this

		w = rhs.w * lhs.w - rhs.x * lhs.x - rhs.y * lhs.y - rhs.z * lhs.z;
		x = rhs.w * lhs.x + rhs.x * lhs.w + rhs.y * lhs.z - rhs.z * lhs.y;
		y = rhs.w * lhs.y - rhs.x * lhs.z + rhs.y * lhs.w + rhs.z * lhs.x;
		z = rhs.w * lhs.z + rhs.x * lhs.y - rhs.y * lhs.x + rhs.z * lhs.w;
		return *this;
	}

	// Quaternion& operator*=(double factor) {
	// 	// x *= factor;
	// 	// y *= factor;
	// 	// z *= factor;
	// 	w *= factor;
	// 	return *this;
	// }
};

// - operators and functions -
inline Quaternion operator+(Quaternion a, const Quaternion& b) { return (a += b); }
inline Quaternion operator*(Quaternion a, const Quaternion& b) { return (a *= b); }

inline bool operator==(const Quaternion& a, const Quaternion& b)
	{ return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
inline bool operator!=(const Quaternion& a, const Quaternion& b)
	{ return a.x != b.x || a.y == b.y || a.z == b.z || a.w == b.w; }

/// Returns a row-major NxN matrix that represents the given Quaternion.
template<std::size_t N, typename T = float>
nytl::SquareMat<N, T> toMat(const Quaternion& q) {
	static_assert(N >= 3);
	auto ret = nytl::identity<N, T>();

	auto wz = q.w * q.z;
	auto wy = q.w * q.y;
	auto wx = q.w * q.x;
	auto xx = q.x * q.x;
	auto xy = q.x * q.y;
	auto xz = q.x * q.z;
	auto yy = q.y * q.y;
	auto yz = q.y * q.z;
	auto zz = q.z * q.z;

	ret[0][0] = 1 - 2 * (yy + zz);
	ret[0][1] = 2 * (xy - wz);
	ret[0][2] = 2 * (wy + xz);

	ret[1][0] = 2 * (xy + wz);
	ret[1][1] = 1 - 2 * (xx + zz);
	ret[1][2] = 2 * (yz - wx);

	ret[2][0] = 2 * (xz - wy);
	ret[2][1] = 2 * (wx + yz);
	ret[2][2] = 1 - 2 * (xx + yy);

	return ret;
}

/// Returns the conjugate of the given Quaternion.
inline Quaternion conjugate(const Quaternion& q) {
	return {q.w, -q.x, -q.y, -q.z};
}

/// Returns the norm of the given Quaternion.
inline double norm(const Quaternion& q) {
	return std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
}

/// Returns a unit quaternion for the given quaternion.
inline Quaternion normalize(const Quaternion& q) {
	auto l = norm(q);
	if(l <= 0.0) return {1.0, 0.0, 0.0, 0.0};
	return {q.w / l, q.x / l, q.y / l, q.z / l};
}

/// Returns the given vector rotated by the rotation represented by the given
/// Quaternion.
template<typename T>
nytl::Vec3<T> apply(const Quaternion& q, const nytl::Vec3<T>& v) {
	// TODO: can probably be implemented more efficiently
	auto qv = Quaternion{0.f, v.x, v.y, v.z};
	auto qr = (q * qv) * conjugate(q);
	// assert(qr.w == 0.f);
	return {T(qr.x), T(qr.y), T(qr.z)};
}

/// Retrieves the roll (x-axis rotation) of the given Quaternion
inline double xRot(const Quaternion& q) {
	double t0 = 2.0 * (q.w * q.x + q.y * q.z);
	double t1 = 1.0 - 2.0 * (q.x * q.x + q.y * q.y);
	return std::atan2(t0, t1);
}

/// Retrieves the pitch (y-axis rotation) of the given Quaternion
inline double yRot(const Quaternion& q) {
	double t2 = 2.0 * (q.w * q.y - q.z * q.x);
	t2 = t2 > 1.0 ? 1.0 : t2;
	t2 = t2 < -1.0 ? -1.0 : t2;
	return std::asin(t2);
}

/// Retrieves the yaw (z-axis rotation) of the given Quaternion.
inline double zRot(const Quaternion& q) {
	double t3 = 2.0 * (q.w * q.z + q.x * q.y);
	double t4 = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
	return std::atan2(t3, t4);
}

} // namespace tkn

// NOTE: implementation sources:
// - https://en.wikipedia.org/wiki/Conversion_between_quaternions_and_Euler_angles<Paste>
