#pragma once

#include "buffer.hpp"
#include <rvg/fwd.hpp>
#include <rvg/context.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/mat.hpp>
#include <cstdint>

namespace rvg {

// TODO: placeholder. Query dynamically, also use descriptor indexing.
constexpr auto numTextures = 15;

// TODO: add reserve, shrink mechanisms
// TODO(opt, low): we could add a FixedSizeBuffer where all allocations
// have the same size, simplifies some things and allows us to get rid of
// Span (and its size member).
class Buffer {
public:
	Buffer(rvg::Context& ctx, vk::BufferUsageFlags usage, u32 memBits);

	// TODO: revisit/fix
	~Buffer() = default;

	Buffer(Buffer&&) = delete;
	Buffer& operator=(Buffer&&) = delete;

	unsigned allocate(unsigned size);
	unsigned allocate(nytl::Span<const std::byte> data);
	void write(unsigned offset, nytl::Span<const std::byte> data);
	void free(unsigned offset, unsigned size);

	// Returns the given span as writable buffer.
	// The buffer is only guaranteed to be valid until the allocate or free
	// call. Will automatically mark the returned range for update.
	nytl::Span<std::byte> writable(unsigned offset, unsigned size);

	bool updateDevice();

	rvg::Context& context() const { return *context_; }
	const vpp::SubBuffer& buffer() const { return buffer_; }
	const std::vector<std::byte> data() const { return data_; }

protected:
	struct Span {
		vk::DeviceSize offset;
		vk::DeviceSize size;
	};

	rvg::Context* context_;
	vpp::SubBuffer buffer_;
	std::vector<std::byte> data_;
	std::vector<Span> updates_;
	std::vector<Span> free_; // sorted by offset

	vk::BufferUsageFlags usage_;
	u32 memBits_;
};

// Buffer that tracks allocation and therefore does not require the size
// of the allocation when freeing.
class AllocTrackedBuffer : public Buffer {
public:
	using Buffer::Buffer;
	unsigned allocate(unsigned size);
	void free(unsigned offset);
	void free(unsigned offset, unsigned size);

protected:
	std::vector<Span> allocations_;
};

class TransformPool {
public:
	using Matrix = Mat4f;

public:
	TransformPool(Context&);

	unsigned allocate();
	void free(unsigned id);
	void write(unsigned id, const Matrix& transform);

	Matrix& writable(unsigned id);
	nytl::Span<Matrix> writable(unsigned id, unsigned count);

	bool updateDevice() { return buffer_.updateDevice(); }
	const auto& buffer() const { return buffer_.buffer(); }

protected:
	Buffer buffer_;
};

class ClipPool {
public:
	// Plane is all points x for which: dot(plane.xy, x) - plane.z == 0.
	// The inside of the plane are all points for which it's >0.
	// Everything <0 gets clipped.
	using Plane = nytl::Vec3f;

public:
	ClipPool(Context&);

	unsigned allocate(unsigned id);
	void free(unsigned id, unsigned count);
	void write(unsigned id, nytl::Span<const Plane> planes);

	bool updateDevice() { return buffer_.updateDevice(); }

	const auto& buffer() const { return buffer_.buffer(); }

protected:
	Buffer buffer_;
};

class PaintPool {
public:
	struct PaintData {
		Vec4f inner;
		Vec4f outer;
		Vec4f custom;
		nytl::Mat4f transform; // mat3 + additional data
	};

public:
	PaintPool(Context& ctx);

	unsigned allocate();
	void free(unsigned id);
	void write(unsigned id, const PaintData& data);
	void setTexture(unsigned i, vk::ImageView texture);

	bool updateDevice() { return buffer_.updateDevice(); }

	const auto& buffer() const { return buffer_.buffer(); }
	const auto& textures() const { return textures_; }

protected:
	Buffer buffer_;
	std::vector<vk::ImageView> textures_;
};

class VertexPool {
public:
	using Index = u32;
	struct Vertex {
		Vec2f pos; // NOTE: we could probably use 16 bit floats instead
		Vec2f uv; // NOTE: try using 8 or 16 bit snorm, or 16bit float instead
		Vec4u8 color;
	};

public:
	VertexPool(rvg::Context& ctx);

	void bind(vk::CommandBuffer cb);
	bool updateDevice();

	unsigned allocateVertices(unsigned count);
	unsigned allocateIndices(unsigned count);
	void writeVertices(unsigned id, Span<const Vertex> vertices);
	void writeIndices(unsigned id, Span<const Index> indices);
	void freeVertices(unsigned id);
	void freeIndices(unsigned id);

protected:
	AllocTrackedBuffer vertices_;
	AllocTrackedBuffer indices_;
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

struct DrawSet {
	std::vector<DrawCommand> commands;
	vpp::TrDs ds;

	TransformPool* transformPool;
	ClipPool* clipPool;
	PaintPool* paintPool;
	VertexPool* vertexPool;
	vk::ImageView fontAtlas;
};

class Scene;
class DrawRecorder;

class DrawRecorder {
public:
	DrawRecorder(Scene& scene);
	~DrawRecorder();

	void bind(TransformPool&);
	void bind(ClipPool&);
	void bind(PaintPool&);
	void bind(VertexPool&);
	void bindFontAtlas(vk::ImageView fontAtlas);

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

