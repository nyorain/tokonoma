#pragma once

#include "context.hpp"
#include "buffer.hpp"
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/mat.hpp>
#include <cstdint>
#include <variant>

// TODO
// - allow pool deferred initilization? but maybe each pool should have
//   its dedicated allocation after all

namespace rvg2 {

using namespace nytl;

class TransformPool : public Buffer<Mat4f> {
public:
	using Matrix = Mat4f;
	TransformPool(Context&, DevMemBits = DevMemType::deviceLocal);
};

class ClipPool : public Buffer<nytl::Vec3f> {
public:
	// Plane is all points x for which: dot(plane.xy, x) - plane.z == 0.
	// The inside of the plane are all points for which it's >0.
	// Everything <0 gets clipped.
	using Plane = nytl::Vec3f;
	ClipPool(Context&, DevMemBits = DevMemType::deviceLocal);
};

// TODO: rename
struct PaintData2 {
	Vec4f inner;
	Vec4f outer;
	Vec4f custom;
	nytl::Mat4f transform; // mat3 + additional data
};

class PaintPool : public Buffer<PaintData2> {
public:
	using PaintData = PaintData2;
	PaintPool(Context& ctx, DevMemBits = DevMemType::deviceLocal);

	void setTexture(unsigned i, vk::ImageView);
	const auto& textures() const { return textures_; }

protected:
	std::vector<vk::ImageView> textures_;
};

using Index = u32;
struct Vertex {
	Vec2f pos; // NOTE: we could probably use 16 bit floats instead
	Vec2f uv; // NOTE: try using 8 or 16 bit snorm, or 16bit float instead
	Vec4u8 color;
};

class VertexPool : public Buffer<Vertex> {
public:
	VertexPool(Context& ctx, DevMemBits = DevMemType::deviceLocal);
};

class IndexPool : public Buffer<Index> {
public:
	IndexPool(Context& ctx, DevMemBits = DevMemType::deviceLocal);
};

// TODO
// Creates the clip planes for a (closed) polygon. Can even be concave.
// The number of clip planes created is the same as the number of points.
// TODO: add rect constructor. Or add RectClip?
class PolygonClip {
public:
	PolygonClip(ClipPool&, std::vector<Vec2f> points);
	~PolygonClip();

	const std::vector<Vec2f> points() const { return points_; }
	unsigned clipStart() const { return id_; }
	unsigned clipCount() const { return points_.size(); }

private:
	ClipPool* pool_ {};
	std::vector<Vec2f> points_;
	unsigned id_ {};
};

// TODO
//std::vector<Vec2f> polygonIntersection(Span<const Vec2f> a, Span<const Vec2f> b);
//std::vector<Vec2f> polygonUnion(Span<const Vec2f> a, Span<const Vec2f> b);

// TODO
struct Transform {
	TransformPool* pool;
	unsigned id;
};

} // namespace rvg2

