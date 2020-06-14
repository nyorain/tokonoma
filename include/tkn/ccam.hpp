#pragma once

#include <tkn/types.hpp>
#include <tkn/camera2.hpp>
#include <tkn/transform.hpp>
#include <variant>
#include <optional>

// High-level and easy-to-use camera concept that (in comparison
// to the low-level lightweight tkn::Camera) fully manages projection
// and input/controls management. For tasks like offscreen render
// views and automatically controlled cameras or cameras that need
// special projections (non symmetric off-center projections,
// splitted multi-frustum rendering) you better use tkn::Camera directly.

// TODO: use positive near and far values since all the spaces are
//   managed internally?

namespace tkn {

class ControlledCamera {
public:
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
		float zoomFac {1.05};
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

	enum class PerspectiveMode {
		// normal depth mapping, the near plane is mapped to 0, far to 1.
		normal,
		// reverse the depth buffer, i.e. projection maps near to 1.
		revDepth,
		// Like revDepth, but sets the far plane to infinity. The
		// 'far' value is ignored in this case.
		revDepthInf,
	};

	struct Perspective {
		float near;
		float far;
		float aspect;
		float fov;
		PerspectiveMode mode;
	};

	struct Orthographic {
		float near;
		float far;
		float aspect;
		float maxSize;
	};

	static constexpr auto defaultUp = nytl::Vec3f{0.f, 1.f, 0.f};
	static constexpr auto defaultNear = -0.01f;
	static constexpr auto defaultFar = -100.f;

	static constexpr auto defaultPerspective = Perspective{
		defaultNear, defaultFar,
		1.f, 0.5 * nytl::constants::pi,
		// NOTE: revDepthInf is better but most applications
		// use this already (and for most applications it does not
		// really make a difference, anyways)
		PerspectiveMode::normal
	};

	static constexpr auto defaultOrtho = Orthographic{
		defaultNear, defaultFar,
		1.f, 50.f
	};

	// This will be set to true by all operations that modify the
	// view or projection of this camera. The application can reset it
	// to false everytime it updates its states based on the camera
	// and wants to reset the state-change tracking.
	bool needsUpdate {true};

public:
	explicit ControlledCamera(ControlType = ControlType::firstPerson,
		const Perspective& = defaultPerspective);
	explicit ControlledCamera(ControlType, const Orthographic&);

	nytl::Mat4f viewMatrix() const;
	nytl::Mat4f projectionMatrix() const;
	nytl::Mat4f viewProjectionMatrix() const; // projection * view

	nytl::Mat4f fixedViewMatrix() const; // view matrix without translation
	nytl::Mat4f fixedViewProjectionMatrix() const; // projection * fixedView

	// - view -
	void position(nytl::Vec3f pos);
	void orientation(const Quaternion& rot);
	void lookAt(nytl::Vec3f pos, nytl::Vec3f dir, nytl::Vec3f up = defaultUp);

	const Camera& camera() const { return camera_; }
	Quaternion orientation() const { return camera_.rot; }
	nytl::Vec3f position() const { return camera_.pos; }
	nytl::Vec3f dir() const { return tkn::dir(camera_); }
	nytl::Vec3f up() const { return tkn::up(camera_); }
	nytl::Vec3f right() const { return tkn::right(camera_); }

	// - controls -
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

	// - projection-
	void orthographic(const Orthographic& = defaultOrtho);
	void perspective(const Perspective& = defaultPerspective);

	std::optional<Orthographic> orthographic() const;
	std::optional<Perspective> perspective() const;

	bool isOrthographic() const;

	void near(float);
	void far(float);
	void aspect(float);
	void aspect(nytl::Vec2ui windowSize);
	void perspectiveFov(float);
	void perspectiveMode(PerspectiveMode);
	void orthoSize(float);

	float near() const;
	float far() const;
	float aspect() const;
	std::optional<float> perspectiveFov() const;
	std::optional<float> orthoSize() const;
	std::optional<PerspectiveMode> perspectiveMode();

	// Whether to flip the y coordinates at the end.
	// Alternative to setting negative viewport height and y = +height
	// with vulkan rendering. True by default.
	bool flipY() const { return flipY_; }
	void flipY(bool);

	// Returns the size of an unprojected unit square (size (1, 1)) at
	// the given depth. Basically answers: "by how far do i have to move
	// something in pre-projection space so it moves by (1, 1) in projection
	// space, given its depth".
	nytl::Vec2f unprojectUnit(float depth) const;

protected:
	Camera camera_; // orientation + position
	bool flipY_ {true};
	std::variant<std::monostate, Arcball, FirstPerson, Spaceship> controls_;
	std::variant<Perspective, Orthographic> projection_ {Perspective {}};
};

} // namespace tkn

