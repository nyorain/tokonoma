#pragma once

#include "context.hpp"
#include "buffer.hpp"
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
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

class ClipPool : public Buffer<nytl::Vec4f> {
public:
	// Plane is all points x for which: dot(plane.xy, x) - plane.z == 0.
	// The inside of the plane are all points for which it's >0.
	// Everything <0 gets clipped. Fourth component is unused, needed
	// for padding.
	using Plane = nytl::Vec4f;
	ClipPool(Context&, DevMemBits = DevMemType::deviceLocal);
};

struct PaintData {
	Vec4f inner;
	Vec4f outer;
	Vec4f custom;
	nytl::Mat4f transform; // mat3 + additional data
};

class PaintPool : public Buffer<PaintData> {
public:
	PaintPool(Context& ctx, DevMemBits = DevMemType::deviceLocal);

	void setTexture(unsigned i, vk::ImageView);
	const auto& textures() const { return textures_; }

	bool updateDevice() {
		bool rerec = texturesChanged_;
		rerec |= Buffer::updateDevice();
		return rerec;
	}

	// Returns the first free texture slot.
	// If there is none, returns 'invalid'.
	// This does not actually allocate something, it can be used to
	// check whether this pool has free texture slots left.
	u32 freeTextureSlot() const;

protected:
	std::vector<vk::ImageView> textures_;
	bool texturesChanged_ {};
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

std::vector<Vec4f> polygonClip(nytl::Span<const Vec2f> points, bool close);
std::vector<Vec4f> rectClip(Vec2f position, Vec2f size);

class Clip {
public:
	Clip() = default;
	Clip(ClipPool&, nytl::Span<const Vec4f> planes);
	~Clip();

	Clip(Clip&& rhs) noexcept { swap(*this, rhs); }
	Clip& operator=(Clip rhs) noexcept {
		swap(*this, rhs);
		return *this;
	}

	unsigned clipStart() const { return id_; }
	unsigned clipCount() const { return count_; }

	nytl::Span<const Vec4f> planes() const { return pool_->read(id_, count_); }
	void planes(nytl::Span<const Vec4f>);
	nytl::Span<Vec4f> change() { return pool_->writable(id_, count_); }

	bool valid() const { return pool_ && id_ != invalid && count_ > 0; }
	auto* pool() const { return pool_; }

	friend void swap(Clip&, Clip&);

private:
	ClipPool* pool_ {};
	u32 id_ {invalid};
	u32 count_ {0};
};

class Transform {
public:
	using Matrix = TransformPool::Matrix;

public:
	Transform() = default;
	Transform(TransformPool& pool, const Matrix& = nytl::identity<4, float>());
	~Transform();

	Transform(Transform&& rhs) noexcept { swap(*this, rhs); }
	Transform& operator=(Transform rhs) noexcept {
		swap(*this, rhs);
		return *this;
	}

	const Matrix& matrix() const { return pool_->read(id_); }
	void matrix(const Matrix& m) { pool_->write(id_, m); }
	Matrix& change() { return pool_->writable(id_); }

	bool valid() const { return pool_ && id_ != invalid; }
	auto* pool() const { return pool_; }
	auto id() const { return id_; }

	friend void swap(Transform& a, Transform& b);

protected:
	TransformPool* pool_ {};
	u32 id_ {invalid};
};

// Allocating a Paint with a texture on a PaintPool that does not have a free
// texture slot will result in an exception. Just make sure that never
// happens.
class Paint {
public:
	Paint() = default;

	// TODO: add pool-only constructor? with a default color?
	Paint(PaintPool& pool, const PaintData&, vk::ImageView = {});
	~Paint();

	Paint(Paint&& rhs) noexcept { swap(*this, rhs); }
	Paint& operator=(Paint rhs) noexcept {
		swap(*this, rhs);
		return *this;
	}

	const PaintData& data() const { return pool_->read(id_); }
	PaintData& changeData() { return pool_->writable(id_); }
	void data(const PaintData& data, vk::ImageView = {});
	void texture(vk::ImageView);

	auto* pool() const { return pool_; }
	bool valid() const { return pool_ && id_ != invalid; }
	auto id() const { return id_; }
	auto texID() const { return texID_; }
	vk::ImageView texture() const;

	friend void swap(Paint& a, Paint& b);

protected:
	PaintPool* pool_ {};
	u32 id_ {invalid};
	u32 texID_ {invalid};
};

} // namespace rvg2

