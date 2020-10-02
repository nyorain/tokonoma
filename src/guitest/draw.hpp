#pragma once

#include "buffer.hpp"
#include <vpp/trackedDescriptor.hpp>
#include <vpp/buffer.hpp>
#include <vector>

namespace rvg2 {

class Context;
using u32 = std::uint32_t;

// Must match the GPU representation in layout.glsl
struct DrawBindingsData {
	u32 transform;
	u32 paint;
	u32 clipStart;
	u32 clipCount;
	u32 type;
	float uvFadeWidth;
	u32 _pad[2];
};

struct DrawPool {
	DrawPool(Context& ctx, DevMemBits memBits = DevMemType::deviceLocal);

	Buffer<vk::DrawIndexedIndirectCommand> indirectCmdBuf;
	Buffer<DrawBindingsData> bindingsCmdBuf;
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

struct Draw {
	u32 transform {invalid};
	u32 paint {invalid};
	u32 clipStart {invalid};
	u32 clipCount {invalid};
	u32 indexStart {invalid};
	u32 indexCount {invalid};
	u32 type {invalid};
	u32 vertexOffset {0u};
	float uvFadeWidth {0.f};
};

/// Represents a single (multi-)draw call.
/// Batches multiple draws that all have the same state (pools).
class DrawCall {
public:
	DrawCall(DrawPool&);
	~DrawCall();

	// Records its draw command into the given command buffer.
	// This command will stay valid as long as the DrawCall is valid,
	// its state has not changed and no reallocation in the DrawPool
	// was needed. When descriptor indexing is enabled
	// (and was passed as feature to the associated context), not even
	// changing the state will require a subsequent rerecord.
	void record(vk::CommandBuffer cb);

	// Returns whether a re-record is needed.
	bool updateDevice();

	void reserve(unsigned size);
	unsigned add(nytl::Span<const Draw> draw);
	unsigned add(const Draw& draw);
	void write(unsigned id, nytl::Span<const Draw> draw);
	void write(unsigned id, const Draw& draw);
	Draw get(unsigned id);
	void remove(unsigned id);
	// TODO: remote(unsigned id, unsigned count)?
	// TODO: add insert api (positional addition).

	Context& context() const { return pool_->indirectCmdBuf.context(); }

protected:
	void free();

protected:
	DrawPool* pool_ {};
	u32 indirectBufID_ {invalid};
	u32 bindingsBufID_ {invalid};
	u32 size_ {};
	u32 reserved_ {};

	vpp::TrDs ds_ {};
	DrawState state_ {};
	bool updateDs_ {true};
	bool rerecord_ {true};
};

class DrawRecorder {
public:
	DrawRecorder(Context& ctx, std::vector<DrawCall>& drawCalls);
	~DrawRecorder();

	void bindTransformBuffer(vpp::BufferSpan);
	void bindClipBuffer(vpp::BufferSpan);
	void bindPaintBuffer(vpp::BufferSpan);
	void bindVertexBuffer(vpp::BufferSpan);
	void bindIndexBuffer(vpp::BufferSpan);
	void bindFontAtlas(vk::ImageView);
	void bindTextures(std::vector<vk::ImageView>);

	void bindTransform(u32);
	void bindPaint(u32);
	void bindClips(u32 start, u32 end);

	void draw(u32 indexStart, u32 indexCount, u32 firstVertex,
		u32 type, float uvFadeWidth);

	// TODO: good idea? binding everything at once
	// Members of the draw that are set to 'invalid' will be set
	// to the currently bound state (if there is any, will error
	// if there is none).
	// void draw(const Draw& draw);

	/*
	void bind(TransformPool& pool) { bindTransformBuffer(pool.buffer()); }
	void bind(ClipPool& pool) { bindClipBuffer(pool.buffer()); }
	void bind(VertexPool& pool) { bindVertexBuffer(pool.buffer()); }
	void bind(IndexPool& pool) { bindIndexBuffer(pool.buffer()); }
	void bind(PaintPool& pool) {
		bindPaintBuffer(pool.buffer());
		bindTextures(pool.textures());
	}
	*/

	// TODO
	// void bind(FontAtlas&);

protected:
	DrawState current_;
	DrawState pending_;
	std::vector<Draw> draws_;

	u32 transform_ {invalid};
	u32 paint_ {invalid};
	u32 clipStart_ {invalid};
	u32 clipCount_ {0u};

	Context* context_ {};
	std::vector<DrawCall>* calls_;
};

} // namespace rvg
