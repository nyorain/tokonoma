#include <tkn/transform.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

nytl::Mat4f cubeProjectionVP(nytl::Vec3f pos, unsigned face,
		float near, float far) {
	auto& f = cubemap::faces[face];
	nytl::Mat4f view = nytl::identity<4, float>();
	view[0] = nytl::Vec4f(f.s);
	view[1] = nytl::Vec4f(f.t);
	view[2] = -nytl::Vec4f(f.dir);

	view[0][3] = -dot(f.s, pos);
	view[1][3] = -dot(f.t, pos);
	view[2][3] = dot(f.dir, pos);

	auto fov = 0.5 * nytl::constants::pi;
	auto aspect = 1.f;
	auto mat = tkn::perspective<float>(fov, aspect, near, far);
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

namespace cubemap {

nytl::Vec3f faceUVToDir(unsigned face, float u, float v) {
	u = 2 * u - 1;
	v = 2 * v - 1;
	auto& data = cubemap::faces[face];
	return data.dir + u * data.s + v * data.t;
}

} // namespace cubemap
} // namespace tkn
