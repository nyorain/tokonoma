#pragma once

#include "buffer.hpp"
#include <rvg/fwd.hpp>
#include <rvg/context.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/mat.hpp>
#include <cstdint>

// TODO
// - allow specifying for pools whether they should be allocated on hostVisible mem
// - allow pool deferred initilization? but maybe each pool should have
//   its dedicated allocation after all

namespace rvg {

// TODO: placeholder. Query dynamically, also use descriptor indexing.
constexpr auto numTextures = 15;

// Buffer that tracks allocation and therefore does not require the size
// of the allocation when freeing.
template<typename T>
class AllocTrackedBuffer : public Buffer<T> {
public:
	using Buffer<T>::Buffer;
	unsigned allocate(unsigned count) {
		auto off = Buffer<T>::allocate(count);
		auto it = std::lower_bound(allocations_.begin(), allocations_.end(), off,
			[](auto span, auto offset) { return span.offset < offset; });
		allocations_.insert(it, {off, count});
		return off;
	}

	void free(unsigned start) {
		auto it = std::lower_bound(allocations_.begin(), allocations_.end(), start,
			[](auto span, auto start) { return span.start < start; });
		assert(it != allocations_.end() && it->start == start &&
			"No allocation for given offset");
		Buffer<T>::free(it->start, it->count);
		allocations_.erase(it);
	}

	void free(unsigned start, unsigned count) {
		// debug check that allocation exists
		auto it = std::lower_bound(allocations_.begin(), allocations_.end(), start,
			[](auto span, auto start) { return span.start < start; });
		assert(it != allocations_.end() &&
			it->start == start &&
			it->count == count &&
			"No allocation for given offset and size");
		Buffer<T>::free(start, count);
		allocations_.erase(it);
	}

protected:
	std::vector<detail::BufferSpan> allocations_;
};

class TransformPool : public Buffer<Mat4f> {
public:
	using Matrix = Mat4f;
	TransformPool(Context&);
};

class ClipPool : public Buffer<nytl::Vec3f> {
public:
	// Plane is all points x for which: dot(plane.xy, x) - plane.z == 0.
	// The inside of the plane are all points for which it's >0.
	// Everything <0 gets clipped.
	using Plane = nytl::Vec3f;
	ClipPool(Context&);
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
	PaintPool(Context& ctx);
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
	VertexPool(rvg::Context& ctx);
};

class IndexPool : public Buffer<Index> {
public:
	IndexPool(rvg::Context& ctx);
};

struct DrawCall {
	u32 transform;
	u32 paint;
	u32 clipStart;
	u32 clipCount;
	u32 indexStart;
	u32 indexCount;
	u32 type;
	float uvFadeWidth;
};

// TODO: ugly to declare them here, are mainly internal part of Scene
struct DrawCommand : DrawCall {
	u32 offset; // total offset in cmd buffer
};

struct DrawState {
	vpp::BufferSpan transformBuffer;
	vpp::BufferSpan clipBuffer;
	vpp::BufferSpan paintBuffer;
	vpp::BufferSpan vertexBuffer;
	vpp::BufferSpan indexBuffer;
	std::vector<vk::ImageView> textures;
	vk::ImageView fontAtlas;
};

bool operator==(const DrawState& a, const DrawState& b);

struct DrawSet {
	std::vector<DrawCommand> commands;
	vpp::TrDs ds;
	DrawState state;
};

class Scene;
class DrawRecorder;

class DrawRecorder {
public:
	DrawRecorder(Scene& scene);
	~DrawRecorder();

	void bindTransformBuffer(vpp::BufferSpan);
	void bindClipBuffer(vpp::BufferSpan);
	void bindPaintBuffer(vpp::BufferSpan);
	void bindVertexBuffer(vpp::BufferSpan);
	void bindIndexBuffer(vpp::BufferSpan);
	void bindFontAtlas(vk::ImageView);
	void bindTextures(std::vector<vk::ImageView>);

	void bind(TransformPool& pool) { bindTransformBuffer(pool.buffer()); }
	void bind(ClipPool& pool) { bindClipBuffer(pool.buffer()); }
	void bind(VertexPool& pool) { bindVertexBuffer(pool.buffer()); }
	void bind(IndexPool& pool) { bindIndexBuffer(pool.buffer()); }
	void bind(PaintPool& pool) {
		bindPaintBuffer(pool.buffer());
		bindTextures(pool.textures());
	};

	// TODO
	void bind(FontAtlas&);

	void draw(const DrawCall&);

protected:
	DrawSet current_;
	DrawSet pending_;
	Scene& scene_;
};

class Scene {
public:
	Scene(Context& ctx);

	void recordDraw(vk::CommandBuffer cb);
	bool updateDevice();

	Context& context() const { return *context_; }
	[[nodiscard]] DrawRecorder record() {
		numDraws_ = 0u;
		numDrawCalls_ = 0u;
		return {*this};
	}

	const auto& dsLayout() const { return dsl_; }
	const auto& pipeLayout() const { return pipeLayout_; }

	// TODO, placeholder. Support descriptor indexing extension
	bool dynamicDescriptors() const { return false; }

protected:
	struct DrawCommandData {
		u32 transform;
		u32 paint;
		u32 clipStart;
		u32 clipCount;
		u32 type;
		float uvFadeWidth;
		u32 _pad[2];
	};

	struct Update {
		unsigned draw; // draw id
		unsigned command; // command id (inside draw)
		unsigned offset; // total command id
	};

	friend class DrawRecorder;
	void add(DrawSet);
	void finish() {
		draws_.resize(numDraws_);
	}

protected:
	unsigned numDraws_ {};
	unsigned numDrawCalls_ {};

	// TODO: move to context
	vpp::TrDsLayout dsl_;
	vpp::PipelineLayout pipeLayout_;

	std::vector<Update> cmdUpdates_;
	std::vector<unsigned> dsUpdates_;
	std::vector<DrawSet> draws_;
	// NOTE: we might be able to merge those 2 buffers into one
	vpp::SubBuffer indirectCmdBuffer_;
	vpp::SubBuffer ownCmdBuffer_;

	Context* context_ {};
};

} // namespace rvg

