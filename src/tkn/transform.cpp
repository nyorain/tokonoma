#include <tkn/transform.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

nytl::Mat4f cubeProjectionVP(nytl::Vec3f pos, unsigned face,
		float near, float far) {
	// y sign flipped everywhere
	// TODO: not sure why slightly different to pbr.cpp
	// (positive, negative y swapped), probably bug in pbr shaders
	constexpr struct CubeFace {
		nytl::Vec3f x;
		nytl::Vec3f y;
		nytl::Vec3f z; // direction of the face
	} faces[] = {
		{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
		{{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}},
		{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
		{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}},
		{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
		{{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
	};

	auto& f = faces[face];
	dlg_assertm(nytl::cross(f.x, f.y) == f.z,
		"{} {}", nytl::cross(f.x, f.y), f.z);

	nytl::Mat4f view = nytl::identity<4, float>();
	view[0] = nytl::Vec4f(f.x);
	view[1] = nytl::Vec4f(f.y);
	view[2] = -nytl::Vec4f(f.z);

	view[0][3] = -dot(f.x, pos);
	view[1][3] = -dot(f.y, pos);
	view[2][3] = dot(f.z, pos);

	auto fov = 0.5 * nytl::constants::pi;
	auto aspect = 1.f;
	auto mat = tkn::perspective<float>(fov, aspect, -near, -far);
	return mat * view;
}

Frustum ndcFrustum() {
	return {{
		{-1.f, 1.f, 0.f},
		{1.f, 1.f, 0.f},
		{1.f, -1.f, 0.f},
		{-1.f, -1.f, 0.f},
		{-1.f, 1.f, 1.f},
		{1.f, 1.f, 1.f},
		{1.f, -1.f, 1.f},
		{-1.f, -1.f, 1.f},
	}};
}

} // namespace tkn
