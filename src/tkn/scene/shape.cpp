#include <tkn/scene/shape.hpp>
#include <nytl/vecOps.hpp>
#include <dlg/dlg.hpp>

namespace tkn {
using namespace nytl::vec::cw;

// generates counter-clockwise primitives (right handed coord system)
Shape generate(const Cube& cube) {
	Shape shape;
	shape.indices.reserve(24);
	shape.positions.reserve(24);
	shape.normals.reserve(24);

	// write indices
	for(auto i = 0u; i < 24; i += 4) {
		shape.indices.push_back(i + 0);
		shape.indices.push_back(i + 1);
		shape.indices.push_back(i + 2);

		shape.indices.push_back(i + 0);
		shape.indices.push_back(i + 2);
		shape.indices.push_back(i + 3);
	}

	// write positions and normals
	for(auto i = 0u; i < 3u; ++i) {
		auto n = 0.5f * multiply(cube.size, nytl::Vec3f{
			float(i == 0),
			float(i == 1),
			float(i == 2)
		});
		auto nn = nytl::normalized(n);

		auto x = 0.5f * multiply(cube.size, nytl::Vec3f{
			float(i == 1),
			float(i == 2),
			float(i == 0)
		});

		auto y = 0.5f * multiply(cube.size, nytl::Vec3f{
			float(i == 2),
			float(i == 0),
			float(i == 1)
		});

		auto pos = cube.pos + n;
		shape.positions.push_back(pos - x + y);
		shape.positions.push_back(pos + x + y);
		shape.positions.push_back(pos + x - y);
		shape.positions.push_back(pos - x - y);
		for(auto j = 0u; j < 4; ++j) {
			shape.normals.push_back(nn);
		}

		// mirrored side
		n *= -1;
		nn *= -1;

		pos = cube.pos + n;
		shape.positions.push_back(pos - x - y);
		shape.positions.push_back(pos + x - y);
		shape.positions.push_back(pos + x + y);
		shape.positions.push_back(pos - x + y);
		for(auto j = 0u; j < 4; ++j) {
			shape.normals.push_back(nn);
		}
	}

	return shape;
}

// http://www.songho.ca/opengl/gl_sphere.html
Shape generateUV(const Sphere& sphere, unsigned stackCount,
		unsigned sectorCount) {

	using nytl::constants::pi;
	Shape shape;

	auto stackStep = pi / stackCount;
	auto sectorStep = 2 * pi / sectorCount;

	// normals and positions
	shape.positions.reserve(sectorCount * stackCount);
	shape.normals.reserve(sectorCount * stackCount);
	for(auto i = 0u; i <= stackCount; ++i) {
		auto stackAngle = pi / 2 - i * stackStep; // [-pi/2, pi/2]
		auto xz = std::cos(stackAngle);
		float y = std::sin(stackAngle);
		for(auto j = 0u; j <= sectorCount; ++j) {
			auto sectorAngle = j * sectorStep;

			float x = xz * std::sin(sectorAngle);
			float z = xz * std::cos(sectorAngle);
			nytl::Vec3f off {x, y, z};
			off = multiply(sphere.radius, off);
			shape.normals.push_back(nytl::normalized(off));
			shape.positions.push_back(sphere.pos + off);
		}
	}

	// indices
	shape.indices.reserve(sectorCount * stackCount);
	for(auto i = 0u; i < stackCount; ++i) {
		auto k1 = i * (sectorCount + 1);
		auto k2 = k1 + sectorCount + 1;

		for(auto j = 0u; j < sectorCount; ++j, ++k1, ++k2) {
			// NOTE: order here is important, different from most
			// references. We do it like this so subdivision (see
			// e.g. subd project) is consistent and doesn't produce
			// t-junctions on the edge of the both triangles
			if(i != 0) {
				shape.indices.push_back(k1 + 1);
				shape.indices.push_back(k1);
				shape.indices.push_back(k2);
			}

			if(i != (stackCount - 1)) {
				shape.indices.push_back(k2);
				shape.indices.push_back(k2 + 1);
				shape.indices.push_back(k1 + 1);
			}
		}
	}

	return shape;
}

std::vector<Vec3f> areaSmoothNormals(nytl::Span<const Vec3f> positions,
		nytl::Span<const u32> indices) {

	std::vector<Vec3f> normals;
	normals.resize(positions.size(), {0.f, 0.f, 0.f});
	for(auto i = 0u; i < indices.size(); i += 3) {
		dlg_assert(indices[i + 0] < positions.size());
		dlg_assert(indices[i + 1] < positions.size());
		dlg_assert(indices[i + 2] < positions.size());
		auto e1 = positions[indices[i + 1]] - positions[indices[i + 0]];
		auto e2 = positions[indices[i + 2]] - positions[indices[i + 0]];
		auto normal = nytl::cross(e1, e2);
		normals[indices[i + 0]] += normal;
		normals[indices[i + 1]] += normal;
		normals[indices[i + 2]] += normal;
	}

	// normalize all
	for(auto& n : normals) {
		normalize(n);
	}

	return normals;
}

// TODO: fix this for subd, i.e. fix triangle vertex numbering
// such that hypoth (between vert b and c) of neighbors is the same.
Shape generateIco(unsigned subdiv) {
	// step 1: base ico
	constexpr auto hStep = float(nytl::radians(72));
	constexpr auto z = float(0.447213595499); // sin(atan(0.5f))
	constexpr auto xy = float(0.894427191); // cos(atan(0.5f))
	constexpr auto max = 11u; // number of base vertices

	std::vector<nytl::Vec3f> verts;
	verts.reserve(12 + 12 * std::exp2(subdiv));

	std::vector<u32> inds;
	inds.reserve(12);
	auto addTri = [](auto& inds, u32 a, u32 b, u32 c) {
		inds.push_back(a);
		inds.push_back(b);
		inds.push_back(c);
	};

	auto addVert = [&](Vec3f v) {
		verts.push_back(v);
		return verts.size() - 1;
	};

	verts.push_back({0.f, 0.f, 1.f});

	for(auto i = 0u; i < 5; ++i) {
		// top vertex
		float h1 = i * hStep;
		auto v1 = addVert({xy * std::cos(h1), xy * std::sin(h1), z});

		// bottom vertex
		float h2 = h1 + 0.5f * hStep;
		auto v2 = addVert({xy * std::cos(h2), xy * std::sin(h2), -z});

		auto nextTop = 1 + 2 * ((i + 1) % 5);
		auto nextBot = 2 + 2 * ((i + 1) % 5);

		addTri(inds, 0, v1, nextTop);
		addTri(inds, v1, v2, nextTop);
		addTri(inds, v2, nextBot, nextTop);
		addTri(inds, max, nextBot, v2);
	}

	verts.push_back(Vec3f{0.f, 0.f, -1.f});

	// step 2: subdivide!
	// NOTE: allocation optimization: use ping-pong with pre-max-allocated
	// index buffers
	for(auto i = 0u; i < subdiv; ++i) {
		std::vector<u32> ninds;
		ninds.reserve(inds.size() * 4);

		for(auto i = 0u; i < inds.size(); i += 3) {
			auto ia = inds[i + 0];
			auto ib = inds[i + 1];
			auto ic = inds[i + 2];

			auto a = verts[ia];
			auto b = verts[ib];
			auto c = verts[ic];

			/*       a
			 *      / \
			 *  m1 *---* m3
			 *    / \ / \
			 *   b---*---c
			 *       m2      */
			auto m1 = normalized(0.5f * (a + b));
			auto m2 = normalized(0.5f * (b + c));
			auto m3 = normalized(0.5f * (c + a));
			auto i1 = addVert(m1);
			auto i2 = addVert(m2);
			auto i3 = addVert(m3);

			addTri(ninds, ia, i1, i3); // a-m1-m3
			addTri(ninds, i1, ib, i2); // m1-b-m2
			addTri(ninds, i3, i2, ic); // m3-m2-c
			addTri(ninds, i1, i2, i3); // middle one, m1-m2-m3
		}

		inds = std::move(ninds);
	}

	// NOTE: for (origin-centered unit) spheres, normals and positions
	// are the same
	return {verts, verts, inds};
}

Shape generateQuad(nytl::Vec3f center, nytl::Vec3f x, nytl::Vec3f y) {
	Shape ret;

	ret.positions.reserve(4);
	ret.positions.push_back(center - x - y);
	ret.positions.push_back(center + x - y);
	ret.positions.push_back(center + x + y);
	ret.positions.push_back(center - x + y);

	ret.indices = {0, 1, 2, 0, 2, 3};

	auto normal = normalized(cross(x, y));
	ret.normals = {normal, normal, normal, normal};

	return ret;
}

} // namespace tkn
