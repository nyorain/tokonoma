#pragma once

#include "buffer.hpp"
#include "scene.hpp"
#include <vpp/trackedDescriptor.hpp>
#include <vpp/buffer.hpp>
#include <vector>
#include <variant>

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

// TODO: we should probably combine those two buffers into one.
struct DrawPool {
	DrawPool() = default;
	DrawPool(UpdateContext& ctx, DevMemBits memBits = DevMemType::deviceLocal);
	void init(UpdateContext& ctx, DevMemBits memBits = DevMemType::deviceLocal);

	// When the indirect-command buffer gets recreated, we always have to
	// rerecord the command buffer, rebatching won't detect the change.
	static constexpr auto indirectUpdateFlags = UpdateFlags::rerec;
	static constexpr auto bindingsUpdateFlags = UpdateFlags::descriptors;

	Buffer<vk::DrawIndexedIndirectCommand, indirectUpdateFlags> indirectCmdBuf;
	Buffer<DrawBindingsData, bindingsUpdateFlags> bindingsCmdBuf;
};

/// Bundles different ways to reference a buffer allocation.
/// In comparision to vpp::BufferSpan, this can reference a vk::Buffer
/// variable, meaning it can propagate buffer recreation.
/// so can remain valid after a buffer recreation.
struct BufferRef {
	using Allocation = vpp::BasicAllocation<vk::DeviceSize>;
	static const Allocation& fullAllocation();

	BufferRef() = default;

	/// The referenced resources (themselves, *not* just what they reference)
	/// must stay valid as long as this BufferRef is used.
	BufferRef(const vpp::SubBuffer&);
	BufferRef(const vk::Buffer&, const Allocation&);

	// NOTE: when you pass a BufferRef constructed from a BufferSpan
	// to a DrawDescriptor/DrawCall/DrawRecorder, the exact allocation
	// must stay valid as long as the call uses it.
	// The other constructors just require that e.g. the SubBuffer itself
	// stays valid, while its allocation/underlaying buffer can change
	// (that might require a rerecord/descriptor update though).
	BufferRef(vpp::BufferSpan span);

	vk::DescriptorBufferInfo info() const;

	// Returns whether this actually references a resource.
	// The referenced allocation may be empty though.
	bool valid() const;

	// Returns whether this is valid *and* references a nonempty allocation.
	bool nonempty() const;

	struct Raw {
		const vk::Buffer* buffer {};
		const Allocation* allocation {&fullAllocation()};
	};

	struct Sub {
		const vpp::SharedBuffer* const * buffer {};
		const Allocation* allocation {&fullAllocation()};
	};

	std::variant<vpp::BufferSpan, Raw, Sub> ref;
};

bool operator==(const BufferRef& a, const BufferRef& b);
bool same(const vk::DescriptorBufferInfo& a, const vk::DescriptorBufferInfo& b);

struct DrawState {
	BufferRef transformBuffer; // TransformPool
	BufferRef clipBuffer; // ClipPool
	BufferRef paintBuffer; // PaintPool
	BufferRef drawBuffer; // DrawPool: bindingsCmdBuf
	std::vector<vk::ImageView> textures;
	const vk::ImageView* fontAtlas {};
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

class DrawDescriptor : public DeviceObject {
public:
	DrawDescriptor(UpdateContext&);

	/// Returns whether a rerecord is needed.
	UpdateFlags updateDevice() override;

	void state(const DrawState& state) {
		if(state_ != state) {
			// registerDeviceUpdate();
		}
		state_ = state;
	}
	const DrawState& state() const {
		return state_;
	}

	DrawState& changeState() {
		registerDeviceUpdate();
		return state_;
	}

	const auto& ds() const { return ds_; }

protected:
	DrawState state_;
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
		bool bindVertsInds = true) const;
	void record(vk::CommandBuffer cb,
		const vk::Extent2D& targetSize,
		bool bindPipe = true,
		bool bindVertsInds = true) const;

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
	void vertexBuffer(BufferRef);
	void indexBuffer(BufferRef);

	vk::DescriptorSet descriptor() const { return ds_; }
	BufferRef indexBuffer() const { return indexBuffer_; }
	BufferRef vertexBuffer() const { return vertexBuffer_; }

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
	BufferRef indexBuffer_ {};
	BufferRef vertexBuffer_ {};
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

	void bindTransformBuffer(BufferRef);
	void bindClipBuffer(BufferRef);
	void bindPaintBuffer(BufferRef);
	void bindVertexBuffer(BufferRef);
	void bindIndexBuffer(BufferRef);
	void bindFontAtlas(const vk::ImageView&);
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
	struct State : DrawState {
		BufferRef vertexBuffer; // VertexPool
		BufferRef indexBuffer; // VertexPool
	};

	State current_ {};
	State pending_ {};
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
