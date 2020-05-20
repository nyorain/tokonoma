#pragma once

#include <tkn/camera2.hpp>
#include <tkn/transform.hpp>
#include <variant>

// High-level and easy-to-use camera concept that (in comparison
// to the low-level lightweight tkn::Camera) fully manages projection
// and input/controls management.

// TODO: is caching the matrices even worth it? seriously, computing view
// and projection matrices is not expensive at all, it's like
// one transcendental function each and nothing else really

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
	ControlledCamera(ControlType);

	nytl::Mat4f viewMatrix();
	nytl::Mat4f projectionMatrix();
	nytl::Mat4f viewProjectionMatrix(); // projection * view

	// NOTE: these are not cached
	nytl::Mat4f fixedViewMatrix(); // view matrix without translation
	nytl::Mat4f fixedViewProjectionMatrix(); // projection * fixedView

	void position(nytl::Vec3f pos);
	void orientation(const Quaternion& rot);
	void lookAt(nytl::Vec3f pos, nytl::Vec3f dir, nytl::Vec3f up = defaultUp);

	const Camera& camera() const { return camera_; }
	Quaternion orientation() const { return camera_.rot; }
	nytl::Vec3f position() const { return camera_.pos; }
	nytl::Vec3f dir() const { return tkn::dir(camera_); }
	nytl::Vec3f up() const { return tkn::right(camera_); }
	nytl::Vec3f right() const { return tkn::up(camera_); }

	ControlType controlType() const;
	void useControl(ControlType type); // default contructs controls

	std::optional<Arcball*> arcballControl();
	std::optional<FirstPerson*> firstPersonControl();
	std::optional<Spaceship*> spaceshipControl();

	void disableControl();
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

	bool flipY_ {true};
	std::variant<std::monostate, Arcball, FirstPerson, Spaceship> controls_;
};

// ControlledCamera with a perspective projection
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

// ControlledCamera with an orthographic projection.
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

} // namespace tkn

