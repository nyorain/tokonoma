#pragma once

#include <nytl/vec.hpp>
#include <vector>

namespace doi {

struct Cube {
	nytl::Vec3f pos;
	nytl::Vec3f size; // total size
};

struct Sphere {
	nytl::Vec3f pos;
	nytl::Vec3f radius;
};

struct Shape {
	std::vector<nytl::Vec3f> positions;
	std::vector<nytl::Vec3f> normals;
	std::vector<std::uint32_t> indices;
};

Shape generate(const Cube& cube);
Shape generateUV(const Sphere& sphere, unsigned stackCount = 64,
	unsigned sectorCount = 64);

} // namespace doi
