#include "network.hpp"

#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/window.hpp>
#include <stage/types.hpp>
#include <stage/bits.hpp>
#include <stage/texture.hpp>

#include <vpp/fwd.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/bufferOps.hpp>

#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/mat.hpp>

#include <ny/mouseButton.hpp>
#include <ny/key.hpp>

#include <optional>
#include <cstddef>

#include <shaders/iro.vert.h>
#include <shaders/iro.frag.h>
#include <shaders/iro_building.vert.h>
#include <shaders/iro_texture.frag.h>
// #include <shaders/simple.vert.h>
// #include <shaders/texture.frag.h>
// #include <shaders/iro_outline.vert.h>
// #include <shaders/iro_outline.frag.h>
#include <shaders/iro.comp.h>

using namespace doi::types;

// mirrors glsl layout
struct Player {
	u32 gained {}; // written by gpu

	u32 resources {}; // global
	u32 netResources {}; // local; with delayed actions
	float _1; // padding
};

// mirrors glsl layout
struct Field {
	enum class Type : u32 {
		empty = 0u,
		resource = 1u,
		spawn = 2u,
		tower = 3u,
		accel = 4u,
	};

	// can be indexed with u32(Type)
	static constexpr u32 prices[5] = {
		// 0, 20000, 12000, 10000, 3000
		0, 20, 120, 10, 30 // debugging
	};

	// weakly typed to allow array indexing
	// counter clockwise, like unit circle
	enum Side : u32 {
		right = 0u,
		topRight = 1u,
		topLeft = 2u,
		left = 3u,
		botLeft = 4u,
		botRight = 5u,
	};

	static constexpr u32 playerNone = 0xFFFFFFFF;
	static constexpr u32 nextNone = 0xFFFFFFFF;

	Vec2f pos;
	Type type {Type::empty};
	f32 strength {0.f};
	nytl::Vec2f vel {0.f, 0.f};
	u32 player {playerNone};
	std::array<u32, 6> next {nextNone, nextNone, nextNone,
		nextNone, nextNone, nextNone};
	float _; // padding
};

// Assumption: y=1 has a positive x-offset relative to y=0
// therefore all fields with uneven y have additional x offset
// Also assumes y up
Vec2i neighborPos(Vec2i pos, Field::Side side) {
	switch(side) {
		case Field::Side::left: return {pos.x - 1, pos.y};
		case Field::Side::right: return {pos.x + 1, pos.y};
		case Field::Side::topLeft: return {pos.x + pos.y % 2 - 1, pos.y + 1};
		case Field::Side::botLeft: return {pos.x + pos.y % 2 - 1, pos.y - 1};
		case Field::Side::topRight: return {pos.x + pos.y % 2, pos.y + 1};
		case Field::Side::botRight: return {pos.x + pos.y % 2, pos.y - 1};
		default: return {-1, -1};
	}
}

class HexApp : public doi::App {
public:
	static constexpr auto size = 20u;

public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		// renderer().clearColor({1.f, 1.f, 1.f, 1.f});

		// logical
		view_.center = {0.f, 0.f};
		view_.size = {size, size};

		// layouts
		auto& dev = vulkanDevice();

		// resources
		textures_ = doi::loadTextureArray(dev, {
			"../assets/iro/test.png",
			"../assets/iro/spawner.png",
			"../assets/iro/ample.png",
			"../assets/iro/velocity.png"});

		vk::SamplerCreateInfo samplerInfo {};
		samplerInfo.minFilter = vk::Filter::linear;
		samplerInfo.magFilter = vk::Filter::linear;
		samplerInfo.minLod = 0;
		samplerInfo.maxLod = 0.25;
		sampler_ = {dev, samplerInfo};

