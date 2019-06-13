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

nytl::Mat4f cubeProjectionVP(nytl::Vec3f pos, unsigned face,
		float near, float far) {
	static constexpr struct {
		nytl::Vec3f normal;
		nytl::Vec3f up;
	} views[6] = {
		{{1.f, 0.f, 0.f}, {0.f, -1.f, 0.f}},
		{{-1.f, 0.f, 0.f}, {0.f, -1.f, 0.f}},
		{{0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}},
		{{0.f, -1.f, 0.f}, {0.f, 0.f, -1.f}},
		{{0.f, 0.f, 1.f}, {0.f, -1.f, 0.f}},
		{{0.f, 0.f, -1.f}, {0.f, -1.f, 0.f}},
	};

	auto fov = 0.5 * nytl::constants::pi;
	auto aspect = 1.f;
	auto mat = doi::perspective3RH<float>(fov, aspect, near, far);
	mat = mat * doi::lookAtRH(pos, pos + views[face].normal, views[face].up);
	return mat;

	/*
	// alternative implementation that uses manual rotations (and translation)
	// instead lookAt. Mostly from
	// https://github.com/SaschaWillems/Vulkan/blob/master/examples/shadowmappingomni/shadowmappingomni.cpp
	auto pi = float(nytl::constants::pi);
	auto fov = 0.5 * pi;
	auto aspect = 1.f;
	auto np = 0.1f;
	auto fp = this->data.farPlane;
	auto mat = doi::perspective3RH<float>(fov, aspect, np, fp);
	auto viewMat = nytl::identity<4, float>();

	switch(i) {
	case 0:
		// viewMat = doi::rotateMat({1.f, 0.f, 0.f}, pi)
		viewMat = doi::rotateMat({0.f, 1.f, 0.f}, pi / 2);
		break;
	case 1:	// NEGATIVE_X
		// viewMat = doi::rotateMat({1.f, 0.f, 0.f}, pi)
		viewMat = doi::rotateMat({0.f, 1.f, 0.f}, -pi / 2);
		break;
	case 2:	// POSITIVE_Y
		viewMat = doi::rotateMat({1.f, 0.f, 0.f}, pi / 2);
		break;
	case 3:	// NEGATIVE_Y
		viewMat = doi::rotateMat({1.f, 0.f, 0.f}, -pi / 2);
		break;
	case 4:	// POSITIVE_Z
		viewMat = doi::rotateMat({1.f, 0.f, 0.f}, pi);
		break;
	case 5:	// NEGATIVE_Z
		viewMat = doi::rotateMat({0.f, 0.f, 1.f}, pi);
		break;
	}

	return mat * viewMat * doi::translateMat({-this->data.position});
	*/
}

} // namespace doi
