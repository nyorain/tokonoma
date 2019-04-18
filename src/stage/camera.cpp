#include <stage/camera.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/key.hpp>

namespace doi {

void checkMovement(Camera& c, ny::KeyboardContext& kc, float dt) {
	auto fac = dt;
	auto yUp = nytl::Vec3f {0.f, 1.f, 0.f};
	auto right = nytl::normalized(nytl::cross(c.dir, yUp));
	auto up = nytl::normalized(nytl::cross(c.dir, right));
	if(kc.pressed(ny::Keycode::d)) { // right
		c.pos += fac * right;
		c.update = true;
	}
	if(kc.pressed(ny::Keycode::a)) { // left
		c.pos += -fac * right;
		c.update = true;
	}
	if(kc.pressed(ny::Keycode::w)) {
		c.pos += fac * c.dir;
		c.update = true;
	}
	if(kc.pressed(ny::Keycode::s)) {
		c.pos += -fac * c.dir;
		c.update = true;
	}
	if(kc.pressed(ny::Keycode::q)) { // up
		c.pos += -fac * up;
		c.update = true;
	}
	if(kc.pressed(ny::Keycode::e)) { // down
		c.pos += fac * up;
		c.update = true;
	}
}

} // namespace doi