		// compute
		auto compDsBindings = {
			// old fields
			vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			// new fields
			vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			// players
			vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			// NOTE: not used atm
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute)
		};

		compDsLayout_ = {dev, compDsBindings};
		compPipeLayout_ = {dev, {compDsLayout_}, {}};

		// graphics
		auto gfxDsBindings = {
			// transform (view) matrix
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex),
			// texture array
			vpp::descriptorBinding(
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment,
				-1, 1, &sampler_.vkHandle()),
		};

		gfxDsLayout_ = {dev, gfxDsBindings};
		gfxPipeLayout_ = {dev, {gfxDsLayout_},
			{{vk::ShaderStageBits::fragment, 0u, sizeof(nytl::Vec3f)}}};

		// pipes
		if(!initCompPipe(false)) {
			return false;
		}

		if(!initGfxPipe(false)) {
			return false;
		}

		// buffer
		auto hostMem = dev.hostMemoryTypes();
		auto devMem = dev.deviceMemoryTypes();

		// ubo
		auto uboSize = sizeof(nytl::Mat4f);
		gfxUbo_ = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		// init fields, storage
		fields_ = initFields();
		fieldCount_ = fields_.size();

		// TODO: host/dev memory?
		auto usage = vk::BufferUsageFlags(vk::BufferUsageBits::storageBuffer);
		auto storageSize = fields_.size() * sizeof(fields_[0]);
		storageOld_ = {dev.bufferAllocator(), storageSize,
			usage | vk::BufferUsageBits::transferDst, 0, devMem};
		vpp::writeStaging430(storageOld_, vpp::raw(*fields_.data(), fields_.size()));

		usage |= vk::BufferUsageBits::transferSrc |
			vk::BufferUsageBits::vertexBuffer;
		storageNew_ = {dev.bufferAllocator(), storageSize, usage, 0, devMem};

		// player buf
		usage = vk::BufferUsageBits::storageBuffer;
		auto playerSize = sizeof(Player) * 2;
		playerBuffer_ = {dev.bufferAllocator(), playerSize, usage, 0, hostMem};

		{
			Player init;
			init.resources = 0u;
			auto map = playerBuffer_.memoryMap();
			auto span = map.span();
			doi::write(span, init);
			doi::write(span, init);
		}

		// descriptors
		compDs_ = {dev.descriptorAllocator(), compDsLayout_};
		gfxDs_ = {dev.descriptorAllocator(), gfxDsLayout_};

		vpp::DescriptorSetUpdate compDsUpdate(compDs_);
		compDsUpdate.storage({{storageOld_.buffer(),
			storageOld_.offset(), storageOld_.size()}});
		compDsUpdate.storage({{storageNew_.buffer(),
			storageNew_.offset(), storageNew_.size()}});
		compDsUpdate.storage({{playerBuffer_.buffer(),
			playerBuffer_.offset(), playerBuffer_.size()}});

		vpp::DescriptorSetUpdate gfxDsUpdate(gfxDs_);
		gfxDsUpdate.uniform({{gfxUbo_.buffer(),
			gfxUbo_.offset(), gfxUbo_.size()}});
		gfxDsUpdate.imageSampler({{{}, textures_.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		vpp::apply({compDsUpdate, gfxDsUpdate});

		// indirect selected buffer
		selectedIndirect_ = {dev.bufferAllocator(),
			sizeof(vk::DrawIndirectCommand),
			vk::BufferUsageBits::indirectBuffer, 0, hostMem};

		// upload buffer
		// XXX: start size? implement dynamic resizing!
		stage_ = {dev.bufferAllocator(), 64u, vk::BufferUsageBits::transferSrc,
			0, hostMem};
		stageView_ = stage_.memoryMap();
		uploadPtr_ = stageView_.ptr();
		uploadSemaphore_ = {dev};
		uploadCb_ = dev.commandAllocator().get(
			dev.queueSubmitter().queue().family(),
			vk::CommandPoolCreateBits::resetCommandBuffer);

		// own compute cb
		computeSemaphore_ = {dev};
		createComputeCb();

		return true;
	}

	void createComputeCb() {
		auto& dev = vulkanDevice();
		auto qfamily = dev.queueSubmitter().queue().family();
		compCb_ = dev.commandAllocator().get(qfamily);
		vk::beginCommandBuffer(compCb_, {});
		vk::cmdBindDescriptorSets(compCb_, vk::PipelineBindPoint::compute,
			compPipeLayout_, 0, {compDs_}, {});
		vk::cmdBindPipeline(compCb_, vk::PipelineBindPoint::compute, compPipe_);
		vk::cmdDispatch(compCb_, fieldCount_, 1, 1);

		vk::BufferMemoryBarrier barrier;
		barrier.buffer = storageNew_.buffer();
		barrier.srcAccessMask = vk::AccessBits::shaderWrite;
		barrier.dstAccessMask = vk::AccessBits::transferRead;
		barrier.offset = 0;
		barrier.size = storageNew_.size();
		vk::cmdPipelineBarrier(compCb_,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::transfer,
			{}, {}, {barrier}, {});

		vk::BufferCopy region;
		region.srcOffset = storageNew_.offset();
		region.dstOffset = storageOld_.offset();
		region.size = storageOld_.size();
		vk::cmdCopyBuffer(compCb_, storageNew_.buffer(), storageOld_.buffer(),
			{region});

		vk::endCommandBuffer(compCb_);
	}

	std::vector<Field> initFields() {
		constexpr float cospi6 = 0.86602540378; // cos(pi/6) or sqrt(3)/2
		constexpr float radius = 1.f;
		constexpr float rowHeight = 1.5 * radius;
		constexpr float colWidth = 2 * cospi6 * radius;

		constexpr auto height = size;
		constexpr auto width = size;

		auto id = [&](Vec2i c){
			if(c.x >= int(width) || c.y >= int(height) || c.x < 0 || c.y < 0) {
				return Field::nextNone;
			}
			return c.y * width + c.x;
		};

		std::vector<Field> ret;
		ret.reserve(width * height);
		for(auto y = 0u; y < height; ++y) {
			for(auto x = 0u; x < width; ++x) {
				Field f {};
				f.player = Field::playerNone;
				f.pos.x = x * colWidth;
				f.pos.y = y * rowHeight;
				if(y % 2 == 1) {
					f.pos.x += cospi6 * radius; // half colWidth; shift
				}

				// neighbors
				for(auto i = 0u; i < 6; ++i) {
					auto neighbor = neighborPos(Vec2i{int(x), int(y)}, Field::Side(i));
					f.next[i] = id(neighbor);
				}

				ret.push_back(f);
			}
		}

		ret[0].player = 0u;
		ret[0].type = Field::Type::spawn;
		ret[0].strength = 10.f;

		ret[2].player = 0u;
		ret[2].type = Field::Type::resource;
		ret[2].strength = 10.f;

		ret[ret.size() - 1].player = 1u;
		ret[ret.size() - 1].type = Field::Type::spawn;
		ret[ret.size() - 1].strength = 10.f;

		ret[ret.size() - 3].player = 1u;
		ret[ret.size() - 3].type = Field::Type::resource;
		ret[ret.size() - 3].strength = 10.f;

		return ret;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gfxPipeLayout_, 0, {gfxDs_}, {});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfxPipe_);
		vk::cmdBindVertexBuffers(cb, 0, {storageNew_.buffer()},
			{storageNew_.offset()});

		const nytl::Vec3f black = {0.f, 0.f, 0.f};
		vk::cmdPushConstants(cb, gfxPipeLayout_, vk::ShaderStageBits::fragment,
			0u, sizeof(nytl::Vec3f), &black);
		vk::cmdDraw(cb, 8, fieldCount_, 0, 0);

		const nytl::Vec3f red = {1.f, 0.f, 0.f};
		vk::cmdPushConstants(cb, gfxPipeLayout_, vk::ShaderStageBits::fragment,
			0u, sizeof(nytl::Vec3f), &red);
		vk::cmdDrawIndirect(cb, selectedIndirect_.buffer(),
			selectedIndirect_.offset(), 1, 0);

		// textures
		// vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		// 	texPipeLayout_, 0, {gfxDs_}, {});
		// vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		// 	texPipeLayout_, 1, {texDs_}, {});
		// vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, texPipe_);
		// vk::cmdBindVertexBuffers(cb, 0, {texBuf_.buffer()},
		// 	{texBuf_.offset() + sizeof(vk::DrawIndirectCommand)});
		// vk::cmdDrawIndirect(cb, texBuf_.buffer(),
		// 	texBuf_.offset(), 1, 0);
	}

	// void beforeRender(vk::CommandBuffer cb) override {
	// 	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
	// 		compPipeLayout_, 0, {compDs_}, {});
	// 	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, compPipe_);
	// 	vk::cmdDispatch(cb, fieldCount_, 1, 1);
	// }

	void update(double delta) override {
		App::update(delta);
		App::redraw();

		if(socket_.update([&](auto p, auto& recv){ this->handleMsg(p, recv); })) {
			// XXX: we need ordering of upload/compute command buffers
			// and we can't record the upload cb here since it might
			// be in use. So we do all in updateDevice
			// TODO i guess
			doCompute_ = true;
		}
	}

	void handleMsg(int player, RecvBuf& buf) {
		auto type = doi::read<MessageType>(buf);
		if(type == MessageType::build) {
			auto field = doi::read<std::uint32_t>(buf);
			auto type = doi::read<Field::Type>(buf);

			auto needed = Field::prices[u32(type)];
			if(players_[player].resources < needed) {
				throw std::runtime_error("Protocol error: insufficient resources");
			}

			players_[player].resources -= needed;
			setBuilding(field, type);
			dlg_trace("{}: build {} {}", socket_.step(),
				field, int(type));
		} else if(type == MessageType::velocity) {
			auto field = doi::read<std::uint32_t>(buf);
			auto dir = doi::read<nytl::Vec2f>(buf);
			setVelocity(field, dir);
			dlg_trace("{}: velocity {} {}", socket_.step(),
				field, dir);
		} else {
			throw std::runtime_error("Invalid package");
		}
	}

	void setBuilding(u32 field, Field::Type type) {
		auto offset = uploadPtr_ - stageView_.ptr();
		auto size = sizeof(u32) + sizeof(f32);
		dlg_assert(offset + size < stage_.size());

		doi::write(uploadPtr_, type);
		doi::write(uploadPtr_, 10.f); // strength

		auto off = offsetof(Field, type);
		vk::BufferCopy copy;
		copy.dstOffset = storageNew_.offset() +
			sizeof(Field) * field + off;
		copy.srcOffset = stage_.offset() + offset;
		copy.size = size;
		uploadRegions_.push_back(copy);
	}

	void setVelocity(u32 field, nytl::Vec2f dir) {
		auto offset = uploadPtr_ - stageView_.ptr();
		auto size = sizeof(nytl::Vec2f);
		dlg_assert(offset + size < stage_.size());

		doi::write(uploadPtr_, dir);

		auto off = offsetof(Field, vel);
		vk::BufferCopy copy;
		copy.dstOffset = storageNew_.offset() +
			sizeof(Field) * field + off;
		copy.srcOffset = stage_.offset() + offset;
		copy.size = size;
		uploadRegions_.push_back(copy);
	}

	// void afterRender(vk::CommandBuffer cb) override {
	// 	// TODO: synchronization
	// 	vk::BufferCopy region;
	// 	region.srcOffset = storageNew_.offset();
	// 	region.dstOffset = storageOld_.offset();
	// 	region.size = storageOld_.size();
	// 	vk::cmdCopyBuffer(cb, storageNew_.buffer(), storageOld_.buffer(),
	// 		{region});
	// }

	void updateDevice() override {
		if(updateTransform_) {
			updateTransform_ = false;
			auto map = gfxUbo_.memoryMap();
			auto span = map.span();
			doi::write(span, levelMatrix(view_));
		}

		if(reloadPipes_) {
			reloadPipes_ = false;
			initGfxPipe(true);
			initCompPipe(true);
			rerecord();
			createComputeCb();
		}

		if(updateSelected_) {
			auto map = selectedIndirect_.memoryMap();
			auto span = map.span();
			vk::DrawIndirectCommand cmd;
			cmd.firstInstance = selected_;
			cmd.firstVertex = 0u;
			cmd.instanceCount = 1u;
			cmd.vertexCount = 8u;
			doi::write(span, cmd);
		}

		// sync players
		{
			static u32 outputCounter = 0;
			if(++outputCounter % 100 == 0) {
				outputCounter = 0; // % 500 still 0
			}

			auto map = playerBuffer_.memoryMap();
			auto span = map.span();

			// two way sync. First read, but then subtract the used
			// resources. Relies on the fact that the gpu does never
			// decrease the resource count
			auto i = 0u;
			for(auto& p : players_) {
				auto& gained = doi::refRead<u32>(span);
				p.resources += gained;
				p.netResources += gained;
				gained = 0u; // reset for next turn

				if(outputCounter % 500 == 0) {
					dlg_info("resources {}: {}", i++, p.resources);
				}

				// skip padding
				doi::skip(span, 12);
			}
		}

		if(doCompute_) {
			auto& dev = vulkanDevice();
			vk::SubmitInfo info;
			info.commandBufferCount = 1u;
			info.pCommandBuffers = &compCb_.vkHandle();
			info.signalSemaphoreCount = 1;
			info.pSignalSemaphores = &computeSemaphore_.vkHandle();
			App::addSemaphore(computeSemaphore_, vk::PipelineStageBits::allGraphics);

			if(!uploadRegions_.empty()) {
				vk::beginCommandBuffer(uploadCb_, {});
				vk::cmdCopyBuffer(uploadCb_, stage_.buffer(), storageOld_.buffer(),
					{uploadRegions_});
				vk::endCommandBuffer(uploadCb_);

				vk::SubmitInfo si {};
				si.commandBufferCount = 1u;
				si.pCommandBuffers = &uploadCb_.vkHandle();
				si.signalSemaphoreCount = 1u;
				si.pSignalSemaphores = &uploadSemaphore_.vkHandle();
				vulkanDevice().queueSubmitter().add(si);

				uploadRegions_.clear();
				uploadPtr_ = stageView_.ptr();
				stageView_.flush();

				// wait on semaphore with compute
				info.waitSemaphoreCount = 1;
				info.pWaitSemaphores = &uploadSemaphore_.vkHandle();
				static const auto stage =
					vk::PipelineStageFlags(vk::PipelineStageBits::computeShader);
				info.pWaitDstStageMask = &stage;
			}

			dev.queueSubmitter().add(info);
		}
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button == ny::MouseButton::left) {
			draggingView_ = ev.pressed;
			return true;
		}

		return false;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(!draggingView_) {
			return;
		}

		using namespace nytl::vec::cw::operators;
		auto normed = nytl::Vec2f(ev.delta) / window().size();
		normed.y *= -1.f;
		view_.center -= view_.size * normed;
		updateTransform_ = true;
	}

	bool mouseWheel(const ny::MouseWheelEvent& ev) override {
		if(App::mouseWheel(ev)) {
			return true;
		}

		using namespace nytl::vec::cw::operators;

		auto s = std::pow(0.95f, ev.value.y);
		viewScale_ *= s;
		view_.size *= s;
		updateTransform_ = true;
		return true;
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(ev.pressed) {
			std::optional<Field::Side> side;
			std::optional<Field::Type> type;
			std::optional<nytl::Vec2f> vel;

			const float cospi3 = 0.5;
			const float sinpi3 = 0.86602540378; // cos(pi/6) or sqrt(3)/2;
			switch(ev.keycode) {
				case ny::Keycode::r:
					reloadPipes_ = true;
					return true;
				// movement scheme 1
				// case ny::Keycode::w: side = Field::Side::topLeft; break;
				// case ny::Keycode::e: side = Field::Side::topRight; break;
				// case ny::Keycode::a: side = Field::Side::left; break;
				// case ny::Keycode::d: side = Field::Side::right; break;
				// case ny::Keycode::z: side = Field::Side::botLeft; break;
				// case ny::Keycode::x: side = Field::Side::botRight; break;

				// vim like movement scheme 2
				// TODO: works only for current grid
				// pos should probably be position on grid/pos on grid
				// should be stored somewhere
				case ny::Keycode::j:
					side = ((selected_ / size) % 2 == 1) ?
						Field::Side::botLeft : Field::Side::botRight;
					break;
				case ny::Keycode::k:
					side = ((selected_ / size) % 2 == 1) ?
						Field::Side::topLeft : Field::Side::topRight;
					break;
				case ny::Keycode::h: side = Field::Side::left; break;
				case ny::Keycode::l: side = Field::Side::right; break;

				// actions
				case ny::Keycode::s: type = Field::Type::spawn; break;
				case ny::Keycode::t: type = Field::Type::tower; break;
				case ny::Keycode::v: type = Field::Type::accel; break;
				case ny::Keycode::g: type = Field::Type::resource; break;

				// change velocity
				case ny::Keycode::w: vel = {-cospi3, sinpi3}; break;
				case ny::Keycode::e: vel = {cospi3, sinpi3}; break;
				case ny::Keycode::a: vel = {-1, 0}; break;
				case ny::Keycode::d: vel = {1, 0}; break;
				case ny::Keycode::z: vel = {-cospi3, -sinpi3}; break;
				case ny::Keycode::x: vel = {cospi3, -sinpi3}; break;
				default: break;
			}

			if(side) {
				auto next = fields_[selected_].next[*side];
				if(next != Field::nextNone) {
					selected_ = next;
					updateSelected_ = true;
				}
			}

			// TODO: problem: allow change only on "own" fields
			// should probably be checked when event is processed (after delay)
			// then refund resources if field is unavailable after
			// delay? or say that building was planned to be build
			// but immediately destroyed/resources destroyed?
			if(type) {
				// we will handle it after delay as well as the other side
				auto needed = Field::prices[u32(*type)];
				if(players_[socket_.player()].netResources < needed) {
					dlg_info("Insufficient resources!");
				} else {
					players_[socket_.player()].netResources -= needed;

					auto& buf = socket_.add();
					write(buf, MessageType::build);
					write(buf, u32(selected_));
					write(buf, *type);
				}
			}

			if(vel) {
				// we will handle it after delay as well as the other side
				auto& buf = socket_.add();
				write(buf, MessageType::velocity);
				write(buf, u32(selected_));
				write(buf, *vel);
			}
		}

		return false;
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		view_.size = doi::levelViewSize(ev.size.x / float(ev.size.y), viewScale_);
		updateTransform_ = true;
	}


	bool initGfxPipe(bool dynamic) {
		auto& dev = vulkanDevice();
		vpp::ShaderModule modv, modf;

		if(dynamic) {
			auto omodv = doi::loadShader(dev, "iro.vert");
			auto omodf = doi::loadShader(dev, "iro.frag");
			if(!omodv || !omodf) {
				return false;
			}

			modv = std::move(*omodv);
			modf = std::move(*omodf);
		} else {
			modv = {dev, iro_vert_data};
			modf = {dev, iro_frag_data};
		}

		auto&& rp = renderer().renderPass();
		vpp::GraphicsPipelineInfo gpi(rp,
			gfxPipeLayout_, {{
				{modv, vk::ShaderStageBits::vertex},
				{modf, vk::ShaderStageBits::fragment}}},
			0, renderer().samples());

		constexpr auto fieldStride = sizeof(Field);
		vk::VertexInputBindingDescription bufferBinding {
			0, fieldStride, vk::VertexInputRate::instance
		};

		vk::VertexInputAttributeDescription attributes[5];
		attributes[0].format = vk::Format::r32g32b32Sfloat; // pos
		attributes[0].offset = offsetof(Field, pos);
		attributes[0].location = 0;

		attributes[1].format = vk::Format::r32Uint; // player
		attributes[1].offset = offsetof(Field, player);
		attributes[1].location = 1;

		attributes[2].format = vk::Format::r32Sfloat; // strength
		attributes[2].offset = offsetof(Field, strength);
		attributes[2].location = 2;

		attributes[3].format = vk::Format::r32Uint; // type
		attributes[3].offset = offsetof(Field, type);
		attributes[3].location = 3;

		attributes[4].format = vk::Format::r32g32Sfloat; // velocity
		attributes[4].offset = offsetof(Field, vel);
		attributes[4].location = 4;

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 5u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		gfxPipe_ = {dev, vkpipe};

		return true;
	}

	bool initCompPipe(bool dynamic) {
		auto& dev = vulkanDevice();
		vpp::ShaderModule mod;
		if(dynamic) {
			auto omod = doi::loadShader(dev, "iro.comp");
			if(!omod) {
				return false;
			}

			mod = std::move(*omod);
		} else {
			mod = {dev, iro_comp_data};
		}

		vk::ComputePipelineCreateInfo info;
		info.layout = compPipeLayout_;
		info.stage.module = mod;
		info.stage.pName = "main";
		info.stage.stage = vk::ShaderStageBits::compute;
		vk::Pipeline vkPipeline;
		vk::createComputePipelines(dev, {}, 1, info, nullptr, vkPipeline);
		compPipe_ = {dev, vkPipeline};

		return true;
	}

