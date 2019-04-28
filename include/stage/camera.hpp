#pragma once

#include <stage/transform.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <ny/fwd.hpp>

namespace doi {

// Simple perspective 3D matrix (rh coordinate system)
struct Camera {
	bool update = true; // set when changed
	nytl::Vec3f pos {0.f, 0.f, 3.f};
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
	return doi::perspective3RH<float>(p.fov, p.aspect, p.near, p.far);
}

inline auto fixedMatrix(const Camera& c) {
	return projection(c) * doi::lookAtRH({}, c.dir, c.up);
}

inline auto matrix(const Camera& c) {
	return projection(c) * doi::lookAtRH(c.pos, c.pos + c.dir, c.up);
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

} // namespace doi
