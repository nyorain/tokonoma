#include <tkn/ccam.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

// ControlledCamera
ControlledCamera::ControlledCamera(ControlType ctrl) {
	useControl(ctrl);
}

void ControlledCamera::useControl(ControlType ctrl) {
	switch(ctrl) {
		case ControlType::arcball: this->useArcballControl(); break;
		case ControlType::spaceship: this->useSpaceshipControl(); break;
		case ControlType::firstPerson: this->useFirstPersonControl(); break;
		case ControlType::none: break;
	}
}

nytl::Mat4f ControlledCamera::viewMatrix() {
	if(camChanged_) {
		view_ = tkn::viewMatrix(camera_);
		camChanged_ = false;
		matChanged_ = true;
	}

	return view_;
}

nytl::Mat4f ControlledCamera::projectionMatrix() {
	if(projChanged_) {
		projection_ = calcProjectionMatrix();
		if(flipY_) {
			tkn::flipY(projection_);
		}

		projChanged_ = false;
		matChanged_ = true;
	}

	return projection_;
}

nytl::Mat4f ControlledCamera::viewProjectionMatrix() {
	return projectionMatrix() * viewMatrix();
}

nytl::Mat4f ControlledCamera::fixedViewMatrix() {
	return tkn::fixedViewMatrix(camera());
}

nytl::Mat4f ControlledCamera::fixedViewProjectionMatrix() {
	return projectionMatrix() * fixedViewMatrix();
}

void ControlledCamera::position(nytl::Vec3f pos) {
	camera_.pos = pos;
	camChanged_ = true;
}

void ControlledCamera::orientation(const Quaternion& rot) {
	camera_.rot = rot;
	camChanged_ = true;
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
	camChanged_ = true;
}

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
	// TODO: referesh view matrix here? we got rid of the roll after all...
	//   or maybe change FPCamCon to return the new up vector and use that
	//   so that we just keep the current roll?
}
void ControlledCamera::useSpaceshipControl(const SpaceshipCamControls& ctrls) {
	controls_ = Spaceship{{}, ctrls};
}

template<typename ...Ts>
struct Visitor : Ts...  {
	Visitor(const Ts&... args) : Ts(args)...  {}
	using Ts::operator()...;
};

void ControlledCamera::update(swa_display* dpy, double dt) {
	std::visit(Visitor {
		[&](Spaceship& spaceship) {
			camChanged_ |= tkn::update(camera_, spaceship.con,
				dpy, dt, spaceship.controls);
		},
		[&](FirstPerson& fp) {
			camChanged_ |= tkn::checkMovement(camera_, dpy, dt, fp.move);
		},
		[&](const auto&) {
		}
	}, controls_);
}

void ControlledCamera::mouseMove(swa_display* dpy, nytl::Vec2i delta,
		nytl::Vec2ui winSize) {
	std::visit(Visitor {
		[&](FirstPerson& fp) {
			camChanged_ |= ::tkn::mouseMove(camera_, fp.con, dpy, delta,
				fp.controls);
		},
		[&](Arcball& arcball) {
			auto pan = 2.f * unprojectUnit(arcball.con.offset);
			pan.x /= winSize.x;
			pan.y /= winSize.y;
			camChanged_ |= ::tkn::mouseMove(camera_, arcball.con, dpy, delta,
				arcball.controls, pan);
		},
		[](const auto&) {
		}
	}, controls_);
}

void ControlledCamera::mouseWheel(float delta) {
	if(auto* arcball = std::get_if<Arcball>(&controls_)) {
		mouseWheelZoom(camera_, arcball->con, delta);
		camChanged_ = true;
	}
}

void ControlledCamera::flipY(bool f) {
	flipY_ = f;
	projChanged_ = true;
}

// perspective
PerspectiveCamera::PerspectiveCamera(ControlType c, float near, float far,
		Mode mode) : ControlledCamera(c), mode_(mode), near_(near), far_(far) {
}

nytl::Mat4f PerspectiveCamera::calcProjectionMatrix() const {
	dlg_assert(aspect_ != 0.f);
	dlg_assert(fov_ != 0.f);

	switch(mode_) {
		case Mode::normal:
			return perspective(fov_, aspect_, near_, far_);
		case Mode::revDepth:
			return perspectiveRev(fov_, aspect_, near_, far_);
		case Mode::revDepthInf:
			return perspectiveRevInf(fov_, aspect_, near_);
		default:
			dlg_error("Unreachable: invalid perspective mode");
			return {};
	}
}

nytl::Vec2f PerspectiveCamera::unprojectUnit(float depth) const {
	auto f = 1.f / float(std::tan(fov_ / 2.f));
	float fx = depth * aspect_ / f;
	float fy = depth / f;
	return {fx, fy};
}

void PerspectiveCamera::near(float near) {
	near_ = near;
	invalidateProjection();
}

void PerspectiveCamera::far(float far) {
	far_ = far;
	invalidateProjection();
}

void PerspectiveCamera::fov(float fov) {
	fov_ = fov;
	invalidateProjection();
}

void PerspectiveCamera::aspect(float aspect) {
	aspect_ = aspect;
	invalidateProjection();
}

void PerspectiveCamera::mode(Mode mode) {
	mode_ = mode;
	invalidateProjection();
}
// ortho
OrthographicCamera::OrthographicCamera(ControlType c, float width, float height,
		float near, float far) : ControlledCamera(c) {
	rect(width, height, near, far);
}

OrthographicCamera::OrthographicCamera(ControlType c,
		float left, float right, float bot, float top,
		float near, float far) : ControlledCamera(c)  {
	rect(left, right, bot, top, near, far);
}

void OrthographicCamera::rect(float width, float height, float near, float far) {
	rect(-width / 2, width / 2, -height / 2, height / 2, near, far);
}
void OrthographicCamera::rect(float left, float right, float bot, float top,
		float near, float far) {
	left_ = left;
	right_ = right;
	top_ = top;
	bot_ = bot;
	near_ = near;
	far_ = far;
	invalidateProjection();
}

nytl::Mat4f OrthographicCamera::calcProjectionMatrix() const {
	return ortho(left_, right_, bot_, top_, near_, far_);
}

nytl::Vec2f OrthographicCamera::unprojectUnit(float depth) const {
	(void) depth;
	return {0.5f * (right_ - left_), 0.5f * (top_ - bot_)};
}

} // namespace tkn
