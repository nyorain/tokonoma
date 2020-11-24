#include <tkn/camera.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

// Spaceship
bool update(Camera& cam, SpaceshipCamCon& con, swa_display* dpy, float dt,
		const SpaceshipCamControls& controls) {
	Vec3f move = checkMovement(cam.rot, dpy, controls.move);
	con.moveVel += dt * move;

	auto updated = false;

	// check if we are currently rotating
	if(con.mposStart.x != con.mposInvalid) {
		if(con.rotating) {
			// update going-on rotation
			Vec2i mpos;
			swa_display_mouse_position(dpy, &mpos.x, &mpos.y);
			auto d = controls.rotateFac * Vec2f(mpos - con.mposStart);

			auto sign = [](auto f) { return f > 0.f ? 1.f : -1.f; };
			float yawAcc = sign(d.x) * std::pow(std::abs(d.x), controls.rotatePow);
			float pitchAcc = sign(d.y) * std::pow(std::abs(d.y), controls.rotatePow);

			con.yawVel += dt * yawAcc;
			con.pitchVel += dt * pitchAcc;
		} else {
			// rotation ended
			con.mposStart.x = con.mposInvalid;
		}
	} else if(con.rotating) {
		// rotation was started
		swa_display_mouse_position(dpy,
			&con.mposStart.x, &con.mposStart.y);
	}

	if(std::abs(con.yawVel) + std::abs(con.pitchVel) > 0.0001) {
		tkn::rotateView(cam, dt * con.yawVel, dt * con.pitchVel, 0.f);
		cam.update = true;
		updated = true;
	}

	// update movement
	if(dot(con.moveVel, con.moveVel) > 0.0000001) {
		cam.pos += dt * con.moveVel;
		cam.update = true;
		updated = true;
	}

	// update roll
	float rollAcc = 0.0;
	if(swa_display_key_pressed(dpy, controls.rollLeft)) {
		rollAcc -= controls.rollFac;
		updated = true;
	}

	if(swa_display_key_pressed(dpy, controls.rollRight)) {
		rollAcc += controls.rollFac;
		updated = true;
	}

	con.rollVel += dt * rollAcc;
	if(std::abs(con.rollVel) > 0.0001) {
		tkn::rotateView(cam, 0.f, 0.f, dt * con.rollVel);
		cam.update = true;
		updated = true;
	}

	// con.rollVel *= std::pow(1.f - controls.rollFriction, dt);
	// con.yawVel *= std::pow(1.f - controls.yawFriction, dt);
	// con.pitchVel *= std::pow(1.f - controls.pitchFriction, dt);
	con.rollVel *= std::exp(-dt * controls.rollFriction);
	con.yawVel *= std::exp(-dt * controls.yawFriction);
	con.pitchVel *= std::exp(-dt * controls.pitchFriction);
	con.moveVel *= std::exp(-dt * controls.moveFriction);

	return updated;
}

void mouseButton(SpaceshipCamCon& con, swa_mouse_button button,
		bool pressed, const SpaceshipCamControls& controls) {
	if(button == controls.rotateButton) {
		con.rotating = pressed;
	}
}

// FPS
FPCamCon FPCamCon::fromOrientation(const Quaternion& q) {
	auto [yaw, pitch, roll] = eulerAngles(q, RotationSequence::yxz);
	// dlg_assertlm(dlg_level_warn, std::abs(roll) < 0.05,
	// 	"Disregarding non-zero roll in conversion to FPCamCon yaw/pitch");
	return {float(yaw), float(pitch), float(roll)};
}

bool mouseMove(Camera& cam, FPCamCon& con, swa_display* dpy,
		Vec2i delta, const FPCamControls& controls) {
	auto res = false;
	using nytl::constants::pi;
	if(controls.rotateButton == swa_mouse_button_none || con.rotating) {
		con.yaw = std::fmod(con.yaw - controls.fac * delta.x, 2 * pi);
		con.pitch -= controls.fac * delta.y;

		if(controls.limitPitch) {
			using nytl::constants::pi;
			auto e = controls.pitchEps;
			con.pitch = std::clamp<float>(con.pitch, -pi / 2 + e, pi / 2 - e);
		}

		cam.rot = Quaternion::yxz(con.yaw, con.pitch, con.roll);

		// attempt to make rotations rotate around an axis respecting
		// the roll. Not better though.
		// cam.rot =
		// 	Quaternion::axisAngle(0, 0, 1, con.roll) *
		// 	Quaternion::axisAngle(0, 1, 0, con.yaw) *
		// 	Quaternion::axisAngle(1, 0, 0, con.pitch);
		cam.update = true;
		res = true;
	}

	if(controls.rollButton != swa_mouse_button_none &&
			swa_display_mouse_button_pressed(dpy, controls.rollButton)) {
		con.roll = std::fmod(con.roll + controls.rollFac * delta.x, 2 * pi);
		cam.rot = Quaternion::yxz(con.yaw, con.pitch, con.roll);
		// cam.rot =
		// 	Quaternion::axisAngle(0, 0, 1, con.roll) *
		// 	Quaternion::axisAngle(0, 1, 0, con.yaw) *
		// 	Quaternion::axisAngle(1, 0, 0, con.pitch);

		cam.update = true;
		res = true;
	}

	return res;
}

