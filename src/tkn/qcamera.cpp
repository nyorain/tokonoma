#include <tkn/qcamera.hpp>

namespace tkn {

bool checkMovement(QuatCamera& c, ny::KeyboardContext& kc, float dt,
		const QuatCameraMovement& params) {

	auto fac = dt;
	if(kc.modifiers() & params.fastMod) {
		fac *= params.fastMult;
	}
	if(kc.modifiers() & params.slowMod) {
		fac *= params.slowMult;
	}

	auto right = apply(c.rot, nytl::Vec3f{1.f, 0.f, 0.f});
	auto up = params.respectRotationY ?
		apply(c.rot, nytl::Vec3f{0.f, 1.f, 0.f}) :
		nytl::Vec3f{0.f, 1.f, 0.f};
	auto fwd = apply(c.rot, nytl::Vec3f{0.f, 0.f, -1.f});
	bool update = false;
	if(kc.pressed(params.right)) { // right
		c.pos += fac * right;
		update = true;
	}
	if(kc.pressed(params.left)) { // left
		c.pos += -fac * right;
		update = true;
	}
	if(kc.pressed(params.forward)) { // forward
		c.pos += fac * fwd;
		update = true;
	}
	if(kc.pressed(params.backward)) { // backwards
		c.pos += -fac * fwd;
		update = true;
	}
	if(kc.pressed(params.up)) { // up
		c.pos += fac * up;
		update = true;
	}
	if(kc.pressed(params.down)) { // down
		c.pos += -fac * up;
		update = true;
	}

	if(update) {
		c.update = true;
	}

	return update;
}

} // namespace tkn

