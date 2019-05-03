#include <stage/camera.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/key.hpp>

namespace doi {

bool checkMovement(Camera& c, ny::KeyboardContext& kc, float dt) {
	auto fac = dt;
	if(kc.modifiers() & ny::KeyboardModifier::shift) {
		fac *= 5;
	}
	if(kc.modifiers() & ny::KeyboardModifier::ctrl) {
		fac *= 0.2;
	}

	auto yUp = nytl::Vec3f {0.f, 1.f, 0.f};
	auto right = nytl::normalized(nytl::cross(c.dir, yUp));
	auto up = nytl::normalized(nytl::cross(c.dir, right));
	bool update = false;
	if(kc.pressed(ny::Keycode::d)) { // right
		c.pos += fac * right;
		update = true;
	}
	if(kc.pressed(ny::Keycode::a)) { // left
		c.pos += -fac * right;
		update = true;
	}
	if(kc.pressed(ny::Keycode::w)) {
		c.pos += fac * c.dir;
		update = true;
	}
	if(kc.pressed(ny::Keycode::s)) {
		c.pos += -fac * c.dir;
		update = true;
	}
	if(kc.pressed(ny::Keycode::q)) { // up
		c.pos += -fac * up;
		update = true;
	}
	if(kc.pressed(ny::Keycode::e)) { // down
		c.pos += fac * up;
		update = true;
	}

	if(update) {
		c.update = true;
	}

	return update;
}

} // namespace doi
