#pragma once

#include <tkn/types.hpp>
#include <nytl/vec.hpp>
#include <nytl/span.hpp>
#include <vector>

namespace tkn {

struct Cube {
	Vec3f pos {0.f, 0.f, 0.f}; // center
	Vec3f size {1.f, 1.f, 1.f}; // total size
};

struct Sphere {
	Vec3f pos {0.f, 0.f, 0.f}; // center
	Vec3f radius {1.f, 1.f, 1.f};
};

struct Shape {
	std::vector<Vec3f> positions;
	std::vector<Vec3f> normals;
	std::vector<u32> indices;
};

Shape generate(const Cube& cube);
Shape generateUV(const Sphere& sphere, unsigned stackCount = 64,
	unsigned sectorCount = 64);

Shape generateQuad(nytl::Vec3f center, nytl::Vec3f x, nytl::Vec3f y);

// TODO: allows position and radius as well
Shape generateIco(unsigned subdiv);

// generate smooth normals weighted by triangle area
std::vector<Vec3f> areaSmoothNormals(nytl::Span<const Vec3f> positions,
	nytl::Span<const u32> indices);

// TODO: add (dual-)cube marching. See tkn/volume

} // namespace tkn
