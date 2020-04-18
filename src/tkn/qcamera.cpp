#include <tkn/qcamera.hpp>

namespace tkn {

bool checkMovement(QuatCamera& c, swa_display* dpy, float dt,
		const QuatCameraMovement& params) {

	auto fac = dt;
	if(swa_display_active_keyboard_mods(dpy) & params.fastMod) {
		fac *= params.fastMult;
	}
	if(swa_display_active_keyboard_mods(dpy) & params.slowMod) {
		fac *= params.slowMult;
	}

	auto right = apply(c.rot, nytl::Vec3f{1.f, 0.f, 0.f});
	auto up = params.respectRotationY ?
		apply(c.rot, nytl::Vec3f{0.f, 1.f, 0.f}) :
		nytl::Vec3f{0.f, 1.f, 0.f};
	auto fwd = apply(c.rot, nytl::Vec3f{0.f, 0.f, -1.f});
	bool update = false;
	if(swa_display_key_pressed(dpy, params.right)) { // right
		c.pos += fac * right;
		update = true;
	}
	if(swa_display_key_pressed(dpy, params.left)) { // left
		c.pos += -fac * right;
		update = true;
	}
	if(swa_display_key_pressed(dpy, params.forward)) { // forward
		c.pos += fac * fwd;
		update = true;
	}
	if(swa_display_key_pressed(dpy, params.backward)) { // backwards
		c.pos += -fac * fwd;
		update = true;
	}
	if(swa_display_key_pressed(dpy, params.up)) { // up
		c.pos += fac * up;
		update = true;
	}
	if(swa_display_key_pressed(dpy, params.down)) { // down
		c.pos += -fac * up;
		update = true;
	}

	if(update) {
		c.update = true;
	}

	return update;
}

} // namespace tkn

