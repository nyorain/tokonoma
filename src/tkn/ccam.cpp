#include <tkn/ccam.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

template<typename ...Ts>
struct Visitor : Ts...  {
	Visitor(const Ts&... args) : Ts(args)...  {}
	using Ts::operator()...;
};

nytl::Vec2f orthoExtent(float maxSize, float aspect) {
	float width = maxSize;
	float height = maxSize;
	if(aspect > 1.f) {
		height /= aspect;
	} else {
		width *= aspect;
	}

	return {width, height};
}

// ControlledCamera
ControlledCamera::ControlledCamera(ControlType ctrl, const Perspective& persp) {
	useControl(ctrl);
	perspective(persp);
}

ControlledCamera::ControlledCamera(ControlType ctrl, const Orthographic& ortho) {
	useControl(ctrl);
	orthographic(ortho);
}

void ControlledCamera::useControl(ControlType ctrl) {
	switch(ctrl) {
		case ControlType::arcball: this->useArcballControl(); break;
		case ControlType::spaceship: this->useSpaceshipControl(); break;
		case ControlType::firstPerson: this->useFirstPersonControl(); break;
		case ControlType::none: break;
	}
}

nytl::Mat4f ControlledCamera::viewMatrix() const {
	return tkn::viewMatrix(camera_);
}

nytl::Mat4f ControlledCamera::projectionMatrix() const {
	nytl::Mat4f proj = std::visit(Visitor{
		[&](const Perspective& p) {
			dlg_assert(p.aspect != 0.f);
			dlg_assert(p.fov != 0.f);

			switch(p.mode) {
				case PerspectiveMode::normal:
					return tkn::perspective(p.fov, p.aspect, p.near, p.far);
				case PerspectiveMode::revDepth:
					return tkn::perspectiveRev(p.fov, p.aspect, p.near, p.far);
				case PerspectiveMode::revDepthInf:
					return tkn::perspectiveRevInf(p.fov, p.aspect, p.near);
				default:
					dlg_error("Unreachable: invalid perspective mode");
					return nytl::Mat4f {};
			}
		}, [&](const Orthographic& ortho) {
			dlg_assert(ortho.aspect != 0.f);
			dlg_assert(ortho.maxSize > 0.f);

			auto [width, height] = orthoExtent(ortho.maxSize, ortho.aspect);
			// TODO
			return tkn::ortho(-width / 2, width / 2, -height / 2, height,
				ortho.far, ortho.near);
		},
	}, projection_);

	if(flipY_) {
		tkn::flipY(proj);
	}

	return proj;
}

nytl::Mat4f ControlledCamera::viewProjectionMatrix() const {
	return projectionMatrix() * viewMatrix();
}

nytl::Mat4f ControlledCamera::fixedViewMatrix() const {
	return tkn::fixedViewMatrix(camera());
}

nytl::Mat4f ControlledCamera::fixedViewProjectionMatrix() const {
	return projectionMatrix() * fixedViewMatrix();
}

void ControlledCamera::position(nytl::Vec3f pos) {
	camera_.pos = pos;
	needsUpdate = true;
}

void ControlledCamera::orientation(const Quaternion& rot) {
	camera_.rot = rot;
	needsUpdate = true;
}

void ControlledCamera::lookAt(nytl::Vec3f pos, nytl::Vec3f dir,
		nytl::Vec3f up) {
	// yes, this is apparently the cleanest way to do this.
	// I feel like there should be a super intuitive way to construct
	// a lookAt quaternion but apparently there is not (with the
	// up vector at least, creating an 'tkn::orientMat'-like quaternion
	// is possible).
	camera_.rot = Quaternion::fromMat(tkn::lookAt<3>(-dir, up));
	camera_.pos = pos;
	needsUpdate = true;
}

// NOTE: changing the control does not change the camera itself
// so needsUpdate does not need to be set to true.
ControlledCamera::ControlType ControlledCamera::controlType() const {
	if(std::holds_alternative<Arcball>(controls_)) {
		return ControlType::arcball;
	} else if(std::holds_alternative<FirstPerson>(controls_)) {
		return ControlType::firstPerson;
	} else if(std::holds_alternative<Spaceship>(controls_)) {
		return ControlType::spaceship;
	}

	return ControlType::none;
}

