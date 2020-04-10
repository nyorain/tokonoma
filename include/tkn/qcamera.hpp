#pragma once

#include <nytl/vec.hpp>
#include <tkn/quaternion.hpp>
#include <tkn/transform.hpp>
#include <dlg/dlg.hpp>

#include <swa/swa.h>
#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>

namespace tkn {

struct QuatCamera {
	bool update = true; // set when changed
	nytl::Vec3f pos {0.f, 0.f, 0.f};
	Quaternion rot {1, 0, 0, 0};
};

inline auto viewMatrix(const QuatCamera& c) {
	auto up = apply(c.rot, nytl::Vec3f {0.f, 1.f, 0.f});
	auto dir = apply(c.rot, nytl::Vec3f {0.f, 0.f, -1.f});
	return tkn::lookAtRH(c.pos, c.pos + dir, up);
}

inline auto fixedViewMatrix(const QuatCamera& c) {
	auto up = apply(c.rot, nytl::Vec3f {0.f, 1.f, 0.f});
	auto dir = apply(c.rot, nytl::Vec3f {0.f, 0.f, -1.f});
	return tkn::lookAtRH({}, dir, up);
}

// yaw: rotation around y axis (i.e looking to left or right)
// pitch: rotation around x axis (i.e looking down or up)
// roll: rotation around z axis (i.e. tilting left or right)
inline void rotateView(QuatCamera& c, float yaw, float pitch, float roll) {
	c.rot = normalize(c.rot * Quaternion::eulerAngle(-pitch, -yaw, -roll));
	c.update = true;
}

// TODO: deprecrated, remove. Use swa instead
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

struct QuatCameraMovementSwa {
	// The keycodes to check for the respective movement.
	// Can be set to ny::Keycode::none to disable movement
	// in that direction.
	swa_key forward = swa_key_w;
	swa_key backward = swa_key_s;
	swa_key left = swa_key_a;
	swa_key right = swa_key_d;
	swa_key up = swa_key_q;
	swa_key down = swa_key_e;

	// The modifiers that allow faster/slower movement.
	// Can be set to Modifier::none to disable the feature.
	swa_keyboard_mod fastMod = swa_keyboard_mod_shift;
	swa_keyboard_mod slowMod = swa_keyboard_mod_ctrl;

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

bool checkMovement(QuatCamera& c, swa_display* dpy, float dt,
		const QuatCameraMovementSwa& params = {});

} // namespace tkn

