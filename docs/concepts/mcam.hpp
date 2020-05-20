#pragma once

#include <tkn/camera2.hpp>
#include <tkn/transform.hpp>
#include <nytl/rect.hpp>
#include <variant>

#include <dlg/dlg.hpp> // TODO

// High-level and easy-to-use camera concept that (in comparison
// to the low-level lightweight tkn::Camera) fully manages projection
// and input/controls management.

namespace tkn {

class ControlledCamera {
public:
	static constexpr auto defaultUp = nytl::Vec3f{0.f, 1.f, 0.f};

	enum class ControlType {
		none,
		arcball,
		firstPerson,
		spaceship,
	};

	// control information
	struct Arcball {
		ArcballCamCon con;
		ArcballControls controls;
	};

	struct FirstPerson {
		FPCamCon con;
		FPCamControls controls;
		CamMoveControls move;
	};

	struct Spaceship {
		SpaceshipCamCon con;
		SpaceshipCamControls controls;
	};

public:
	ControlledCamera() = default;
	ControlledCamera(ControlType ctrl);

	nytl::Mat4f viewMatrix();
	nytl::Mat4f projectionMatrix();
	nytl::Mat4f viewProjectionMatrix();

	void position(nytl::Vec3f pos);
	void orientation(const Quaternion& rot);
	void lookAt(nytl::Vec3f pos, nytl::Vec3f dir, nytl::Vec3f up = defaultUp);

	const Camera& camera() const { return camera_; }
	ControlType controlType() const;

	std::optional<Arcball*> arcballControl();
	std::optional<FirstPerson*> firstPersonControl();
	std::optional<Spaceship*> spaceshipControl();

	void useArcballControl(const ArcballControls& ctrls = {});
	void useSpaceshipControl(const SpaceshipCamControls& ctrls = {});
	void useFirstPersonControl(const FPCamControls& ctrls = {},
		const CamMoveControls& move = {});

	void update(swa_display* dpy, double dt);
	void mouseMove(swa_display* dpy, nytl::Vec2i delta, nytl::Vec2ui winSize);
	void mouseWheel(float delta);

	bool flipY() const { return flipY_; }
	void flipY(bool);

	// Returns whether a change ocurred since the last time the matrix was
	// calculated and 'resetChanged' called. Basically, after checking once
	// per frame via 'needsUpdate' if the camera has changed in some way
	// and dependent systems/buffers must be updated, you want to call
	// 'resetChanged', resetting the change state tracking for the next frame.
	bool needsUpdate() const { return matChanged_ || camChanged_ || projChanged_; }
	void resetChanged() { matChanged_ = false; }

protected:
	// Calculates the projection matrix.
	virtual nytl::Mat4f calcProjectionMatrix() const = 0;

	// Returns the size of an unprojected unit square (size (1, 1)) at
	// the given depth. Basically answers: "by how far do i have to move
	// something in pre-projection space so it moves by (1, 1) in projection
	// space".
	// Implementations don't have to cache an inverse projection matrix
	// and can instead calculate the result manually.
	// NOTE: we assume here that the change of size by the projection
	// is uniform for a given depth. Strictly speaking, there probably
	// might be projections that don't fulfill that. If the need
	// every arises, we could just make this a general (less-efficient)
	// 'unproject' function or just cache the inverse projection matrix.
	virtual nytl::Vec2f unprojectUnit(float depth) const = 0;

	void invalidateProjection() { projChanged_ = true; }

protected:
	Camera camera_;
	nytl::Mat4f projection_;
	nytl::Mat4f view_;

	// whether any matrix was changed since 'resetChanged' was called
	// the last time.
	bool matChanged_ {false};
	// whether the camera changed since the last matrix update.
	// used to determine whether the calculated matrices are valid or not
	bool camChanged_ {true};
	// whether the projection has changed and needs to be recalculated
	bool projChanged_ {true};

	bool flipY_ {};
	std::variant<std::monostate, Arcball, FirstPerson, Spaceship> controls_;
};

ControlledCamera::ControlledCamera(ControlType ctrl) {
	switch(ctrl) {
		case ControlType::arcball: this->useArcballControl();
		case ControlType::spaceship: this->useSpaceshipControl();
		case ControlType::firstPerson: this->useFirstPersonControl();
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

void ControlledCamera::useArcballControl(const ArcballControls& ctrls) {
	controls_ = Arcball{{}, ctrls};
}
void ControlledCamera::useFirstPersonControl(const FPCamControls& ctrls,
		const CamMoveControls& move) {
	auto con = FPCamCon::fromOrientation(camera().rot);
	controls_ = FirstPerson{con, ctrls, move};
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
	std::visit(controls_, Visitor {
		[&](Spaceship& spaceship) {
			camChanged_ |= tkn::update(camera_, spaceship.con,
				dpy, dt, spaceship.controls);
		},
		[&](FirstPerson& fp) {
			camChanged_ |= tkn::checkMovement(camera_, dpy, dt, fp.move);
		},
	});
}

void ControlledCamera::mouseMove(swa_display* dpy, nytl::Vec2i delta,
		nytl::Vec2ui winSize) {
	std::visit(controls_, Visitor {
		[&](FirstPerson& fp) {
			::tkn::mouseMove(camera_, fp.con, dpy, delta, fp.controls);
		},
		[&](Arcball& arcball) {
			auto pan = 0.5f * unprojectUnit(arcball.con.offset);
			pan.x /= winSize.x;
			pan.y /= winSize.y;
			::tkn::mouseMove(camera_, arcball.con, dpy, delta,
				arcball.controls, pan);
		},
	});
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

class PerspectiveCamera : public ControlledCamera {
public:
	static constexpr auto defaultFov = 0.5 * nytl::constants::pi;

	enum class Mode {
		normal,
		// reverse the depth buffer, i.e. projection maps near to 1.
		revDepth,
		// Like revDepth, but sets the far plane to infinity. The
		// 'far' value is ignored in this case.
		revDepthInf,
	};

public:
	PerspectiveCamera() = default;
	PerspectiveCamera(ControlType, float near, float far, Mode = Mode::normal);

	nytl::Mat4f calcProjectionMatrix() const override;
	nytl::Vec2f unprojectUnit(float) const override;

	void near(float near);
	void far(float far);
	void fov(float fov);
	void aspect(float aspect);
	void mode(Mode);

	float near() const { return near_; }
	float far() const { return far_; }
	float aspect() const { return aspect_; }
	float fov() const { return fov_; }
	Mode mode() const { return mode_; }

protected:
	Mode mode_ {};
	float near_ {};
	float far_ {};
	float aspect_ {1.f};
	float fov_ {defaultFov};
};


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


class OrthographicCamera : public ControlledCamera {
public:
	OrthographicCamera() = default;
	OrthographicCamera(ControlType,
		float width, float height, float near, float far);
	OrthographicCamera(ControlType,
		float left, float right, float bot, float top,
		float near, float far);

	void rect(float width, float height, float near, float far);
	void rect(float left, float right, float bot, float top,
		float near, float far);

	float left() const { return left_; }
	float right() const { return right_; }
	float top() const { return top_; }
	float bot() const { return bot_; }
	float near() const { return near_; }
	float far() const { return far_; }

	nytl::Mat4f calcProjectionMatrix() const override;
	nytl::Vec2f unprojectUnit(float) const override;

protected:
	float left_;
	float right_;
	float bot_;
	float top_;
	float near_;
	float far_;
};

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
