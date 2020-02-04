#pragma once

#include <nytl/vec.hpp>
#include <tkn/quaternion.hpp>
#include <tkn/transform.hpp>
#include <dlg/dlg.hpp>

#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>

namespace tkn {

struct QuatCamera {
	bool update = true; // set when changed
	nytl::Vec3f pos {0.f, 0.f, 0.f};
	Quaternion rot {1, 0, 0, 0};

	// float yaw {0.f}, pitch {0.f}, roll {0.f};
};

inline auto viewMatrix(const QuatCamera& c) {
	auto up = apply(c.rot, nytl::Vec3f {0.f, 1.f, 0.f});
	auto dir = apply(c.rot, nytl::Vec3f {0.f, 0.f, -1.f});
	return tkn::lookAtRH(c.pos, c.pos + dir, up);
}

inline auto fixedViewMatrix(const QuatCamera& c) {
	auto up = apply(c.rot, nytl::Vec3f {0.f, 1.f, 0.f});
	auto dir = apply(c.rot, nytl::Vec3f {0.f, 0.f, -1.f});
	// dlg_info("up: {}, dir: {}", up, dir);
	// dlg_info("quat: {} {} {} {}", c.rot.w, c.rot.x, c.rot.y, c.rot.z);
	return tkn::lookAtRH({}, dir, up);
}

// yaw: rotation around y axis (i.e looking to left or right)
// pitch: rotation around x axis (i.e looking down or up)
// roll: rotation around z axis (i.e. tilting left or right)
inline void rotateView(QuatCamera& c, float yaw, float pitch, float roll) {
	// TODO: not sure if normalize is needed. Should be able to
	// just check empirically if the eulerAngle function returns
	// normalized quaternions
	//
	// auto r = Quaternion{-c.rot.w, c.rot.x, c.rot.y, c.rot.z};
	// auto up = apply(r, nytl::Vec3f {0.f, 1.f, 0.f});
	// auto dir = apply(c.rot, nytl::Vec3f {0.f, 0.f, -1.f});
	// auto q = Quaternion::eulerAngle(pitch, yaw, roll);
	// auto q = Quaternion::axisAngle(1, 0, 0, pitch);
	// auto q = Quaternion::axisAngle(up.x, up.y, up.z, yaw);
	// c.rot = normalize(q * c.rot);

	// auto right = apply(c.rot, nytl::Vec3f {1.f, 0.f, 0.f});
	// q =	Quaternion::axisAngle(right.x, right.y, right.z, pitch);

		// Quaternion::axisAngle(dir.x, dir.y, dir.z, roll);
	// c.rot = normalize(q * c.rot);

	// auto axis = nytl::Vec3f{pitch, yaw, roll};
	// auto ta = apply(c.rot, axis);
	c.rot = normalize(c.rot * Quaternion::eulerAngle(-pitch, -yaw, -roll));
	// c.rot = normalize(c.rot * Quaternion::axisAngle(up.x, up.y, up.z, yaw) * Quaternion::axisAngle(1, 0, 0, pitch) * Quaternion::axisAngle(0, 0, 1, roll));
	// c.rot = normalize(Quaternion::axisAngle(0, 0, -1, roll) * c.rot);
	c.update = true;

	// c.yaw += yaw;
	// c.pitch += pitch;
	// c.roll += roll;
	// c.rot = Quaternion::eulerAngle(c.pitch, c.yaw, c.roll);
}

struct QuatCameraMovement {
	// The keycodes to check for the respective movement.
	// Can be set to ny::Keycode::none to disable movement
	// in that direction.
	ny::Keycode forward = ny::Keycode::w;
	ny::Keycode backward = ny::Keycode::s;
	ny::Keycode left = ny::Keycode::a;
	ny::Keycode right = ny::Keycode::d;
	ny::Keycode up = ny::Keycode::q;
	ny::Keycode down = ny::Keycode::e;

	// The modifiers that allow faster/slower movement.
	// Can be set to Modifier::none to disable the feature.
	ny::KeyboardModifier fastMod = ny::KeyboardModifier::shift;
	ny::KeyboardModifier slowMod = ny::KeyboardModifier::ctrl;

	// Specifies by how much the modifiers make the movement
	// faster or slower.
	float fastMult = 5.f;
	float slowMult = 0.2f;

	// Whether up and down should respect the current orientation
	// of the camera. For many camera implementations, up and down
	// always use the (global) Y axis statically instead of the upwards
	// pointing vector from the camera.
	bool respectRotationY = true;
};

// Checks default wasd+qe movement (with modifiers for faster/slower).
// Returns whether a change was made.
bool checkMovement(QuatCamera& c, ny::KeyboardContext& kc, float dt,
		const QuatCameraMovement& params = {});

} // namespace tkn

