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

void init(TouchCameraController& tc, tkn::Camera& cam, rvg::Context& rvgctx) {
	tc.cam = &cam;
	tc.paint = {rvgctx, rvg::colorPaint({255, 200, 200, 40})};

	rvg::DrawMode dm;
#ifndef __ANDROID__
	dm.aaFill = true;
#endif // __ANDROID__
	dm.fill = true;
	dm.deviceLocal = true;
	tc.move.circle = {rvgctx, {}, 20.f, dm};
	tc.move.circle.disable(true);
	tc.rotate.circle = {rvgctx, {}, 20.f, dm};
	tc.rotate.circle.disable(true);
}

void touchBegin(TouchCameraController& tc, const ny::TouchBeginEvent& ev,
		nytl::Vec2ui windowSize) {
	constexpr auto invalidID = TouchCameraController::invalidID;
	using namespace nytl::vec::cw::operators;
	auto rp = ev.pos / windowSize;
	if(rp.x < 0.5f && tc.move.id == invalidID) {
		tc.move.id = ev.id;
		tc.move.pos = ev.pos;
		tc.move.start = ev.pos;

		tc.move.circle.disable(false);
		tc.move.circle.change()->center = ev.pos;
	} else if(rp.x > 0.5f && tc.rotate.id == invalidID) {
		tc.rotate.id = ev.id;
		tc.rotate.pos = ev.pos;
		tc.rotate.start = ev.pos;

		tc.rotate.circle.disable(false);
		tc.rotate.circle.change()->center = ev.pos;
	}
}

void touchEnd(TouchCameraController& tc, const ny::TouchEndEvent& ev) {
	constexpr auto invalidID = TouchCameraController::invalidID;
	if(ev.id == tc.rotate.id) {
		tc.rotate.id = invalidID;
		tc.rotate.circle.disable(true);
	} else if(ev.id == tc.move.id) {
		tc.move.id = invalidID;
		tc.move.circle.disable(true);
	}
}

void touchUpdate(TouchCameraController& tc, const ny::TouchUpdateEvent& ev) {
	dlg_assert(tc.cam);
	if(ev.id == tc.rotate.id) {
		auto delta = ev.pos - tc.rotate.pos;
		tc.rotate.pos = ev.pos;

		if(!tc.alt) {
			tkn::rotateView(*tc.cam, 0.005 * delta.x, 0.005 * delta.y);
		}
	} else if(ev.id == tc.move.id) {
		auto delta = ev.pos - tc.move.pos;
		tc.move.pos = ev.pos;

		if(!tc.alt) {
			auto& c = *tc.cam;
			auto yUp = nytl::Vec3f {0.f, 1.f, 0.f};
			auto right = nytl::normalized(nytl::cross(c.dir, yUp));

			auto fac = 0.01f;
			c.pos += fac * delta.x * right;
			c.pos -= fac * delta.y * c.dir; // y input coords have top-left origin
			c.update = true;
		}
	}
}

void update(TouchCameraController& tc, double dt) {
	dlg_assert(tc.cam);
	constexpr auto invalidID = TouchCameraController::invalidID;
	if(tc.alt) {
		auto sign = [](auto f) { return f > 0.f ? 1.f : -1.f; };
		auto cut = [&](auto f) {
			auto off = 20.f;
			if(std::abs(f) < off) return 0.f;
			return f - sign(f) * off;
		};

		if(tc.move.id != invalidID) {
			auto& c = *tc.cam;
			auto yUp = nytl::Vec3f {0.f, 1.f, 0.f};
			auto right = nytl::normalized(nytl::cross(c.dir, yUp));

			nytl::Vec2f delta = tc.move.pos - tc.move.start;
			delta = {cut(delta.x), cut(delta.y)};
			delta.x = sign(delta.x) * std::pow(std::abs(delta.x), 1.65);
			delta.y = sign(delta.y) * std::pow(std::abs(delta.y), 1.65);
			delta *= 0.0005f * dt;

			c.pos += delta.x * right;
			c.pos -= delta.y * c.dir; // y input coords have top-left origin
			c.update = true;
		}
		if(tc.rotate.id != invalidID) {
			nytl::Vec2f delta = tc.rotate.pos - tc.rotate.start;
			delta = {cut(delta.x), cut(delta.y)};
			delta.x = sign(delta.x) * std::pow(std::abs(delta.x), 1.65);
			delta.y = sign(delta.y) * std::pow(std::abs(delta.y), 1.65);
			delta *= 0.0004f * dt;

			tkn::rotateView(*tc.cam, delta.x, delta.y);
		}
	}
}

} // namespace tkn
