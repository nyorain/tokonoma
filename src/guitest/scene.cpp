#include "scene.hpp"
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>
#include <nytl/bytes.hpp>
#include <dlg/dlg.hpp>

namespace rvg {

// util
namespace {

template<typename T>
nytl::Span<T> asSpan(nytl::Span<std::byte> bytes) {
	dlg_assert(bytes.size() % sizeof(T) == 0);
	auto size = bytes.size() / sizeof(T);
	auto ptr = reinterpret_cast<T*>(bytes.data());
	return nytl::Span<T>(ptr, size);
}

template<typename T>
nytl::Span<const T> asSpan(nytl::Span<const std::byte> bytes) {
	dlg_assert(bytes.size() % sizeof(T) == 0);
	auto size = bytes.size() / sizeof(T);
	auto ptr = reinterpret_cast<const T*>(bytes.data());
	return nytl::Span<const T>(ptr, size);
}

} // anon namespace

// VertexPool
VertexPool::VertexPool(rvg::Context& ctx) :
	Buffer(ctx, vk::BufferUsageBits::vertexBuffer, ctx.device().deviceMemoryTypes()) {
}

// IndexPool
IndexPool::IndexPool(rvg::Context& ctx) :
	Buffer(ctx, vk::BufferUsageBits::indexBuffer, ctx.device().deviceMemoryTypes()) {
}

// TransformPool
TransformPool::TransformPool(Context& ctx) :
	Buffer(ctx, vk::BufferUsageBits::storageBuffer, ctx.device().deviceMemoryTypes()) {
}

// ClipPool
ClipPool::ClipPool(Context& ctx) :
	Buffer(ctx, vk::BufferUsageBits::storageBuffer, ctx.device().deviceMemoryTypes()) {
}

// PaintPool
PaintPool::PaintPool(Context& ctx) :
	Buffer(ctx, vk::BufferUsageBits::storageBuffer, ctx.device().deviceMemoryTypes()) {
}

void PaintPool::setTexture(unsigned i, vk::ImageView texture) {
	dlg_assert(i < numTextures);
	textures_.resize(std::max<std::size_t>(i, textures_.size()));
	textures_[i] = texture;
}

// Scene
Scene::Scene(Context& ctx) : context_(&ctx) {
	const auto stages = vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment;
	std::array<vk::Sampler, numTextures> samplers;
	for(auto& s : samplers) {
		s = context().textureSampler();
	}

	auto& dev = context().device();
	auto bindings = std::array{
		// clip
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		// transform
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		// paint, buffer + textures
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler, stages,
			samplers.data(), numTextures),
		// draw commands
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		// font atlas
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler, stages,
			&context().textureSampler().vkHandle()),
	};
	dsl_.init(dev, bindings);

	if(context().multidrawIndirect()) {
		pipeLayout_ = {dev, {{dsl_.vkHandle()}}, {}};
	} else {
		vk::PushConstantRange pcr;
		pcr.offset = 0u;
		pcr.size = 4u;
		pcr.stageFlags = vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment;
		pipeLayout_ = {dev, {{dsl_.vkHandle()}}, {{pcr}}};
	}
}

