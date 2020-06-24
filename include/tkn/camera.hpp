#pragma once

#include <tkn/types.hpp>
#include <tkn/quaternion.hpp>
#include <tkn/transform.hpp>
#include <swa/swa.h>

// Simple lowlevel camera module.
// Implements a central camera structure that uniquely identifies
// a camera via position and orientation.
// Furthermore offers various different camera controllers.
// The general goal here was to bring all these different camera
// styles under a common roof to make switching the used
// control scheme easy.
// Furthermore, we aim to not store redundant state in the camera
// structure or its controllers (as far as possible).

namespace tkn {

struct Camera {
	Vec3f pos {0.f, 0.f, 1.f}; // position of camera in world space
	Quaternion rot {}; // transforms from camera/view space to world space
	bool update {true};
};

[[nodiscard]] inline nytl::Vec3f dir(const Camera& c) {
	return -apply(c.rot, nytl::Vec3f {0.f, 0.f, 1.f});
}

[[nodiscard]] inline nytl::Vec3f up(const Camera& c) {
	return apply(c.rot, nytl::Vec3f {0.f, 1.f, 0.f});
}

[[nodiscard]] inline nytl::Vec3f right(const Camera& c) {
	return apply(c.rot, nytl::Vec3f {1.f, 0.f, 0.f});
}

[[nodiscard]] inline nytl::Mat4f viewMatrix(const Camera& c) {
	return tkn::lookAt(c.rot, c.pos);
}

[[nodiscard]] inline nytl::Mat4f fixedViewMatrix(const Camera& c) {
	return tkn::lookAt(c.rot, nytl::Vec3f{});
}

// yaw: rotation around y axis (i.e looking to left or right)
// pitch: rotation around x axis (i.e looking down or up)
// roll: rotation around z axis (i.e. tilting left or right)
// Note that this might introduce roll (even if roll is always set to 0.0)
// due to this interpretation of rotation works. Usually better to
// use a camera controller setup.
inline void rotateView(Camera& c, float yaw, float pitch, float roll) {
	c.rot = normalized(c.rot * Quaternion::yxz(-yaw, -pitch, -roll));
	c.update = true;
}


/// Arguments (with defaults) for checkMovement.
/// Only exists so movement keys and multipliers are not hardcoded
/// but could be adjusted by applications.
struct CamMoveControls {
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

	// Multiplier that is always applied
	float mult = 1.f;

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

// Implements direct (first-person) camera movement via the specified keys.
bool checkMovement(Camera&, swa_display* dpy, float dt,
	const CamMoveControls& params = {});

// camera controllers
// spaceship: no fixed up firection, uses quaternion for orientation.
// might look weird when rotating on ground since up isn't fixed.
struct SpaceshipCamCon {
	static constexpr auto mposInvalid = 0x7FFF;
	Vec2i mposStart {mposInvalid, mposInvalid};
	float rollVel {0.f};
};

struct SpaceshipCamControls {
	float rotateFac = 0.0025f;
	float rotatePow = 1.f;
	float rollFac = 25.f; // roll velocity per second
	float rollFriction = 0.9999f;
	swa_mouse_button rotateButton = swa_mouse_button_left;
	swa_key rollLeft = swa_key_z;
	swa_key rollRight = swa_key_x;
	CamMoveControls move = {};
};

// Calls checkMovement as well.
bool update(Camera&, SpaceshipCamCon&, swa_display* dpy, float dt,
	const SpaceshipCamControls& = {});

// First-person camera controller.
// Permits no roll by default, limits pitch to positive or negative 90 degs.
// Yaw is always the rotation around a fixed up vector (0, 1, 0) instead
// of relative to the camera.
struct FPCamCon {
	float yaw {0.f};
	float pitch {0.f};
	float roll {0.f};

	[[nodiscard]] static FPCamCon fromOrientation(const Quaternion&);
};

struct FPCamControls {
	float fac = 0.005f;
	float rollFac = 0.005f;
	bool limitPitch = true;
	float pitchEps = 0.1;

	// set to 'none' to always handle mouseMove as rotation
	swa_mouse_button rotateButton = swa_mouse_button_left;

	// set to 'none' to disable roll
	// NOTE: since rotation for a first person controller
	// is always around the 'up' axis (positive y axis),
	// rotating after rolling will not feel intuitive.
	// For natural camera rolling, see the spaceship controller
	swa_mouse_button rollButton = swa_mouse_button_none;
};

bool mouseMove(Camera&, FPCamCon&, swa_display*, Vec2i delta,
	const FPCamControls& controls = {});

// Third-person arcball controller.
// Has a dynamic center around which it rotates, the distance
// from that can be changed via zooming.
struct ArcballCamCon {
	// Describes how far the rotation center is in front of the camera.
	float offset {1.f};
};

struct ArcballControls {
	swa_mouse_button rotateButton = swa_mouse_button_middle;
	swa_mouse_button panButton = swa_mouse_button_middle;
	swa_keyboard_mod rotateMod = swa_keyboard_mod_none;
	swa_keyboard_mod panMod = swa_keyboard_mod_shift;
	Vec2f rotateFac = {0.005f, 0.005f};
	bool allowRoll = false;
};

// Returns the rotation center of the camera with the given
// arcball controller.
[[nodiscard]] Vec3f center(const Camera&, const ArcballCamCon&);

// Implements rotation and panning in response to mouse move events.
// - delta: The delta of the mouse movement, in pixels.
// - panFac: Factor that translates the window-space movement delta
//   into a position delta for the camera for a panning movement.
//   See mouseMovePersp for calculating this based on the used
//   perspective projection.
bool mouseMove(Camera&, ArcballCamCon&, swa_display*, Vec2i delta,
	const ArcballControls& = {}, Vec2f panFac = {0.01f, 0.01f});

// Like mouseMove but instead of a hardcoded panning factor, uses
// information about the perspective projection to make sure a pixel
// movement for the mouse always results in the projection of the
// rotation center being translated by one pixel (meaning that the
// rotation center will exactly move with the mouse).
bool mouseMovePersp(Camera&, ArcballCamCon&, swa_display*, Vec2i delta,
	Vec2ui winSize, float fov, const ArcballControls& = {});

// Implements zooming for the given vertical mouse wheel delta.
void mouseWheelZoom(Camera&, ArcballCamCon&, float delta,
	float zoomFac = 1.05f);

} // namespace tkn
