#pragma once

#include <tkn/types.hpp>
#include <tkn/quaternion.hpp>

// NOTE: not really sure what this is useful for.
// do we every really need such a description? You can easily
// build the cam/world frustum from just the (inverted) projection or
// VP matrix

namespace tkn {

// Structure able to describe all commonly used projections.
struct Projection {
	enum class Mode {
		// normal depth mapping, the near plane is mapped to 0, far to 1.
		persp,
		// reverse the depth buffer, i.e. projection maps near to 1.
		perspRevDepth,
		// Like revDepth, but sets the far plane to infinity. The
		// 'far' value is ignored in this case.
		perspRevDepthInf,
		// Orthographic projection
		orthographic,
	};

	// Positions of near and far planes on z axis.
	// Sign matters here, should be negative when the near and far
	// planes are on the negative z axis.
	float near;
	float far;

	// Frustum coordinates on the near plane, always parallel to the xy plane.
	float left;
	float right;
	float bot;
	float top;

	// How the values between near and far are mapped to the depth buffer.
	Mode mode;


	[[nodiscard]] static Projection
		perspective(float fov, float aspect, float near, float far);
	[[nodiscard]] static Projection
		perspectiveRev(float fov, float aspect, float near, float far);
	[[nodiscard]] static Projection
		perspectiveRevInf(float fov, float aspect, float near, float far);

	[[nodiscard]] static Projection orthographicBox(float left, float right,
		float bot, float top, float near, float far);
	[[nodiscard]] static Projection orthographicSym(float width, float height,
		float near, float far);
	[[nodiscard]] static Projection orthographicRatio(float size, float aspect,
		float near, float far);
};

// Full and easy to interpret description of a camera (frustum): its view and
// projection transformations. More descriptive than e.g. just a transformation
// matrix.
struct CameraInfo {
	Vec3f position;
	Quaternion orientation;
	Projection projection;
};

using Frustum = std::array<nytl::Vec3f, 8>;
Frustum ndcFrustum(); // frustum in ndc space, i.e. [-1, 1]^3

Frustum frustum(const Projection& proj);
Frustum frustum(const CameraInfo& info);

} // namespace tkn
