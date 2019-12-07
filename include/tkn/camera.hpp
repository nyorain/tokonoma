#pragma once

#include <tkn/transform.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <rvg/shapes.hpp>
#include <rvg/paint.hpp>
#include <ny/fwd.hpp>
#include <ny/event.hpp>

namespace tkn {

// Simple perspective 3D matrix (rh coordinate system)
struct Camera {
	bool update = true; // set when changed
	nytl::Vec3f pos {0.f, 0.f, 2.f};
	nytl::Vec3f dir {0.f, 0.f, -1.f};
	nytl::Vec3f up {0.f, 1.f, 0.f};

	float yaw {0.f};
	float pitch {0.f};

	struct {
		float fov = 0.48 * nytl::constants::pi;
		float aspect = 1.f;
		float near = 0.01f;
		float far = 30.f;
	} perspective;
};

inline auto projection(const Camera& c) {
	auto& p = c.perspective;
	return tkn::perspective3RH<float>(p.fov, p.aspect, p.near, p.far);
}

inline auto fixedMatrix(const Camera& c) {
	return projection(c) * tkn::lookAtRH({}, c.dir, c.up);
}

inline auto matrix(const Camera& c) {
	return projection(c) * tkn::lookAtRH(c.pos, c.pos + c.dir, c.up);
}

inline void rotateView(Camera& c, float dyaw, float dpitch) {
	using nytl::constants::pi;
	c.yaw += dyaw;
	c.pitch += dpitch;
	c.pitch = std::clamp<float>(c.pitch, -pi / 2 + 0.1, pi / 2 - 0.1);

	c.dir.x = std::sin(c.yaw) * std::cos(c.pitch);
	c.dir.y = -std::sin(c.pitch);
	c.dir.z = -std::cos(c.yaw) * std::cos(c.pitch);
	nytl::normalize(c.dir);
	c.update = true;
}

// checks default wasd+qe movement
// returns whether a change was made
bool checkMovement(Camera& c, ny::KeyboardContext& kc, float dt);

// returns the view projection matrix to render a cubemap from position 'pos'
// for face 'i'. Aspect is assumed to be 1.
nytl::Mat4f cubeProjectionVP(nytl::Vec3f pos, unsigned face,
	float near = 0.01f, float far = 30.f);

// order:
// front (topleft, topright, bottomleft, bottomright)
// back (topleft, topright, bottomleft, bottomright)
using Frustum = std::array<nytl::Vec3f, 8>;
Frustum ndcFrustum(); // frustum in ndc space, i.e. [-1, 1]^3

struct TouchCameraController {
	static constexpr auto invalidID = 0xFFFFFFFFu;
	struct TouchPoint {
		unsigned id = invalidID;
		nytl::Vec2f pos;
		nytl::Vec2f start; // only for alt
		rvg::CircleShape circle;
	};

	tkn::Camera* cam;
	TouchPoint move;
	TouchPoint rotate;
	bool alt = true; // whether we use alternative controls
	rvg::Paint paint;
};

void init(TouchCameraController&, tkn::Camera& cam, rvg::Context& rvgctx);
void touchBegin(TouchCameraController&, const ny::TouchBeginEvent&,
	nytl::Vec2ui windowSize);
void touchEnd(TouchCameraController&, const ny::TouchEndEvent& ev);
void touchUpdate(TouchCameraController&, const ny::TouchUpdateEvent& ev);
void update(TouchCameraController&, double dt);

} // namespace tkn
