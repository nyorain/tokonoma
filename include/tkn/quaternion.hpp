// Simple, lightweight and independent Quaternion implementation.
// Mostly put together from snippets and implementation notes on the internet.

#pragma once

#include <cmath> // std::sin
#include <cassert>

#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>

// What makes quaternions somewhat hard to work with is that once again
// everybody has a different convention regarding roll, pitch, yaw and
// attitude, heading, bank. We use it like this (taking the oriented
// camera object as example):
// - pitch: rotation around x-axis (up/down)
// - yaw: rotation around y-axis (rotate left/right)
// - roll: rotation around z-axis (tilt left/right)

namespace tkn {

/// Represents a mathematical quaternion, useful for representing 3D rotations.
/// See https://en.wikipedia.org/wiki/Quaternion for the mathematical background.
class Quaternion {
public:
	double x = 0.f, y = 0.f, z = 0.f;
	double w = 1.f;

public:
	/// Constructs a Quaternion from an axis (given by x,y,z) and an angle (in radians)
	/// to rotate around the axis.
	static Quaternion axisAngle(double ax, double ay, double az, double angle) {
		auto ha = std::sin(angle / 2);
		return {ax * ha, ay * ha, az * ha, std::cos(angle / 2)};
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

		return {
			sx * cy * cz - cx * sy * sz,
			cx * sy * cz + sx * cy * sz,
			cx * cy * sz - sx * sy * cz,
			cx * cy * cz + sx * sy * sz,
		};
		// return {
		// 	cx * sy * cz - sx * cy * sz,
		// 	cx * cy * sz + sx * sy * cz,
		// 	sx * cy * cz - cx * sy * sz
		// 	cx * cy * cz + sx * sy * sz,
		// };
		// return {
		// 	sx * cy * cz - cx * sy * sz,
		// 	sx * cy * sz + cx * sy * cz,
		// 	cx * cy * sz - sx * sy * cz,
		// 	cx * cy * cz + sx * sy * sz,
		// };
	}

	static Quaternion yxz(double yaw, double pitch, double roll) {
		double cy = std::cos(yaw * 0.5);
		double sy = std::sin(yaw * 0.5);
		double cp = std::cos(pitch * 0.5);
		double sp = std::sin(pitch * 0.5);
		double cr = std::cos(roll * 0.5);
		double sr = std::sin(roll * 0.5);

		return {
			cy * sp * cr + sy * cp * sr,
			sy * cp * cr - cy * sp * sr,
			cy * cp * sr - sy * sp * cr,
			cy * cp * cr + sy * sp * sr,
		};
	}

	// WIP
	static Quaternion taitBryan(double yaw, double pitch, double roll) {
		double cx = std::cos(roll * 0.5);
		double sx = std::sin(roll * 0.5);
		double cy = std::cos(pitch * 0.5);
		double sy = std::sin(pitch * 0.5);
		double cz = std::cos(yaw * 0.5);
		double sz = std::sin(yaw * 0.5);

		return {
			sx * cy * cz + cx * sy * sz,
			cx * cy * cz - sx * sy * sz,
			cx * sy * cz - sx * cy * sz,
			cx * cy * sz + sx * sy * cz,
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

		x = rhs.w * lhs.x + rhs.x * lhs.w + rhs.y * lhs.z - rhs.z * lhs.y;
		y = rhs.w * lhs.y - rhs.x * lhs.z + rhs.y * lhs.w + rhs.z * lhs.x;
		z = rhs.w * lhs.z + rhs.x * lhs.y - rhs.y * lhs.x + rhs.z * lhs.w;
		w = rhs.w * lhs.w - rhs.x * lhs.x - rhs.y * lhs.y - rhs.z * lhs.z;
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
	{ return a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w; }

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
	return {-q.x, -q.y, -q.z, q.w};
}

/// Returns the norm of the given Quaternion.
inline double norm(const Quaternion& q) {
	return std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
}

/// Returns a unit quaternion for the given quaternion.
inline Quaternion normalize(const Quaternion& q) {
	auto l = norm(q);
	if(l <= 0.0) return {0.0, 0.0, 0.0, 1.0};
	return {q.x / l, q.y / l, q.z / l, q.w / l};
}

/// Returns the given vector rotated by the rotation represented by the given
/// Quaternion.
template<typename T>
nytl::Vec3<T> apply(const Quaternion& q, const nytl::Vec3<T>& v) {
	// TODO: can probably be implemented more efficiently
	auto qv = Quaternion{v.x, v.y, v.z, 0.f};
	auto qr = (q * qv) * conjugate(q);
	assert(std::abs(qr.w) < 0.01f);
	return {T(qr.x), T(qr.y), T(qr.z)};
}

/// Retrieves the x-axis rotation of the given Quaternion
inline double xRot(const Quaternion& q) {
	double t0 = 2.0 * (q.w * q.x + q.y * q.z);
	double t1 = 1.0 - 2.0 * (q.x * q.x + q.y * q.y);
	return std::atan2(t0, t1);
}

/// Retrieves the y-axis rotation of the given Quaternion
inline double yRot(const Quaternion& q) {
	double t2 = 2.0 * (q.w * q.y - q.z * q.x);
	t2 = t2 > 1.0 ? 1.0 : t2;
	t2 = t2 < -1.0 ? -1.0 : t2;
	return std::asin(t2);
}

/// Retrieves the z-axis rotation of the given Quaternion.
inline double zRot(const Quaternion& q) {
	double t3 = 2.0 * (q.w * q.z + q.x * q.y);
	double t4 = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
	return std::atan2(t3, t4);
}


/// WIP
// inline double yaw(const Quaternion& q) {
// 	double t0 = 2.f * (q.y * q.w + q.z * q.x);
// 	double t1 = 1.f - 2.f * (q.y * q.y + q.x * q.x);
// 	return -std::atan2(t0, t1);
// }

// inline double pitch(const Quaternion& q) {
// 	double t0 = std::clamp(2.f * (q.z * q.y - q.x * q.w), -1.0, 1.0);
// 	return std::asin(t0);
// }

// WIP
inline double roll(const Quaternion& q) {
	double t0 = 2.f * (q.x * q.y + q.z * q.w);
	double t1 = -q.z * q.z - q.x * q.x + q.y * q.y + q.w * q.w;
	return std::atan2(t0, t1);
}

inline double yaw(const Quaternion& q) {
	double t0 = 2.f * (q.z * q.x + q.y * q.w);
	double t1 = q.z * q.z - q.x * q.x - q.y * q.y + q.w * q.w;
	return std::atan2(t0, t1);
}

inline double pitch(const Quaternion& q) {
	double t0 = std::clamp(2.f * (q.x * q.w - q.z * q.y), -1.0, 1.0);
	return std::asin(t0);
}

} // namespace tkn

// NOTE: implementation sources:
// - https://en.wikipedia.org/wiki/Conversion_between_quaternions_and_Euler_angles
