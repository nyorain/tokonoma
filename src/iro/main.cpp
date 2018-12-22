#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/window.hpp>
#include <stage/types.hpp>
#include <stage/bits.hpp>

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
#include <shaders/iro.comp.h>

using namespace doi::types;

// mirrors glsl layout
struct Field {
	enum class Type : u32 {
		empty = 0u,
		resource = 1u,
		spawn = 2u,
		tower = 3u,
	};

	// weakly typed to allow array indexing
	// counter clockwise, like unit circle
	enum Side : u32 {
		right = 0u,
		topRight = 1u,
		topLeft = 2u,
		left = 3u,
		bottomLeft = 4u,
		bottomRight = 5u,
	};

	static constexpr u32 playerNone = 0xFFFFFFFF;
	static constexpr u32 nextNone = 0xFFFFFFFF;

	Vec2f pos;
	u32 player {playerNone};
	Type type {Type::empty};
	f32 strength {};
	std::array<u32, 6> next {nextNone, nextNone, nextNone,
		nextNone, nextNone, nextNone};
};

// Assumption: y=1 has a positive x-offset relative to y=0
// therefore all fields with uneven y have additional x offset
// Also assumes y up
Vec2i neighborPos(Vec2i pos, Field::Side side) {
	switch(side) {
		case Field::Side::left: return {pos.x - 1, pos.y};
		case Field::Side::right: return {pos.x + 1, pos.y};
		case Field::Side::topLeft: return {pos.x + pos.y % 2 - 1, pos.y + 1};
		case Field::Side::bottomLeft: return {pos.x + pos.y % 2 - 1, pos.y - 1};
		case Field::Side::topRight: return {pos.x + pos.y % 2, pos.y + 1};
		case Field::Side::bottomRight: return {pos.x + pos.y % 2, pos.y - 1};
		default: return {-1, -1};
	}
}

class HexApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		// logical
		view_.center = {0.f, 0.f};
		view_.size = {10.f, 10.f};

		// layouts
		auto& dev = vulkanDevice();

		// compute
		auto compDsBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute) // NOTE: not used atm
		};

		compDsLayout_ = {dev, compDsBindings};
		compPipeLayout_ = {dev, {compDsLayout_}, {}};

		// graphics
		auto gfxDsBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex),
		};

		gfxDsLayout_ = {dev, gfxDsBindings};
		gfxPipeLayout_ = {dev, {gfxDsLayout_}, {}};

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
		auto fields = initFields();
		fieldCount_ = fields.size();

		auto usage = vk::BufferUsageFlags(vk::BufferUsageBits::storageBuffer);
		auto storageSize = fields.size() * sizeof(fields[0]);
		storageOld_ = {dev.bufferAllocator(), storageSize,
			usage | vk::BufferUsageBits::transferDst, 0, devMem};
		vpp::writeStaging430(storageOld_, vpp::raw(*fields.data(), fields.size()));

		usage |= vk::BufferUsageBits::transferSrc |
			vk::BufferUsageBits::vertexBuffer;
		storageNew_ = {dev.bufferAllocator(), storageSize, usage, 0, devMem};

		// descriptors
		compDs_ = {dev.descriptorAllocator(), compDsLayout_};
		gfxDs_ = {dev.descriptorAllocator(), gfxDsLayout_};

		vpp::DescriptorSetUpdate compDsUpdate(compDs_);
		compDsUpdate.storage({{storageOld_.buffer(),
			storageOld_.offset(), storageOld_.size()}});
		compDsUpdate.storage({{storageNew_.buffer(),
			storageNew_.offset(), storageNew_.size()}});

		vpp::DescriptorSetUpdate gfxDsUpdate(gfxDs_);
		gfxDsUpdate.uniform({{gfxUbo_.buffer(),
			gfxUbo_.offset(), gfxUbo_.size()}});

		vpp::apply({compDsUpdate, gfxDsUpdate});

		return true;
	}

	std::vector<Field> initFields() {
		constexpr float cospi6 = 0.86602540378; // cos(pi/6) or sqrt(3)/2
		constexpr float radius = 1.f;
		constexpr float rowHeight = 1.5 * radius;
		constexpr float colWidth = 2 * cospi6 * radius;

		constexpr auto height = 10u;
		constexpr auto width = 10u;

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

		ret[ret.size() - 1].player = 1u;
		ret[ret.size() - 1].type = Field::Type::spawn;
		ret[ret.size() - 1].strength = 10.f;

		return ret;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gfxPipeLayout_, 0, {gfxDs_}, {});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfxPipe_);
		vk::cmdBindVertexBuffers(cb, 0, {storageNew_.buffer()},
			{storageNew_.offset()});
		vk::cmdDraw(cb, 6, fieldCount_, 0, 0);
	}

	void update(double delta) override {
		App::update(delta);
		App::redraw();
	}

	void beforeRender(vk::CommandBuffer cb) override {
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			compPipeLayout_, 0, {compDs_}, {});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, compPipe_);
		vk::cmdDispatch(cb, fieldCount_, 1, 1);
	}

	void afterRender(vk::CommandBuffer cb) override {
		// TODO: synchronization
		vk::BufferCopy region;
		region.srcOffset = storageNew_.offset();
		region.dstOffset = storageOld_.offset();
		region.size = storageOld_.size();
		vk::cmdCopyBuffer(cb, storageNew_.buffer(), storageOld_.buffer(),
			{region});
	}

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
			switch(ev.keycode) {
				case ny::Keycode::r:
					reloadPipes_ = true;
					return true;
				default: break;
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

		vk::VertexInputAttributeDescription attributes[3];
		attributes[0].format = vk::Format::r32g32b32Sfloat; // pos
		attributes[0].offset = offsetof(Field, pos);
		attributes[0].location = 0;

		attributes[1].format = vk::Format::r32Uint; // player
		attributes[1].offset = offsetof(Field, player);
		attributes[1].location = 1;

		attributes[2].format = vk::Format::r32Sfloat; // strength
		attributes[2].offset = offsetof(Field, strength);
		attributes[2].location = 2;

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 3u;
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

	vpp::TrDsLayout compDsLayout_;
	vpp::PipelineLayout compPipeLayout_;
	vpp::Pipeline compPipe_;
	vpp::TrDs compDs_;

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

	struct {
		bool lines {};
		float radius {};
		nytl::Vec2f off {};
	} hex_;
};

int main(int argc, const char** argv) {
	HexApp app;
	if(!app.init({"hex", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
