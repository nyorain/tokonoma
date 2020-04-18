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
#include <swa/swa.h>

namespace tkn {

struct Camera {
	static constexpr auto up = nytl::Vec3f{0.f, 1.f, 0.f};

	bool update = true; // set when changed
	nytl::Vec3f pos {0.f, 0.f, 0.f};
	float yaw {0.f};
	float pitch {0.f};
};

inline nytl::Vec3f dir(const Camera& c) {
	// normalized by construction via cos^2 + sin^2 = 1
	float x = std::sin(c.yaw) * std::cos(c.pitch);
	float y = -std::sin(c.pitch);
	float z = -std::cos(c.yaw) * std::cos(c.pitch);
	return {x, y, z};
}

inline auto viewMatrix(const Camera& c) {
	return tkn::lookAtRH(c.pos, c.pos + dir(c), c.up);
}

inline auto fixedViewMatrix(const Camera& c) {
	return tkn::lookAtRH({}, dir(c), c.up);
}

inline void rotateView(Camera& c, float dyaw, float dpitch) {
	using nytl::constants::pi;
	c.yaw += dyaw;
	c.pitch += dpitch;
	c.pitch = std::clamp<float>(c.pitch, -pi / 2 + 0.1, pi / 2 - 0.1);
	c.update = true;
}

// checks default wasd+qe movement
// returns whether a change was made
bool checkMovement(Camera& c, ny::KeyboardContext& kc, float dt);
bool checkMovement(Camera& c, swa_display* dpy, float dt);

// returns the view projection matrix to render a cubemap from position 'pos'
// for face 'i'. Aspect is assumed to be 1.
nytl::Mat4f cubeProjectionVP(nytl::Vec3f pos, unsigned face,
	float near = 0.01f, float far = 30.f);

// order:
// front/near (topleft, topright, bottomleft, bottomright)
// back/far (topleft, topright, bottomleft, bottomright)
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

	float positionMultiplier = 1.f;
};

void init(TouchCameraController&, tkn::Camera& cam, rvg::Context& rvgctx);
void touchBegin(TouchCameraController&, unsigned id, nytl::Vec2f pos,
	nytl::Vec2ui windowSize);
void touchEnd(TouchCameraController&, unsigned id);
void touchUpdate(TouchCameraController&, unsigned id, nytl::Vec2f pos);
void update(TouchCameraController&, double dt);

} // namespace tkn
