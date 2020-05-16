#include <tkn/camera.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/key.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

bool checkMovement(Camera& c, bool modShift, bool modCtrl,
		std::array<bool, 6> dirs, float dt) {
	auto fac = dt;
	if(modShift) {
		fac *= 5;
	}
	if(modCtrl) {
		fac *= 0.2;
	}

	auto vdir = dir(c);
	auto vright = nytl::normalized(nytl::cross(vdir, c.up));
	auto vup = nytl::normalized(nytl::cross(vright, vdir));
	bool update = false;
	if(dirs[0]) { // right
		c.pos += fac * vright;
		update = true;
	}
	if(dirs[1]) { // left
		c.pos += -fac * vright;
		update = true;
	}
	if(dirs[2]) { // forward
		c.pos += fac * vdir;
		update = true;
	}
	if(dirs[3]) { // backwards
		c.pos += -fac * vdir;
		update = true;
	}
	if(dirs[4]) { // up
		c.pos += fac * vup;
		update = true;
	}
	if(dirs[5]) { // down
		c.pos += -fac * vup;
		update = true;
	}

	if(update) {
		c.update = true;
	}

	return update;
}

bool checkMovement(Camera& c, ny::KeyboardContext& kc, float dt) {
	auto mods = kc.modifiers();
	return checkMovement(c,
		mods & ny::KeyboardModifier::shift,
		mods & ny::KeyboardModifier::ctrl, {
			kc.pressed(ny::Keycode::d),
			kc.pressed(ny::Keycode::a),
			kc.pressed(ny::Keycode::w),
			kc.pressed(ny::Keycode::s),
			kc.pressed(ny::Keycode::q),
			kc.pressed(ny::Keycode::e),
	}, dt);
}

bool checkMovement(Camera& c, swa_display* dpy, float dt) {
	auto mods = swa_display_active_keyboard_mods(dpy);
	return checkMovement(c,
		mods & swa_keyboard_mod_shift,
		mods & swa_keyboard_mod_ctrl, {
			swa_display_key_pressed(dpy, swa_key_d),
			swa_display_key_pressed(dpy, swa_key_a),
			swa_display_key_pressed(dpy, swa_key_w),
			swa_display_key_pressed(dpy, swa_key_s),
			swa_display_key_pressed(dpy, swa_key_q),
			swa_display_key_pressed(dpy, swa_key_e),
	}, dt);
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
	auto mat = tkn::perspective<float>(fov, aspect, -near, -far);
	return mat * view;
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

void touchBegin(TouchCameraController& tc, unsigned id, nytl::Vec2f pos,
		nytl::Vec2ui windowSize) {
	constexpr auto invalidID = TouchCameraController::invalidID;
	using namespace nytl::vec::cw::operators;
	auto rp = pos / windowSize;
	if(rp.x < 0.5f && tc.move.id == invalidID) {
		tc.move.id = id;
		tc.move.pos = pos;
		tc.move.start = pos;

		tc.move.circle.disable(false);
		tc.move.circle.change()->center = pos;
	} else if(rp.x > 0.5f && tc.rotate.id == invalidID) {
		tc.rotate.id = id;
		tc.rotate.pos = pos;
		tc.rotate.start = pos;

		tc.rotate.circle.disable(false);
		tc.rotate.circle.change()->center = pos;
	}
}

void touchEnd(TouchCameraController& tc, unsigned id) {
	constexpr auto invalidID = TouchCameraController::invalidID;
	if(id == tc.rotate.id) {
		tc.rotate.id = invalidID;
		tc.rotate.circle.disable(true);
	} else if(id == tc.move.id) {
		tc.move.id = invalidID;
		tc.move.circle.disable(true);
	}
}

void touchUpdate(TouchCameraController& tc, unsigned id, nytl::Vec2f pos) {
	dlg_assert(tc.cam);
	if(id == tc.rotate.id) {
		auto delta = pos - tc.rotate.pos;
		tc.rotate.pos = pos;

		if(!tc.alt) {
			tkn::rotateView(*tc.cam, 0.005 * delta.x, 0.005 * delta.y);
		}
	} else if(id == tc.move.id) {
		auto delta = pos - tc.move.pos;
		tc.move.pos = pos;

		if(!tc.alt) {
			auto& c = *tc.cam;
			auto zdir = dir(c);
			auto right = nytl::normalized(nytl::cross(zdir, c.up));

			auto fac = 0.01f;
			c.pos += fac * delta.x * right;
			c.pos -= fac * delta.y * zdir; // y input coords have top-left origin
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
			auto zdir = dir(c);
			auto right = nytl::normalized(nytl::cross(zdir, c.up));

			nytl::Vec2f delta = tc.move.pos - tc.move.start;
			delta = {cut(delta.x), cut(delta.y)};
			delta.x = sign(delta.x) * std::pow(std::abs(delta.x), 1.65);
			delta.y = sign(delta.y) * std::pow(std::abs(delta.y), 1.65);
			delta *= tc.positionMultiplier * 0.0005f * dt;

			c.pos += delta.x * right;
			c.pos -= delta.y * zdir; // y input coords have top-left origin
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
