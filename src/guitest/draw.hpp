#pragma once

#include "buffer.hpp"
#include "scene.hpp"
#include <vpp/trackedDescriptor.hpp>
#include <vpp/buffer.hpp>
#include <vector>

namespace rvg2 {

class Context;
using u32 = std::uint32_t;

enum class DrawType : u32 {
	fill,
	text,
	stroke,
	// meta-type: disables this draw already on the cpu by setting
	// the indexCount to zero.
	disabled,
};

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
	vpp::BufferSpan transformBuffer; // TransformPool
	vpp::BufferSpan clipBuffer; // ClipPool
	vpp::BufferSpan paintBuffer; // PaintPool
	vpp::BufferSpan vertexBuffer; // VertexPool
	vpp::BufferSpan indexBuffer; // IndexPool
	vpp::BufferSpan drawBuffer; // DrawPool: bindingsCmdBuf
	std::vector<vk::ImageView> textures;
	vk::ImageView fontAtlas;
};

bool operator==(const DrawState& a, const DrawState& b);
inline bool operator!=(const DrawState& a, const DrawState& b) {
	return !(a == b);
}

bool valid(const DrawState&);

struct Draw {
	u32 transform {invalid};
	u32 paint {invalid};
	u32 clipStart {invalid};
	u32 clipCount {invalid};
	u32 indexStart {invalid};
	u32 indexCount {invalid};
	DrawType type {invalid}; // u32
	u32 vertexOffset {0u};
	float uvFadeWidth {0.f};
};

class DrawDescriptor {
public:
	DrawDescriptor(Context&);

	bool updateDevice();

	void state(const DrawState& state) {
		updateDs_ = !(state_ == state);
		state_ = state;
	}
	const DrawState& state() const {
		return state_;
	}

	DrawState& changeState() {
		updateDs_ = true;
		return state_;
	}

	const auto& ds() const { return ds_; }
	const Context& context() const { return *context_; }

protected:
	bool updateDs_ {true};
	DrawState state_;
	Context* context_ {};
	vpp::TrDs ds_;
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
	void record(vk::CommandBuffer cb,
		bool bindPipe = true,
		bool bindDrawPool = true) const;
	void record(vk::CommandBuffer cb,
		const vk::Extent2D& targetSize,
		bool bindPipe = true,
		bool bindDrawPool = true) const;

	void write(unsigned id, nytl::Span<const Draw> draw);
	void write(unsigned id, const Draw& draw);
	Draw get(unsigned id);

	// When these function are called, a re-record is needed.
	void clear();
	void reserve(unsigned size);
	unsigned add(nytl::Span<const Draw> draw);
	unsigned add(const Draw& draw);
	void remove(unsigned id);

	void descriptor(vk::DescriptorSet ds);
	void vertexBuffer(vpp::BufferSpan);
	void indexBuffer(vpp::BufferSpan);

	vk::DescriptorSet descriptor() const { return ds_; }
	vpp::BufferSpan indexBuffer() const { return indexBuffer_; }
	vpp::BufferSpan vertexBuffer() const { return vertexBuffer_; }

	// TODO: remove(unsigned id, unsigned count)?
	// TODO: add insert api (positional addition).
	//   but how to keep ids in that case?

	Context& context() const { return pool_->indirectCmdBuf.context(); }
	auto size() const { return size_; }

	// TODO: counter-intuitive, bad API design
	bool checkRerecord() {
		auto ret = rerecord_;
		rerecord_ = false;
		return ret;
	}

protected:
	DrawPool* pool_ {};
	u32 indirectBufID_ {invalid};
	u32 bindingsBufID_ {invalid};
	u32 size_ {};
	u32 reserved_ {};

	vk::DescriptorSet ds_ {};
	vpp::BufferSpan indexBuffer_ {};
	vpp::BufferSpan vertexBuffer_ {};
	bool rerecord_ {true};
};

using DrawCalls = std::vector<DrawCall>;

struct DrawInstance {
	u32 drawCallID; // Index into the DrawCall vector passed to DrawRecorder
	u32 drawID; // ID inside the specific DrawCall

	Draw get(DrawCalls& calls) {
		return calls[drawCallID].get(drawID);
	}

	void set(DrawCalls& calls, const Draw& draw) {
		calls[drawCallID].write(drawID, draw);
	}
};

class DrawRecorder {
public:
	DrawRecorder(DrawPool& drawPool, DrawCalls& drawCalls,
		std::vector<DrawDescriptor>& descriptors);
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
	void bindClips(u32 start, u32 count);

	void flush();

	DrawInstance draw(u32 indexStart, u32 indexCount, u32 firstVertex,
		DrawType type, float uvFadeWidth);

	// TODO: good idea? binding everything at once
	// Members of the draw that are set to 'invalid' will be set
	// to the currently bound state (if there is any, will error
	// if there is none).
	// void draw(const Draw& draw);

	void bind(TransformPool& pool) { bindTransformBuffer(pool.buffer()); }
	void bind(ClipPool& pool) { bindClipBuffer(pool.buffer()); }
	void bind(VertexPool& pool) { bindVertexBuffer(pool.buffer()); }
	void bind(IndexPool& pool) { bindIndexBuffer(pool.buffer()); }
	void bind(PaintPool& pool) {
		bindPaintBuffer(pool.buffer());
		bindTextures(pool.textures());
	}

	void bind(const Transform& t) {
		// dlg_assert(t.valid());
		bindTransformBuffer(t.pool()->buffer());
		bindTransform(t.id());
	}

	void bind(const Clip& t) {
		// dlg_assert(t.valid());
		bindClipBuffer(t.pool()->buffer());
		bindClips(t.clipStart(), t.clipCount());
	}

	void bind(const Paint& t) {
		// dlg_assert(t.valid());
		bindPaintBuffer(t.pool()->buffer());
		bindTextures(t.pool()->textures());
		bindPaint(t.id());
	}

	// TODO: implement! returns whether device update is needed so far
	// bool updateDevice();

	// void bind(FontAtlas&);

protected:
	DrawState current_ {};
	DrawState pending_ {};
	std::vector<Draw> draws_;

	u32 transform_ {invalid};
	u32 paint_ {invalid};
	u32 clipStart_ {invalid};
	u32 clipCount_ {0u};

	DrawPool* drawPool_ {};
	u32 numDraws_ {};
	std::vector<DrawCall>* calls_ {};
	u32 numDescriptors_ {};
	std::vector<DrawDescriptor>* descriptors_ {};
};

void record(vk::CommandBuffer cb, nytl::Span<const DrawCall> draws,
		const vk::Extent2D& extent);

} // namespace rvg
