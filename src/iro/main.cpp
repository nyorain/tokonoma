// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "network.hpp"

#include <tkn/singlePassApp.hpp>
#include <tkn/shader.hpp>
#include <tkn/transform.hpp>
#include <tkn/levelView.hpp>
#include <tkn/types.hpp>
#include <tkn/bits.hpp>
#include <tkn/texture.hpp>
#include <argagg.hpp>

#include <vpp/fwd.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/vk.hpp>

#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/mat.hpp>

#include <optional>
#include <cstddef>

#include <shaders/iro.iro.vert.h>
#include <shaders/iro.iro.frag.h>
#include <shaders/iro.iro_building.vert.h>
#include <shaders/iro.iro_texture.frag.h>
// #include <shaders/iro_outline.vert.h>
// #include <shaders/iro_outline.frag.h>
#include <shaders/iro.iro.comp.h>

using namespace tkn::types;

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

class HexApp : public tkn::SinglePassApp {
public:
	static constexpr auto size = 32u;
	using Base = tkn::SinglePassApp;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		// network: multiplayer
		if(network_) {
			socket_.emplace();
			player_ = socket_->player();
		}

		// layouts
		auto& dev = vkDevice();

		// resources
		auto images = {
			"../assets/iro/test.png",
			"../assets/iro/spawner.png",
			"../assets/iro/ample.png",
			"../assets/iro/velocity.png"};
		auto p = tkn::loadImageLayers(images);
		textures_ = tkn::buildTexture(dev, std::move(p));

		vk::SamplerCreateInfo samplerInfo {};
		samplerInfo.minFilter = vk::Filter::linear;
		samplerInfo.magFilter = vk::Filter::linear;
		samplerInfo.minLod = 0;
		samplerInfo.maxLod = 0.25;
		sampler_ = {dev, samplerInfo};

		// compute
		auto compDsBindings = std::array {
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

		compDsLayout_.init(dev, compDsBindings);
		compPipeLayout_ = {dev, {{compDsLayout_.vkHandle()}}, {}};

		// graphics
		auto gfxDsBindings = std::array {
			// transform (view) matrix
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex),
			// texture array
			vpp::descriptorBinding(
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_.vkHandle()),
		};

