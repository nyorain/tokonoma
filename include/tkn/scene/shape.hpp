#pragma once

#include <tkn/types.hpp>
#include <nytl/vec.hpp>
#include <nytl/span.hpp>
#include <vector>

namespace tkn {

struct Cube {
	Vec3f pos;
	Vec3f size; // total size
};

struct Sphere {
	Vec3f pos;
	Vec3f radius;
};

struct Shape {
	std::vector<Vec3f> positions;
	std::vector<Vec3f> normals;
	std::vector<u32> indices;
};

Shape generate(const Cube& cube);
Shape generateUV(const Sphere& sphere, unsigned stackCount = 64,
	unsigned sectorCount = 64);

// generate smooth normals weighted by triangle area
std::vector<Vec3f> areaSmoothNormals(nytl::Span<const Vec3f> positions,
	nytl::Span<const u32> indices);

} // namespace tkn