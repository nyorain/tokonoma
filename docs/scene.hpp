#pragma once

#include <rvg/fwd.hpp>
#include <rvg/context.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/mat.hpp>
#include <cstdint>

namespace rvg {

using u32 = std::uint32_t;

class Buffer {
public:
	Buffer(rvg::Context& ctx) : context_(&ctx) {}

	unsigned allocate(unsigned size);
	unsigned allocate(nytl::Span<const std::byte> data);
	void fill(unsigned offset, nytl::Span<const std::byte> data);
	void free(unsigned offset, unsigned size);

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
	TransformPool(TransformPool&&) = delete;
	TransformPool& operator=(TransformPool&&) = delete;

	void updateDevice();
	const auto& buffer() const { return buffer_; }

protected:
	struct State {
		u32 nextFree {0xFFFFFFFFu};
		nytl::Mat4f transform;
	};

	vpp::SubBuffer buffer_;
	std::vector<State> transforms_;
	std::vector<unsigned> updateList_;
	u32 firstFree_ {0xFFFFFFFFu};
};

class ClipPool {
public:
	const auto& buffer() const { return buffer_; }

protected:
	struct FreeBlock {
		unsigned start;
		unsigned size;
	};

	vpp::SubBuffer buffer_;
	std::vector<nytl::Vec3f> planes_;
	std::vector<unsigned> updateList_;
	std::vector<FreeBlock> freeBlocks_;
};

class PaintPool {
public:
	const auto& buffer() const { return buffer_; }
	const auto& textures() const { return textures_; }

protected:
	struct PaintData {
		// TODO
	};

	struct State {
		unsigned nextFree;
		PaintData data;
	};

	vpp::SubBuffer buffer_;
	std::vector<State> paints_;
	std::vector<vk::ImageView> textures_;
};

class VertexPool {
public:
	struct Vertex {
		Vec2f pos; // NOTE: we could probably use 16 bit floats instead
		Vec2f uv; // NOTE: try using 8 or 16 bit snorm, or 16bit float instead
		Vec4u8 color;
	};

public:
	void bind(vk::CommandBuffer cb);
	bool updateDevice();

	unsigned allocateVertices(unsigned count);
	unsigned allocateIndices(unsigned count);
	void writeVertices(unsigned offset, Span<Vertex> vertices);
	void writeIndices(unsigned offset, Span<u32> indices);
	void freeVertices(unsigned offset);
	void freeIndices(unsigned offset);

	Context& context() const { return *context_; }

protected:
	struct FreeBlock {
		unsigned start;
		unsigned size;
	};

	struct Update {
		unsigned start;
		unsigned bytes;
	};

	Context* context_ {};
	vpp::SubBuffer vertexBuffer_;
	vpp::SubBuffer indexBuffer_;
	std::vector<std::byte> vertices_;
	std::vector<std::byte> indices_;
	std::vector<Update> vertexUpdates_;
	std::vector<Update> indexUpdates_;
	std::vector<FreeBlock> freeVertexBlocks_;
	std::vector<FreeBlock> freeIndexBlocks_;
};

class DrawRecorder {
public:

protected:
	TransformPool* transformPool;
	ClipPool* clipPool;
	PaintPool* paintPool;
	VertexPool* vertexPool;
	vk::ImageView fontAtlas;
};

class Scene {
public:
	void recordDraw(vk::CommandBuffer cb);
	bool updateDevice();

	Context& context() const { return *context_; }
	DrawRecorder record() {
		numDraws_ = 0u;
		numDrawCalls_ = 0u;
		return {};
	}

	// TODO, placeholder. Support descriptor indexing extension
	bool dynamicDescriptors() const { return false; }

protected:
	struct DrawCommand {
		u32 transform;
		u32 paint;
		u32 clipStart;
		u32 clipCount;
		u32 indexStart;
		u32 indexCount;
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

	struct DrawCommandData {
		u32 transform;
		u32 paint;
		u32 clipStart;
		u32 clipCount;
	};

	struct Update {
		unsigned draw; // draw id
		unsigned command; // command id (inside draw)
		unsigned offset; // total command id
	};

	friend class DrawRecorder;
	void set(unsigned i, DrawSet);
	void finish() {
		draws_.resize(numDraws_);
	}

protected:
	unsigned numDraws_ {};
	unsigned numDrawCalls_ {};

	std::vector<Update> cmdUpdates_;
	std::vector<unsigned> dsUpdates_;
	std::vector<DrawSet> draws_;
	// NOTE: we might be able to merge those 2 buffers into one
	vpp::SubBuffer indirectCmdBuffer_;
	vpp::SubBuffer ownCmdBuffer_;

	Context* context_ {};
};

} // namespace rvg