		gfxDsLayout_.init(dev, gfxDsBindings);
		gfxPipeLayout_ = {dev, {{gfxDsLayout_.vkHandle()}},
			{{{vk::ShaderStageBits::fragment, 0u, sizeof(nytl::Vec3f)}}}};

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
			vk::BufferUsageBits::uniformBuffer, hostMem};

		// init fields, storage
		fields_ = initFields();
		fieldCount_ = fields_.size();

		// storageOld_ can be accessed from the host
		auto usage = vk::BufferUsageFlags(vk::BufferUsageBits::storageBuffer);
		auto storageSize = fields_.size() * sizeof(fields_[0]);
		storageOld_ = {dev.bufferAllocator(), storageSize,
			usage | vk::BufferUsageBits::transferDst, hostMem};
		{
			fieldsMap_ = storageOld_.memoryMap();
			auto size = fields_.size() * sizeof(fields_[0]);
			std::memcpy(fieldsMap_.ptr(), fields_.data(), size);
		}

		usage |= vk::BufferUsageBits::transferSrc | vk::BufferUsageBits::vertexBuffer;
		storageNew_ = {dev.bufferAllocator(), storageSize, usage, devMem};

		// player buf
		usage = vk::BufferUsageBits::storageBuffer;
		auto playerSize = sizeof(Player) * 2 + sizeof(u32);
		playerBuffer_ = {dev.bufferAllocator(), playerSize, usage, hostMem};

		{
			Player init;
			init.resources = 0u;
			auto map = playerBuffer_.memoryMap();
			auto span = map.span();
			tkn::write(span, init);
			tkn::write(span, init);
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
		vpp::apply({{{compDsUpdate}, {gfxDsUpdate}}});

		// indirect selected buffer
		selectedIndirect_ = {dev.bufferAllocator(),
			sizeof(vk::DrawIndirectCommand),
			vk::BufferUsageBits::indirectBuffer, hostMem};

		// upload buffer
		// XXX: start size? implement dynamic resizing!
		/*
		stage_ = {dev.bufferAllocator(), 64u, vk::BufferUsageBits::transferSrc,
			0, hostMem};
		stageView_ = stage_.memoryMap();
		uploadPtr_ = stageView_.ptr();
		uploadSemaphore_ = {dev};
		uploadCb_ = dev.commandAllocator().get(
			dev.queueSubmitter().queue().family(),
			vk::CommandPoolCreateBits::resetCommandBuffer);
		*/

		// own compute cb
		computeSemaphore_ = {dev};
		createComputeCb();

		// setup view
		if(player_ == 0) {
			view_.center = fields_[0].pos;
			selected_ = 0;
		} else {
			view_.center = fields_.back().pos;
			selected_ = fields_.size() - 1;
		}


		return true;
	}

	argagg::parser argParser() const override {
		auto parser = App::argParser();
		parser.definitions.push_back({
			"network",
			{"-n", "--network"},
			"Whether to start in network mode", 0
		});
		return parser;
	}

	bool handleArgs(const argagg::parser_results& result,
			Args& out) override {
		if(!Base::handleArgs(result, out)) {
			return false;
		}

		network_ = result["network"].count();
		return true;
	}

	void createComputeCb() {
		auto& dev = vkDevice();
		auto qfamily = dev.queueSubmitter().queue().family();
		compCb_ = dev.commandAllocator().get(qfamily);
		vk::beginCommandBuffer(compCb_, {});
		vk::cmdBindDescriptorSets(compCb_, vk::PipelineBindPoint::compute,
			compPipeLayout_, 0, {{compDs_.vkHandle()}}, {});
		vk::cmdBindPipeline(compCb_, vk::PipelineBindPoint::compute, compPipe_);
		vk::cmdDispatch(compCb_, fieldCount_ / 32, 1, 1);

		vk::BufferMemoryBarrier barrier;
		barrier.buffer = storageNew_.buffer();
		barrier.srcAccessMask = vk::AccessBits::shaderWrite;
		barrier.dstAccessMask = vk::AccessBits::transferRead;
		barrier.offset = 0;
		barrier.size = storageNew_.size();
		vk::cmdPipelineBarrier(compCb_,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::transfer,
			{}, {}, {{barrier}}, {});

		vk::BufferCopy region;
		region.srcOffset = storageNew_.offset();
		region.dstOffset = storageOld_.offset();
		region.size = storageOld_.size();
		vk::cmdCopyBuffer(compCb_, storageNew_.buffer(), storageOld_.buffer(),
			{{region}});

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
			gfxPipeLayout_, 0, {{gfxDs_.vkHandle()}}, {});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfxPipe_);
		vk::cmdBindVertexBuffers(cb, 0, {{storageNew_.buffer().vkHandle()}},
			{{storageNew_.offset()}});

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
		App::scheduleRedraw();

		if(socket_ && !paused_) {
			auto r = socket_->update([&](auto p, auto& recv){
				this->handleMsg(p, recv);
			});
			if(r) {
				// XXX: we need ordering of upload/compute command buffers
				// and we can't record the upload cb here since it might
				// be in use. So we do all in updateDevice
				// TODO i guess
				doCompute_ = true;
			}
		} else if(!paused_) {
			doCompute_ = true;
		}
	}

	void handleMsg(int player, RecvBuf& buf) {
		auto type = tkn::read<MessageType>(buf);
		if(type == MessageType::build) {
			auto field = tkn::read<std::uint32_t>(buf);
			auto type = tkn::read<Field::Type>(buf);

			auto needed = Field::prices[u32(type)];
			if(players_[player].resources < needed) {
				throw std::runtime_error("Protocol error: insufficient resources");
			}

			// no matter if action is succesful or not in the end
			// if not, the resources are lost (building was planned to be
			// build was it couldn't be)
			players_[player].resources -= needed;

			setBuilding(player, field, type);
			dlg_trace("{}: build {} {}", socket_->step(),
				field, int(type));
		} else if(type == MessageType::velocity) {
			auto field = tkn::read<std::uint32_t>(buf);
			auto dir = tkn::read<nytl::Vec2f>(buf);
			setVelocity(player, field, dir);
			dlg_trace("{}: velocity {} {}", socket_->step(),
				field, dir);
		} else {
			throw std::runtime_error("Invalid package");
		}
	}

	void setBuilding(u32 player, u32 field, Field::Type type) {
		Command cmd;
		cmd.type = CommandType::setBuilding;
		cmd.build = type;
		cmd.player = player;
		cmd.field = field;
		commands_.push_back(cmd);

		// auto offset = uploadPtr_ - stageView_.ptr();
		// auto size = sizeof(u32) + sizeof(f32);
		// dlg_assert(offset + size < stage_.size());
//
		// tkn::write(uploadPtr_, type);
		// tkn::write(uploadPtr_, 10.f); // strength
//
		// auto off = offsetof(Field, type);
		// vk::BufferCopy copy;
		// copy.dstOffset = storageNew_.offset() +
		// 	sizeof(Field) * field + off;
		// copy.srcOffset = stage_.offset() + offset;
		// copy.size = size;
		// uploadRegions_.push_back(copy);
	}

	void setVelocity(u32 player, u32 field, nytl::Vec2f dir) {
		Command cmd;
		cmd.type = CommandType::setVel;
		cmd.velocity = dir;
		cmd.player = player;
		cmd.field = field;
		commands_.push_back(cmd);
		// auto offset = uploadPtr_ - stageView_.ptr();
		// auto size = sizeof(nytl::Vec2f);
		// dlg_assert(offset + size < stage_.size());
//
		// tkn::write(uploadPtr_, dir);
//
		// auto off = offsetof(Field, vel);
		// vk::BufferCopy copy;
		// copy.dstOffset = storageNew_.offset() +
		// 	sizeof(Field) * field + off;
		// copy.srcOffset = stage_.offset() + offset;
		// copy.size = size;
		// uploadRegions_.push_back(copy);
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

	nytl::Span<Field> deviceFields() {
		fieldsMap_.invalidate();
		auto ptr = reinterpret_cast<Field*>(fieldsMap_.ptr());
		auto size = fieldsMap_.size() / sizeof(Field);
		return {ptr, ptr + size};
	}

	void updateDevice() override {
		if(updateTransform_) {
			updateTransform_ = false;
			auto map = gfxUbo_.memoryMap();
			auto span = map.span();
			tkn::write(span, levelMatrix(view_));
		}

		if(reloadPipes_) {
			reloadPipes_ = false;
			initGfxPipe(true);
			initCompPipe(true);
			scheduleRerecord();
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
			tkn::write(span, cmd);
		}

		// sync players
		{
			static u32 outputCounter = 0; // TODO

			auto map = playerBuffer_.memoryMap();
			auto span = map.span();

			// two way sync. First read, but then subtract the used
			// resources. Relies on the fact that the gpu does never
			// decrease the resource count
			for(auto& p : players_) {
				auto& gained = tkn::refRead<u32>(span);
				p.resources += gained;
				p.netResources += gained; // only needed for player_
				gained = 0u; // reset for next turn

				// skip padding
				tkn::skip(span, 12);
			}

			// write step
			tkn::write(span, u32(step_));

			if(++outputCounter % 60 == 0) {
				outputCounter = 0;
				dlg_info("resources: {}", players_[player_].resources);
			}
		}

		if(doCompute_) {
			++step_;
			doCompute_ = false;

			// exeucte commands, upload stuff
			auto fields = deviceFields();
			for(auto& cmd : commands_) {
				auto& field = fields[cmd.field];
				if(cmd.type == CommandType::sendVel) {
					dlg_assert(cmd.player == player_);
					if(field.player != cmd.player) {
						dlg_info("Cannot perform operation on enemy field");
						continue;
					}

					if(socket_) {
						auto& buf = socket_->add();
						write(buf, MessageType::velocity);
						write(buf, u32(cmd.field));
						write(buf, cmd.velocity);
					} else {
						// set it directly
						// TODO: directly fwd to CommandType::setVelocity in that case?
						field.vel = cmd.velocity;
					}
				} else if(cmd.type == CommandType::sendBuilding) {
					dlg_assert(cmd.player == player_);
					if(field.player != cmd.player) {
						dlg_info("Cannot perform operation on enemy field");
						continue;
					}

					// check resources
					auto needed = Field::prices[u32(cmd.build)];
					if(players_[cmd.player].netResources < needed) {
						dlg_info("Insufficient resources!");
						continue;
					}

					auto& p = players_[cmd.player];
					dlg_assert(p.netResources <= p.resources);
					p.netResources -= needed;

					if(socket_) {
						auto& buf = socket_->add();
						write(buf, MessageType::build);
						write(buf, u32(selected_));
						write(buf, cmd.build);
					} else {
						// set it directly
						// TODO: directly fwd to CommandType::setBuilding in that case?
						field.type = cmd.build;
						field.strength = 10.f;
					}
				} else if(cmd.type == CommandType::setVel) {
					if(field.player != cmd.player) {
						dlg_warn("Cannot perform operation on enemy field (set)");
						continue;
					}

					field.vel = cmd.velocity;
				} else if(cmd.type == CommandType::setBuilding) {
					if(field.player != cmd.player) {
						dlg_warn("Cannot perform operation on enemy field (set)");
						continue;
					}

					field.type = cmd.build;
					field.strength = 10.f;
				}
			}

			commands_.clear();
			fieldsMap_.flush();

			auto& dev = vkDevice();
			vk::SubmitInfo info;
			info.commandBufferCount = 1u;
			info.pCommandBuffers = &compCb_.vkHandle();
			info.signalSemaphoreCount = 1;
			info.pSignalSemaphores = &computeSemaphore_.vkHandle();
			App::addSemaphore(computeSemaphore_, vk::PipelineStageBits::allGraphics);

			/*
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
			*/

			dev.queueSubmitter().add(info);
		}
	}

	bool mouseButton(const swa_mouse_button_event& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button == swa_mouse_button_left) {
			draggingView_ = ev.pressed;
			return true;
		}

		return false;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		App::mouseMove(ev);
		if(!draggingView_) {
			return;
		}

		using namespace nytl::vec::cw::operators;
		auto normed = nytl::Vec2f{float(ev.dx), float(ev.dy)} / windowSize();
		normed.y *= -1.f;
		view_.center -= view_.size * normed;
		updateTransform_ = true;
	}

	bool mouseWheel(float dx, float dy) override {
		if(App::mouseWheel(dx, dy)) {
			return true;
		}

		using namespace nytl::vec::cw::operators;

		auto s = std::pow(0.95f, dy);
		viewScale_ *= s;
		view_.size *= s;
		updateTransform_ = true;
		return true;
	}

	bool key(const swa_key_event& ev) override {
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
				case swa_key_r:
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
				case swa_key_j:
					side = ((selected_ / size) % 2 == 1) ?
						Field::Side::botLeft : Field::Side::botRight;
					break;
				case swa_key_k:
					side = ((selected_ / size) % 2 == 1) ?
						Field::Side::topLeft : Field::Side::topRight;
					break;
				case swa_key_h: side = Field::Side::left; break;
				case swa_key_l: side = Field::Side::right; break;

				// actions
				case swa_key_s: type = Field::Type::spawn; break;
				case swa_key_t: type = Field::Type::tower; break;
				case swa_key_v: type = Field::Type::accel; break;
				case swa_key_g: type = Field::Type::resource; break;

				// change velocity
				case swa_key_w: vel = {-cospi3, sinpi3}; break;
				case swa_key_e: vel = {cospi3, sinpi3}; break;
				case swa_key_a: vel = {-1, 0}; break;
				case swa_key_d: vel = {1, 0}; break;
				case swa_key_z: vel = {-cospi3, -sinpi3}; break;
				case swa_key_x: vel = {cospi3, -sinpi3}; break;

				case swa_key_p: paused_ = !paused_; break;
				default: break;
			}

			if(side) {
				auto next = fields_[selected_].next[*side];
				if(next != Field::nextNone) {
					selected_ = next;
					updateSelected_ = true;
				}
			}

			// TODO: check here already if netResources is large enough?
			if(type) {
				Command cmd;
				cmd.type = CommandType::sendBuilding;
				cmd.field = selected_;
				cmd.player = player_;
				cmd.build = *type;
				commands_.push_back(cmd);
			}

			if(vel) {
				Command cmd;
				cmd.type = CommandType::sendVel;
				cmd.field = selected_;
				cmd.player = player_;
				cmd.velocity = *vel;
				commands_.push_back(cmd);
			}
		}

		return false;
	}

	void resize(unsigned width, unsigned height) override {
		App::resize(width, height);
		view_.size = tkn::levelViewSize(width / float(height), viewScale_);
		updateTransform_ = true;
	}


	bool initGfxPipe(bool dynamic) {
		auto& dev = vkDevice();
		vpp::ShaderModule modv, modf;

		if(dynamic) {
			auto omodv = tkn::loadShader(dev, "iro.vert");
			auto omodf = tkn::loadShader(dev, "iro.frag");
			if(!omodv || !omodf) {
				return false;
			}

			modv = std::move(*omodv);
			modf = std::move(*omodf);
		} else {
			modv = {dev, iro_iro_vert_data};
			modf = {dev, iro_iro_frag_data};
		}

		auto&& rp = renderPass();
		vpp::GraphicsPipelineInfo gpi(rp, gfxPipeLayout_, {{{
				{modv, vk::ShaderStageBits::vertex},
				{modf, vk::ShaderStageBits::fragment}
		}}}, 0, samples());

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
		auto& dev = vkDevice();
		vpp::ShaderModule mod;
		if(dynamic) {
			auto omod = tkn::loadShader(dev, "iro.comp");
			if(!omod) {
				return false;
			}

			mod = std::move(*omod);
		} else {
			mod = {dev, iro_iro_comp_data};
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

	const char* name() const override { return "iro"; }

protected:
	// TODO: we could make storageOld_ device local and use yet another
	// buffer to which we copy every frame
	vpp::SubBuffer storageOld_;
	vpp::SubBuffer storageNew_;
	vpp::SubBuffer playerBuffer_;
	vpp::MemoryMapView fieldsMap_; // map of storageOld_

	// synced from gpu from last step
	std::array<Player, 2> players_;

	vpp::TrDsLayout compDsLayout_;
	vpp::PipelineLayout compPipeLayout_;
	vpp::Pipeline compPipe_;
	vpp::TrDs compDs_;
	vpp::CommandBuffer compCb_;

	vpp::PipelineLayout linePipeLayout_;
	vpp::Pipeline linePipe_;

	/*
	vpp::SubBuffer stage_; // used for syncing when needed
	vpp::MemoryMapView stageView_;
	std::byte* uploadPtr_;
	std::vector<vk::BufferCopy> uploadRegions_;
	vpp::CommandBuffer uploadCb_;
	vpp::Semaphore uploadSemaphore_;
	*/

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

	tkn::LevelView view_;
	float viewScale_ {20.f};
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

	bool network_;
	std::optional<Socket> socket_;
	u32 player_{0};

	// deferred commands that only be executed in updateDevice phase
	enum class CommandType {
		sendVel,
		sendBuilding,
		setVel,
		setBuilding,
	};

	struct Command {
		CommandType type;
		u32 player;
		u32 field;
		Field::Type build;
		nytl::Vec2f velocity;
	};
	std::vector<Command> commands_;

	bool paused_ {false}; // for debugging/testing

	// TODO: redundant with socket().step
	u32 step_ {};
};

int main(int argc, const char** argv) {
	HexApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}
	app.run();
}
