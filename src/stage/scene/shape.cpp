#include <stage/scene/shape.hpp>
#include <nytl/vecOps.hpp>
#include <dlg/dlg.hpp>

namespace doi {
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
			if(i != 0) {
				shape.indices.push_back(k1);
				shape.indices.push_back(k2);
				shape.indices.push_back(k1 + 1);
			}

			if(i != (stackCount - 1)) {
				shape.indices.push_back(k1 + 1);
				shape.indices.push_back(k2);
				shape.indices.push_back(k2 + 1);
			}
		}
	}

	return shape;
}

// TODO
// Shape generateIco(const Sphere& sphere, unsigned sub) {
// }

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

} // namespace doi