void mouseButton(FPCamCon& con, swa_mouse_button button,
		bool pressed, const FPCamControls& controls) {
	if(button == controls.rotateButton) {
		con.rotating = pressed;
	}
}

// Arcball
Vec3f center(const Camera& cam, const ArcballCamCon& con) {
	return cam.pos + con.offset * dir(cam);
}

bool mouseMove(Camera& cam, ArcballCamCon& arc, swa_display* dpy,
		Vec2i delta, const ArcballControls& controls, Vec2f panFac) {
	bool ret = false;
	auto mods = swa_display_active_keyboard_mods(dpy);
	if(arc.panning && mods == controls.panMod) {
		// y is double flipped because of y-down convention for
		// mouse corrds vs y-up convention of rendering
		// TODO: maybe don't do this here but let the user decide
		// by mirroring panFac? At least document it the behavior.
		// Same for other functions though. Document how a movement
		// (especially mouse up/down) is translated into camera rotation.
		auto x = -panFac.x * delta.x * right(cam);
		auto y = panFac.y * delta.y * up(cam);
		cam.pos += x + y;
		cam.update = true;
		ret = true;
	}

	if(arc.rotating && mods == controls.rotateMod) {
		auto c = center(cam, arc);
		float yaw = controls.rotateFac.x * delta.x;
		float pitch = controls.rotateFac.y * delta.y;
		Quaternion rot;
		if(controls.allowRoll) {
			rot = cam.rot * Quaternion::yxz(-yaw, -pitch, 0);
			// rot = Quaternion::taitBryan(yaw, pitch, 0) * cam.rot;
		} else {
			rot = cam.rot * Quaternion::yxz(0, -pitch, 0);
			rot = Quaternion::yxz(-yaw, 0, 0) * rot;
		}

		cam.rot = rot;
		cam.pos = c - arc.offset * dir(cam);
		cam.update = true;
		ret = true;
	}

	return ret;
}

bool mouseMovePersp(Camera& cam, ArcballCamCon& arc, swa_display* dpy,
		Vec2i delta, Vec2ui winSize, float fov, const ArcballControls& ctrls) {
	auto aspect = winSize.x / float(winSize.y);
	auto f = 0.5f / float(std::tan(fov / 2.f));
	float fx = arc.offset * aspect / (f * winSize.x);
	float fy = arc.offset / (f * winSize.y);
	return mouseMove(cam, arc, dpy, delta, ctrls, {fx, fy});
}

void mouseWheelZoom(Camera& cam, ArcballCamCon& arc, float delta, float zoomFac) {
	auto c = center(cam, arc);
	arc.offset *= std::pow(zoomFac, delta);
	cam.pos = c - arc.offset * dir(cam);
	cam.update = true;
}

void mouseButton(ArcballCamCon& con, swa_mouse_button button,
		bool pressed, const ArcballControls& controls) {
	if(button == controls.panButton) {
		con.panning = pressed;
	}
	if(button == controls.rotateButton) {
		con.rotating = pressed;
	}
}

Vec3f checkMovement(const Quaternion& rot, swa_display* dpy,
		const CamMoveControls& params) {
	auto fac = params.mult;
	if(swa_display_active_keyboard_mods(dpy) & params.fastMod) {
		fac *= params.fastMult;
	}
	if(swa_display_active_keyboard_mods(dpy) & params.slowMod) {
		fac *= params.slowMult;
	}

	auto right = apply(rot, nytl::Vec3f{1.f, 0.f, 0.f});
	auto up = params.respectRotationY ?
		apply(rot, nytl::Vec3f{0.f, 1.f, 0.f}) :
		nytl::Vec3f{0.f, 1.f, 0.f};
	auto fwd = apply(rot, nytl::Vec3f{0.f, 0.f, -1.f});
	Vec3f accel {};

	// TODO: replace with states given over input to filter out gui input.
	if(swa_display_key_pressed(dpy, params.right)) { // right
		accel += fac * right;
	}
	if(swa_display_key_pressed(dpy, params.left)) { // left
		accel += -fac * right;
	}
	if(swa_display_key_pressed(dpy, params.forward)) { // forward
		accel += fac * fwd;
	}
	if(swa_display_key_pressed(dpy, params.backward)) { // backwards
		accel += -fac * fwd;
	}
	if(swa_display_key_pressed(dpy, params.up)) { // up
		accel += fac * up;
	}
	if(swa_display_key_pressed(dpy, params.down)) { // down
		accel += -fac * up;
	}

	return accel;
}

bool checkMovement(Camera& c, swa_display* dpy, float dt,
		const CamMoveControls& params) {

	Vec3f move = checkMovement(c.rot, dpy, params);
	if(move != Vec3f {}) {
		c.update = true;
		c.pos += dt * move;
		return true;
	}

	return false;
}

} // namespace tkn
