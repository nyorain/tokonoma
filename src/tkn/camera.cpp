#include <tkn/camera.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/key.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

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
	auto up = nytl::normalized(nytl::cross(right, c.dir));
	bool update = false;
	if(kc.pressed(ny::Keycode::d)) { // right
		c.pos += fac * right;
		update = true;
	}
	if(kc.pressed(ny::Keycode::a)) { // left
		c.pos += -fac * right;
		update = true;
	}
	if(kc.pressed(ny::Keycode::w)) { // forward
		c.pos += fac * c.dir;
		update = true;
	}
	if(kc.pressed(ny::Keycode::s)) { // backwards
		c.pos += -fac * c.dir;
		update = true;
	}
	if(kc.pressed(ny::Keycode::q)) { // up
		c.pos += fac * up;
		update = true;
	}
	if(kc.pressed(ny::Keycode::e)) { // down
		c.pos += -fac * up;
		update = true;
	}

	if(update) {
		c.update = true;
	}

	return update;
}

nytl::Mat4f cubeProjectionVP(nytl::Vec3f pos, unsigned face,
		float near, float far) {
	// y sign flipped everywhere
	// TODO: not sure why slightly different to pbr.cpp
	// (positive, negative y swapped), probably bug in pbr shaders
	constexpr struct CubeFace {
		nytl::Vec3f x;
		nytl::Vec3f y;
		nytl::Vec3f z; // direction of the face
	} faces[] = {
		{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
		{{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}},
		{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
		{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}},
		{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
		{{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
	};

	auto& f = faces[face];
	dlg_assertm(nytl::cross(f.x, f.y) == f.z,
		"{} {}", nytl::cross(f.x, f.y), f.z);

	nytl::Mat4f view = nytl::identity<4, float>();
	view[0] = nytl::Vec4f(f.x);
	view[1] = nytl::Vec4f(f.y);
	view[2] = -nytl::Vec4f(f.z);

	view[0][3] = -dot(f.x, pos);
	view[1][3] = -dot(f.y, pos);
	view[2][3] = dot(f.z, pos);

	auto fov = 0.5 * nytl::constants::pi;
	auto aspect = 1.f;
	auto mat = tkn::perspective3RH<float>(fov, aspect, near, far);
	return mat * view;

	/*
	static constexpr struct {
		nytl::Vec3f normal;
		nytl::Vec3f up;
	} views[6] = {
		{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}},
		{{-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}},
		{{0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}},
		{{0.f, -1.f, 0.f}, {0.f, 0.f, -1.f}},
		{{0.f, 0.f, 1.f}, {0.f, 1.f, 0.f}},
		{{0.f, 0.f, -1.f}, {0.f, 1.f, 0.f}},
	};

	auto fov = 0.5 * nytl::constants::pi;
	auto aspect = 1.f;
	auto mat = tkn::perspective3RH<float>(fov, aspect, near, far);
	mat = mat * tkn::lookAtRH(pos, pos + views[face].normal, views[face].up);
	return mat;
	*/

	/*
	// alternative implementation that uses manual rotations (and translation)
	// instead lookAt. Mostly from
	// https://github.com/SaschaWillems/Vulkan/blob/master/examples/shadowmappingomni/shadowmappingomni.cpp
	auto pi = float(nytl::constants::pi);
	auto fov = 0.5 * pi;
	auto aspect = 1.f;
	auto mat = tkn::perspective3RH<float>(fov, aspect, near, far);
	auto viewMat = nytl::identity<4, float>();

	switch(face) {
	case 0:
		// viewMat = tkn::rotateMat({1.f, 0.f, 0.f}, pi)
		viewMat = tkn::rotateMat({0.f, 1.f, 0.f}, pi / 2);
		break;
	case 1:	// NEGATIVE_X
		// viewMat = tkn::rotateMat({1.f, 0.f, 0.f}, pi)
		viewMat = tkn::rotateMat({0.f, 1.f, 0.f}, -pi / 2);
		break;
	case 2:	// POSITIVE_Y
		viewMat = tkn::rotateMat({1.f, 0.f, 0.f}, pi / 2);
		break;
	case 3:	// NEGATIVE_Y
		viewMat = tkn::rotateMat({1.f, 0.f, 0.f}, -pi / 2);
		break;
	case 4:	// POSITIVE_Z
		viewMat = tkn::rotateMat({1.f, 0.f, 0.f}, pi);
		break;
	case 5:	// NEGATIVE_Z
		viewMat = tkn::rotateMat({0.f, 0.f, 1.f}, pi);
		break;
	}

	return mat * viewMat * tkn::translateMat({-pos});
	*/
}

Frustum ndcFrustum() {
	return {{
		{-1.f, 1.f, 0.f},
		{1.f, 1.f, 0.f},
		{1.f, -1.f, 0.f},
		{-1.f, -1.f, 0.f},
		{-1.f, 1.f, 1.f},
		{1.f, 1.f, 1.f},
		{1.f, -1.f, 1.f},
		{-1.f, -1.f, 1.f},
	}};
}

} // namespace tkn