protected:
	vpp::SubBuffer storageOld_;
	vpp::SubBuffer storageNew_;
	vpp::SubBuffer playerBuffer_;

	// synced from gpu from last step
	std::array<Player, 2> players_;

	vpp::TrDsLayout compDsLayout_;
	vpp::PipelineLayout compPipeLayout_;
	vpp::Pipeline compPipe_;
	vpp::TrDs compDs_;
	vpp::CommandBuffer compCb_;

	vpp::PipelineLayout linePipeLayout_;
	vpp::Pipeline linePipe_;

	vpp::SubBuffer stage_; // used for syncing when needed
	vpp::MemoryMapView stageView_;
	std::byte* uploadPtr_;
	std::vector<vk::BufferCopy> uploadRegions_;
	vpp::CommandBuffer uploadCb_;
	vpp::Semaphore uploadSemaphore_;
	vpp::Semaphore computeSemaphore_;

	std::vector<Field> fields_; // not synced to gpu

	bool updateSelected_ {true};
	u32 selected_ {0};
	vpp::SubBuffer selectedIndirect_; // indirect draw command for selected field

	// render
	vpp::SubBuffer gfxUbo_;
	vpp::TrDsLayout gfxDsLayout_;
	vpp::PipelineLayout gfxPipeLayout_;
	vpp::Pipeline gfxPipe_;
	vpp::Pipeline gfxPipeLines_;
	vpp::TrDs gfxDs_;

	bool reloadPipes_ {false};
	unsigned fieldCount_;

	doi::LevelView view_;
	float viewScale_ {10.f};
	bool updateTransform_ {true};
	bool draggingView_ {false};

	bool doCompute_ {false};

	struct {
		bool lines {};
		float radius {};
		nytl::Vec2f off {};
	} hex_;

	vpp::ViewableImage textures_; // texture array

	// spawn field ids, keeping track to draw spawn images
	// std::vector<unsigned> spawnFields_;
	// bool updateSpawnFieldsBuffer_ {false};

	// vpp::TrDsLayout texDsLayout_;
	// vpp::TrDs texDs_;
	// vpp::PipelineLayout texPipeLayout_;
	// vpp::Pipeline texPipe_;
	// vpp::SubBuffer texBuf_;
	vpp::Sampler sampler_;
	Socket socket_;
};

int main(int argc, const char** argv) {
	HexApp app;
	if(!app.init({"hex", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