bool Scene::updateDevice() {
	auto rerec = false;
	auto ownCmdBufChanged = false;

	// update commands
	if(!cmdUpdates_.empty()) {
		// ensure size
		// indirectCmdBuffer
		auto needed = sizeof(vk::DrawIndexedIndirectCommand) * numDrawCalls_;
		if(indirectCmdBuffer_.size() < needed) {
			needed = std::max<vk::DeviceSize>(needed * 2, 16 * sizeof(vk::DrawIndexedIndirectCommand));
			auto usage = vk::BufferUsageBits::indirectBuffer;
			indirectCmdBuffer_ = {context().bufferAllocator(),
				needed, usage, context().device().hostMemoryTypes()};
			rerec = true;
		}

		// ownCmdBuffer
		needed = sizeof(DrawCommandData) * numDrawCalls_;
		if(ownCmdBuffer_.size() < needed) {
			needed = std::max<vk::DeviceSize>(needed * 2, 16 * sizeof(DrawCommandData));
			auto usage = vk::BufferUsageBits::storageBuffer;
			ownCmdBuffer_ = {context().bufferAllocator(),
				needed, usage, context().device().hostMemoryTypes()};
			ownCmdBufChanged = true;
			rerec = true;
		}

		// maps
		// TODO(perf): make maps persistent?
		auto indirectCmdMap = indirectCmdBuffer_.memoryMap();
		auto indirectCmds = asSpan<vk::DrawIndexedIndirectCommand>(indirectCmdMap.span());

		auto ownCmdMap = ownCmdBuffer_.memoryMap();
		auto ownCmds = asSpan<DrawCommandData>(ownCmdMap.span());

		// write updates
		for(auto& update : cmdUpdates_) {
			auto& srcCmd = draws_[update.draw].commands[update.command];
			indirectCmds[update.offset].firstInstance = 0u;
			indirectCmds[update.offset].instanceCount = 1u;
			indirectCmds[update.offset].firstIndex = srcCmd.indexStart;
			indirectCmds[update.offset].indexCount = srcCmd.indexCount;
			indirectCmds[update.offset].vertexOffset = 0u;

			ownCmds[update.offset].transform = srcCmd.transform;
			ownCmds[update.offset].paint = srcCmd.paint;
			ownCmds[update.offset].clipStart = srcCmd.clipStart;
			ownCmds[update.offset].clipCount = srcCmd.clipCount;
			ownCmds[update.offset].type = srcCmd.type;
			ownCmds[update.offset].uvFadeWidth = srcCmd.uvFadeWidth;
		}

		indirectCmdMap.flush();
		ownCmdMap.flush();
		cmdUpdates_.clear();
	}

	// update descriptors
	std::vector<vpp::DescriptorSetUpdate> dsus;
	auto updateDs = [&](auto& draw) {
		auto& dsu = dsus.emplace_back(draw.ds);
		dsu.storage(draw.state.clipBuffer);
		dsu.storage(draw.state.transformBuffer);
		dsu.storage(draw.state.paintBuffer);

		auto& texs = draw.state.textures;
		auto binding = dsu.currentBinding();
		dlg_assert(texs.size() <= numTextures);
		for(auto i = 0u; i < texs.size(); ++i) {
			dsu.imageSampler(texs[i], {}, binding, i);
		}

		auto& emptyImage = context().emptyImage();
		for(auto i = texs.size(); i < numTextures; ++i) {
			dsu.imageSampler(emptyImage.vkImageView(), {}, binding, i);
		}

		dsu.storage(ownCmdBuffer_);
		dsu.imageSampler(draw.state.fontAtlas);
	};

	if(ownCmdBufChanged) {
		for(auto& draw : draws_) {
			updateDs(draw);
		}

		dsUpdates_.clear(); // make sure they are not updated again
	}

	if(!dsUpdates_.empty()) {
		for(auto& dsUpdate : dsUpdates_) {
			updateDs(draws_[dsUpdate]);
		}

		dsUpdates_.clear();
		if(!dynamicDescriptors()) {
			rerec = true;
		}
	}

	vpp::apply(dsus);
	return rerec;
}

void Scene::recordDraw(vk::CommandBuffer cb) {
	const auto multidrawIndirect = context().multidrawIndirect();

	const auto cmdSize = sizeof(vk::DrawIndexedIndirectCommand);
	auto offset = indirectCmdBuffer_.offset();
	for(auto& draw : draws_) {
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {{draw.ds.vkHandle()}}, {});

		vk::cmdBindVertexBuffers(cb, 0,
			{{draw.state.vertexBuffer.buffer()}}, {{draw.state.vertexBuffer.offset()}});
		vk::cmdBindIndexBuffer(cb, draw.state.indexBuffer.buffer(),
			draw.state.indexBuffer.offset(), vk::IndexType::uint32);

		if(multidrawIndirect) {
			vk::cmdDrawIndexedIndirect(cb, indirectCmdBuffer_.buffer(), offset,
				draw.commands.size(), cmdSize);
			offset += cmdSize * draw.commands.size();
		} else {
			for(auto i = 0u; i < draw.commands.size(); ++i) {
				const auto pcrValue = u32(i);
				const auto pcrStages =
					vk::ShaderStageBits::fragment |
					vk::ShaderStageBits::vertex;

				vk::cmdPushConstants(cb, pipeLayout_, pcrStages, 0, 4u, &pcrValue);
				vk::cmdDrawIndexedIndirect(cb, indirectCmdBuffer_.buffer(), offset,
					1, cmdSize);
				offset += cmdSize;
			}
		}
	}
}