std::optional<ControlledCamera::Arcball*> ControlledCamera::arcballControl() {
	if(auto* v = std::get_if<Arcball>(&controls_)) {
		return std::optional{v};
	}

	return std::nullopt;
}

std::optional<ControlledCamera::FirstPerson*> ControlledCamera::firstPersonControl() {
	if(auto* v = std::get_if<FirstPerson>(&controls_)) {
		return std::optional{v};
	}

	return std::nullopt;
}

std::optional<ControlledCamera::Spaceship*> ControlledCamera::spaceshipControl() {
	if(auto* v = std::get_if<Spaceship>(&controls_)) {
		return std::optional{v};
	}

	return std::nullopt;
}

void ControlledCamera::disableControl() {
	controls_ = std::monostate{};
}

void ControlledCamera::useArcballControl(const ArcballControls& ctrls) {
	controls_ = Arcball{{}, ctrls};
}
void ControlledCamera::useFirstPersonControl(const FPCamControls& ctrls,
		const CamMoveControls& move) {
	auto con = FPCamCon::fromOrientation(camera().rot);
	controls_ = FirstPerson{con, ctrls, move};
	// TODO: set needsUpdate to true here? we got rid of the roll after all...
	//   or maybe change FPCamCon to return the new up vector and use that
	//   so that we just keep the current roll?
}
void ControlledCamera::useSpaceshipControl(const SpaceshipCamControls& ctrls) {
	controls_ = Spaceship{{}, ctrls};
}

void ControlledCamera::update(swa_display* dpy, double dt) {
	std::visit(Visitor {
		[&](Spaceship& spaceship) {
			needsUpdate |= tkn::update(camera_, spaceship.con,
				dpy, dt, spaceship.controls);
		},
		[&](FirstPerson& fp) {
			needsUpdate |= tkn::checkMovement(camera_, dpy, dt, fp.move);
		},
		[&](const auto&) {
		}
	}, controls_);
}

void ControlledCamera::mouseMove(swa_display* dpy, nytl::Vec2i delta,
		nytl::Vec2ui winSize) {
	std::visit(Visitor {
		[&](FirstPerson& fp) {
			needsUpdate |= ::tkn::mouseMove(camera_, fp.con, dpy, delta,
				fp.controls);
		},
		[&](Arcball& arcball) {
			auto pan = 2.f * unprojectUnit(arcball.con.offset);
			pan.x /= winSize.x;
			pan.y /= winSize.y;
			needsUpdate |= ::tkn::mouseMove(camera_, arcball.con, dpy, delta,
				arcball.controls, pan);
		},
		[](const auto&) {
		}
	}, controls_);
}

void ControlledCamera::key(swa_key key, bool pressed) {
	std::visit(Visitor {
		[&](Spaceship& spaceship) {
			keyEvent(spaceship.con.move, key, pressed);
		},
		[](const auto&) {
		}
	}, controls_);
}

void ControlledCamera::mouseButton(swa_mouse_button button, bool pressed) {
	std::visit(Visitor {
		[&](FirstPerson& fp) {
			tkn::mouseButton(fp.con, button, pressed, fp.controls);
		},
		[&](Arcball& arcball) {
			tkn::mouseButton(arcball.con, button, pressed, arcball.controls);
		},
		[&](Spaceship& spaceship) {
			tkn::mouseButton(spaceship.con, button, pressed, spaceship.controls);
		},
		[&](const auto&) {}
	}, controls_);
}

void ControlledCamera::mouseWheel(float delta) {
	if(auto* arcball = std::get_if<Arcball>(&controls_)) {
		mouseWheelZoom(camera_, arcball->con, delta, arcball->zoomFac);
		needsUpdate = true;
	}

	// TODO: remove again
	if(auto* p = std::get_if<Perspective>(&projection_)) {
		p->fov *= std::pow(1.05, delta);
		needsUpdate = true;
	}
}

