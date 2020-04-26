#include <tkn/camera2.hpp>

namespace tkn {

// Spaceship
void update(Camera& cam, SpaceshipCamCon& con, swa_display* dpy, float dt,
		const SpaceshipCamControls& controls) {
	checkMovement(cam, dpy, dt, controls.move);

	// check if we are currently rotating
	constexpr auto invalid = 0x7FFF;
	auto rot = swa_display_mouse_button_pressed(dpy, controls.rotateButton);
	if(con.mposStart.x != invalid) {
		if(rot) {
			Vec2i mpos;
			swa_display_mouse_position(dpy, &mpos.x, &mpos.y);
			auto d = controls.rotateFac * dt * Vec2f(mpos - con.mposStart);

			auto sign = [](auto f) { return f > 0.f ? 1.f : -1.f; };
			auto yaw = sign(d.x) * std::pow(std::abs(d.x), controls.rotatePow);
			auto pitch = sign(d.y) * std::pow(std::abs(d.y), controls.rotatePow);
			tkn::rotateView(cam, yaw, pitch, 0.f);
			cam.update = true;
		} else {
			con.mposStart.x = invalid;
		}
	} else if(rot) {
		swa_display_mouse_position(dpy,
			&con.mposStart.x, &con.mposStart.y);
	}

	// update roll
	if(swa_display_key_pressed(dpy, controls.rollLeft)) {
		con.rollVel -= controls.rollFac * dt;
	}

	if(swa_display_key_pressed(dpy, controls.rollRight)) {
		con.rollVel += controls.rollFac * dt;
	}

	con.rollVel *= std::pow(1.f - controls.rollFriction, dt);
	if(std::abs(con.rollVel) > -1.0001) {
		tkn::rotateView(cam, 0.f, 0.f, con.rollVel);
		cam.update = true;
	}
}

// FPS
void mouseMove(Camera& cam, FPCamCon& con, swa_display* dpy,
		Vec2i delta, const FPCamControls& controls) {
	if(controls.rotateButton == swa_mouse_button_none ||
			swa_display_mouse_button_pressed(dpy, controls.rotateButton)) {
		con.yaw -= controls.fac * delta.x;
		con.pitch -= controls.fac * delta.y;

		if(controls.limitPitch) {
			using nytl::constants::pi;
			auto e = controls.pitchEps;
			con.pitch = std::clamp<float>(con.pitch, -pi / 2 + e, pi / 2 - e);
		}

		cam.rot = Quaternion::eulerAngle(con.pitch, con.yaw, 0.f);
		cam.update = true;
	}
}

// Arcball
Vec3f center(const Camera& cam, const ArcballCamCon& con) {
	return cam.pos + con.offset * dir(cam);
}

void mouseMove(Camera& cam, ArcballCamCon& arc, swa_display* dpy,
		Vec2i delta, const ArcballControls& controls, Vec2f panFac) {
	auto mods = swa_display_active_keyboard_mods(dpy);
	if(swa_display_mouse_button_pressed(dpy, controls.panButton) &&
			mods == controls.panMod) {
		// y is double flipped because of y-down convention for
		// mouse corrds vs y-up convention of rendering
		auto x = -panFac.x * delta.x * right(cam);
		auto y = panFac.y * delta.y * up(cam);
		cam.pos += x + y;
		cam.update = true;
	}

	if(swa_display_mouse_button_pressed(dpy, controls.rotateButton) &&
			mods == controls.rotateMod) {
		auto c = center(cam, arc);
		float yaw = controls.rotateFac.x * delta.x;
		float pitch = controls.rotateFac.y * delta.y;
		Quaternion rot;
		if(controls.allowRoll) {
			rot = cam.rot * Quaternion::eulerAngle(-pitch, -yaw, 0);
		} else {
			rot = cam.rot * Quaternion::eulerAngle(-pitch, 0, 0);
			rot = Quaternion::eulerAngle(0, -yaw, 0) * rot;
		}

		cam.rot = rot;
		cam.pos = c - arc.offset * dir(cam);
		cam.update = true;
	}
}

void mouseMovePersp(Camera& cam, ArcballCamCon& arc, swa_display* dpy,
		Vec2i delta, Vec2ui winSize, float fov, const ArcballControls& ctrls) {
	auto aspect = winSize.x / float(winSize.y);
	auto f = 0.5f / float(std::tan(fov / 2.f));
	float fx = arc.offset * aspect / (f * winSize.x);
	float fy = arc.offset / (f * winSize.y);
	mouseMove(cam, arc, dpy, delta, ctrls, {fx, fy});
}

void mouseWheel(Camera& cam, ArcballCamCon& arc, float delta) {
	auto c = center(cam, arc);
	arc.offset *= std::pow(1.05, delta);
	cam.pos = c - arc.offset * dir(cam);
	cam.update = true;
}

bool checkMovement(Camera& c, swa_display* dpy, float dt,
		const CamMoveControls& params) {

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