void Scene::add(DrawSet set) {
	auto id = numDrawCalls_;
	if(draws_.size() <= id) {
		dlg_assert(draws_.size() == id);
		auto& draw = draws_.emplace_back(std::move(set));
		// TODO: shouldn't reallocate every time the number of draws
		// is resized, e.g. think of a case where something is hidden
		// and shown again over and over (meaning in one frame a draw call is
		// added, then not), we allocate a new ds every time.
		draw.ds = {context().dsAllocator(), dsl_};
		dsUpdates_.push_back(id);

		for(auto c = 0u; c < draw.commands.size(); ++c) {
			auto& update = cmdUpdates_.emplace_back();
			update.draw = id;
			update.command = c;
			update.offset = numDrawCalls_;
			draw.commands[c].offset = numDrawCalls_;

			++numDrawCalls_;
		}
	} else {
		// check if descriptors are the same
		auto& oldDraw = draws_[id];
		bool same = (oldDraw.state == set.state);
		if(!same) {
			dsUpdates_.push_back(id);
		}

		// compare each draw command
		oldDraw.commands.resize(set.commands.size());
		for(auto c = 0u; c < set.commands.size(); ++c) {
			auto& oldc = oldDraw.commands[c];
			auto& newc = set.commands[c];
			newc.offset = numDrawCalls_;
			if(std::memcmp(&oldc, &newc, sizeof(oldc))) {
				// the commands aren't the same, need update
				auto& update = cmdUpdates_.emplace_back();
				update.draw = id;
				update.command = c;
				update.offset = numDrawCalls_;
			}

			++numDrawCalls_;
		}
	}

	++numDraws_;
}

// DrawRecorder
DrawRecorder::DrawRecorder(Scene& scene) : scene_(scene) {
}

DrawRecorder::~DrawRecorder() {
	if(!current_.commands.empty()) {
		scene_.add(std::move(current_));
	}
	scene_.finish();
}

void DrawRecorder::bindTransformBuffer(vpp::BufferSpan pool) {
	dlg_assert(pool.valid());
	pending_.state.transformBuffer = pool;
	if(current_.commands.empty()) {
		current_.state.transformBuffer = pool;
	}
}

void DrawRecorder::bindClipBuffer(vpp::BufferSpan pool) {
	dlg_assert(pool.valid());
	pending_.state.clipBuffer = pool;
	if(current_.commands.empty()) {
		current_.state.clipBuffer = pool;
	}
}

void DrawRecorder::bindPaintBuffer(vpp::BufferSpan pool) {
	dlg_assert(pool.valid());
	pending_.state.paintBuffer = pool;
	if(current_.commands.empty()) {
		current_.state.paintBuffer = pool;
	}
}

void DrawRecorder::bindVertexBuffer(vpp::BufferSpan pool) {
	dlg_assert(pool.valid());
	pending_.state.vertexBuffer = pool;
	if(current_.commands.empty()) {
		current_.state.vertexBuffer = pool;
	}
}

void DrawRecorder::bindIndexBuffer(vpp::BufferSpan pool) {
	dlg_assert(pool.valid());
	pending_.state.indexBuffer = pool;
	if(current_.commands.empty()) {
		current_.state.indexBuffer = pool;
	}
}

void DrawRecorder::bindTextures(std::vector<vk::ImageView> textures) {
	pending_.state.textures = std::move(textures);
	if(current_.commands.empty()) {
		current_.state.textures = pending_.state.textures;
	}
}

void DrawRecorder::bindFontAtlas(vk::ImageView atlas) {
	dlg_assert(atlas);
	pending_.state.fontAtlas = atlas;
	if(current_.commands.empty()) {
		current_.state.fontAtlas = atlas;
	}
}

void DrawRecorder::draw(const DrawCall& call) {
	// check if state is still the same
	dlg_assert(pending_.commands.empty()); // TODO: shouldn't exist in first place
	bool same = (current_.state == pending_.state);
	if(!same) {
		dlg_assert(!current_.commands.empty());
		scene_.add(std::move(current_));
		current_ = std::move(pending_);
	}

	DrawCommand cmd;
	static_cast<DrawCall&>(cmd) = call;
	current_.commands.push_back(cmd);
}

// free
bool operator==(const DrawState& a, const DrawState& b) {
	return
		a.transformBuffer == b.transformBuffer &&
		a.clipBuffer == b.clipBuffer &&
		a.paintBuffer == b.paintBuffer &&
		a.vertexBuffer == b.vertexBuffer &&
		a.indexBuffer == b.indexBuffer &&
		a.fontAtlas == b.fontAtlas &&
		std::equal(
			a.textures.begin(), a.textures.end(),
			b.textures.begin(), b.textures.end());
}

} // namespace rvg