void ControlledCamera::flipY(bool f) {
	flipY_ = f;
	needsUpdate = true;
}

nytl::Vec2f ControlledCamera::unprojectUnit(float linDepth) const {
	return std::visit(Visitor{
		[&](const Perspective& p) {
			auto f = float(std::tan(p.fov / 2.f));
			float fx = linDepth * p.aspect * f;
			float fy = linDepth * f;
			return Vec2f{fx, fy};
		},
		[&](const Orthographic& o) {
			auto [width, height] = orthoExtent(o.maxSize, o.aspect);
			return Vec2f{0.5f * width, 0.5f * height};
		},
	}, projection_);
}

void ControlledCamera::orthographic(const Orthographic& o) {
	projection_ = o;
	needsUpdate = true;
}

void ControlledCamera::perspective(const Perspective& p) {
	projection_ = p;
	needsUpdate = true;
}

std::optional<ControlledCamera::Orthographic> ControlledCamera::orthographic() const {
	if(auto* v = std::get_if<Orthographic>(&projection_)) {
		return std::optional{*v};
	}
	return std::nullopt;
}

std::optional<ControlledCamera::Perspective> ControlledCamera::perspective() const {
	if(auto* v = std::get_if<Perspective>(&projection_)) {
		return std::optional{*v};
	}
	return std::nullopt;
}

bool ControlledCamera::isOrthographic() const {
	return std::holds_alternative<Orthographic>(projection_);
}

void ControlledCamera::near(float n) {
	std::visit(Visitor{
		[&](Perspective& p) { p.near = n; },
		[&](Orthographic& o) { o.near = n; },
	}, projection_);
	needsUpdate = true;
}

void ControlledCamera::far(float f) {
	std::visit(Visitor{
		[&](Perspective& p) { p.far = f; },
		[&](Orthographic& o) { o.far = f; },
	}, projection_);
	needsUpdate = true;
}

void ControlledCamera::aspect(float a) {
	std::visit(Visitor{
		[&](Perspective& p) { p.aspect = a; },
		[&](Orthographic& o) { o.aspect = a; },
	}, projection_);
	needsUpdate = true;
}

void ControlledCamera::aspect(nytl::Vec2ui windowSize) {
	aspect(float(windowSize.x) / windowSize.y);
}

void ControlledCamera::perspectiveFov(float fov) {
	auto* v = std::get_if<Perspective>(&projection_);
	dlg_assertm(v, "Camera does not use perspective projection");
	v->fov = fov;
	needsUpdate = true;
}

void ControlledCamera::perspectiveMode(PerspectiveMode mode) {
	auto* v = std::get_if<Perspective>(&projection_);
	dlg_assertm(v, "Camera does not use perspective projection");
	v->mode = mode;
	needsUpdate = true;
}

void ControlledCamera::orthoSize(float size) {
	auto* v = std::get_if<Orthographic>(&projection_);
	dlg_assertm(v, "Camera does not use orthographic projection");
	v->maxSize = size;
	needsUpdate = true;
}

float ControlledCamera::near() const {
	return std::visit(Visitor{
		[&](const auto& v) { return v.near; }
	}, projection_);
}
float ControlledCamera::far() const {
	return std::visit(Visitor{
		[&](const auto& v) { return v.far; }
	}, projection_);
}
float ControlledCamera::aspect() const {
	return std::visit(Visitor{
		[&](const auto& v) { return v.aspect; }
	}, projection_);
}
std::optional<float> ControlledCamera::perspectiveFov() const {
	if(auto* v = std::get_if<Perspective>(&projection_)) {
		return std::optional{v->fov};
	}
	return std::nullopt;
}
std::optional<float> ControlledCamera::orthoSize() const {
	if(auto* v = std::get_if<Orthographic>(&projection_)) {
		return std::optional{v->maxSize};
	}
	return std::nullopt;
}
std::optional<ControlledCamera::PerspectiveMode>
		ControlledCamera::perspectiveMode() {
	if(auto* v = std::get_if<Perspective>(&projection_)) {
		return std::optional{v->mode};
	}
	return std::nullopt;
}

} // namespace tkn
