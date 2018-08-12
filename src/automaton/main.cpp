#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/bits.hpp>

#include <vui/dat.hpp>

#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/pipelineInfo.hpp>

#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/stringParam.hpp>

#include <shaders/predprey.comp.h>
#include <shaders/hex.vert.h>
#include <shaders/incolor.frag.h>

#include <random>

class Automaton {
public:
	enum class GridType {
		quad,
		hex,
	};

	enum class BufferType {
		single,
		doubled
	};

	enum class ResizeMode {
		scaleLinear,
		scaleNearest,
		clear
	};

public:
	virtual void click(nytl::Vec2ui pos);
	virtual void settings(vui::dat::Folder&);

	virtual void compute(vk::CommandBuffer);
	virtual void resize(nytl::Vec2ui);
	virtual std::pair<bool, vk::Semaphore> updateDevice(double);

	const auto& size() const { return size_; }
	const auto& img() const { return img_; }
	GridType gridType() const { return gridType_; }

protected:
	Automaton() = default;
	void init(vpp::Device& dev, nytl::StringParam computePath,
		nytl::Vec2ui size, BufferType buffer, vk::Format,
		std::optional<unsigned> count,
		GridType grid = GridType::quad,
		ResizeMode = ResizeMode::scaleNearest);

	void rerecord() { rerecord_ = true; }
	void gridType(GridType);
	void resizeMode(ResizeMode);
	void dispatchCount(std::optional<unsigned>);
	void set(nytl::Vec2ui pos, std::vector<std::byte> data);

	virtual void pipeLayout(std::vector<vk::DescriptorSetLayoutBinding>&);
	virtual void dsLayout(
		std::vector<vk::DescriptorSetLayout>&,
		std::vector<vk::PushConstantRange>&);

private:
	vpp::ViewableImage img_; // compute shader writes this, rendering reads
	vpp::ViewableImage imgBack_; // potentially unused back buffer
	vpp::ViewableImage imgOld_; // for resizing, blitting

	vpp::Pipeline compPipeline_;
	vpp::PipelineLayout compPipelineLayout_;
	vpp::TrDsLayout compDsLayout_;

	vpp::TrDs compDs_;
	bool rerecord_ {};

	vk::Format format_;
	GridType gridType_;
	ResizeMode resizeMode_;
	BufferType bufferType_;
	std::optional<unsigned> dispatchCount_;

	nytl::Vec2ui size_;
	nytl::Vec2ui resize_;

	struct Fill {
		vk::Extent3D offset;
		vk::Extent3D size;
		std::vector<std::byte> data;
	};

	std::vector<Fill> fill_;
};

class PredPrey : public Automaton {
public:
	PredPrey(vpp::Device& dev);

protected:
	struct {
		float preyBirth;
		float preyDeathPerPred;
		float predBirthPerPrey;
		float predDeath;
		float predWander;
		float preyWander;
	} params_;
	vpp::SubBuffer ubo_;
};

class Ant : public Automaton {
public:
	Ant(vpp::Device& dev);

protected:
	struct {
		unsigned count_;
		std::vector<nytl::Vec4f> colors_;
		std::vector<uint32_t> movement_;
	} params_;
	vpp::SubBuffer ubo_;
};

class GameOfLife : public Automaton {
protected:
	struct {
		uint32_t neighbors; // 4, 6 (hex), 8, 12
		uint32_t max; // death threshold
		uint32_t min; // bearth threshold
	} params_;
};

// Like game of life but each pixel has a scalar value.
class ScalarGameOfLife : public Automaton {
protected:
	struct {
		float fac;
		float min;
		float max;
	} params_;
};

class AutomatonRenderer {
public:
	AutomatonRenderer() = default;
	AutomatonRenderer(const Automaton&);

	void render(vk::CommandBuffer);

	void automaton(const Automaton&);
	const Automaton& automaton() const { return *automaton_; }

protected:
	struct {
		vpp::PipelineLayout hexLayout;
		vpp::Pipeline hex;
		vpp::PipelineLayout hexLinesLayout;
		vpp::Pipeline hexLines;
		vpp::PipelineLayout fullTexLayout;
		vpp::Pipeline fullTex;
	} pipes_;

	vpp::Sampler linear_;
	vpp::Sampler nearest_;
	vpp::DescriptorSetLayout dsLayout_;

	const Automaton* automaton_ {};
	vpp::DescriptorSet ds_;
};

// frame concept (init values are in storageOld):
// - compute: read from storageOld, write into storageNew
// * barrier: make sure reading from storageOld (from shaders) is finished
// * barrier: make sure writing into storageNew (from shader) is finished
// - copy storageNew into storageOld (overwrite old values, for next frame)
// - render: read new values from storage2 (renderPass has external dependency
//   that will make sure copying completed)

class AutomatonApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		auto& dev = vulkanDevice();
		gridSize_ = {512, 512};

		vk::DescriptorPoolSize typeCounts[1] {};
		typeCounts[0].type = vk::DescriptorType::storageBuffer;
		typeCounts[0].descriptorCount = 2;

		// layout
		auto bindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute)
		};

		compDsLayout_ = {dev, bindings};
		compDs_ = {dev.descriptorAllocator(), compDsLayout_};
		compPipelineLayout_ = {dev, {compDsLayout_}, {}};

		// pipeline
		auto computeShader = vpp::ShaderModule(dev, predprey_comp_data);

		vk::ComputePipelineCreateInfo info;
		info.layout = compPipelineLayout_;
		info.stage.module = computeShader;
		info.stage.pName = "main";
		info.stage.stage = vk::ShaderStageBits::compute;

		vk::Pipeline vkPipeline;
		vk::createComputePipelines(dev, {}, 1, info, nullptr, vkPipeline);
		compPipeline_ = {dev, vkPipeline};

		// storage buffer for data
		auto mem = dev.memoryTypeBits(vk::MemoryPropertyBits::hostVisible);
		auto usage = vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::transferDst |
			vk::BufferUsageBits::transferSrc;

		dev.bufferAllocator().reserve(2 * bufSize() + 128, usage, 16u, mem);
		storageOld_ = {dev.bufferAllocator(), bufSize(),
			usage, 16u, mem};
		storageNew_ = {dev.bufferAllocator(), bufSize(),
			usage, 16u, mem};

		std::vector<nytl::Vec2f> data(gridSize_.x * gridSize_.y);

		std::mt19937 rgen;
		rgen.seed(std::time(nullptr));
		std::uniform_real_distribution<float> distr(0.f, 1.f);

		for(auto i = 0u; i < data.size(); ++i) {
			data[i] = {distr(rgen), distr(rgen)};
		}

		// data[5 * gridSize_.x + 5] = {1.f, 0.5f};
		// data[10 * gridSize_.x + 10] = {0.2f, 0.01f};
		// data[48 * gridSize_.x + 20] = {1.f, 0.00f};
		// data[50 * gridSize_.x + 20] = {1.f, 0.00f};
		// data[53 * gridSize_.x + 20] = {1.f, 0.4f};
		// data[120 * gridSize_.x + 120] = {0.1f, 1.f};
		// data[150 * gridSize_.x + 20] = {0.1f, 1.f};
		// data[250 * gridSize_.x + 250] = {1.f, 1.f};

		vpp::writeStaging430(storageOld_, vpp::rawSpan(data));
		vpp::writeStaging430(storageNew_, vpp::rawSpan(data));

		// update descriptor
		{
			vpp::DescriptorSetUpdate update(compDs_);
			update.storage({{storageOld_.buffer(), storageOld_.offset(),
				bufSize()}});
			update.storage({{storageNew_.buffer(), storageNew_.offset(),
				bufSize()}});
		}

		// graphics
		auto gbindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex),
		};

		gfxDsLayout_ = {dev, gbindings};
		gfxDs_ = {dev.descriptorAllocator(), gfxDsLayout_};
		gfxPipelineLayout_ = {dev, {gfxDsLayout_}, {}};
		gfxUbo_ = {dev.bufferAllocator(), sizeof(nytl::Mat4f) + 4,
			vk::BufferUsageBits::uniformBuffer, 16u, mem};

		{
			auto map = gfxUbo_.memoryMap();
			auto span = map.span();

			auto mat = nytl::identity<4, float>();
			mat[0][0] = 0.02;
			mat[1][1] = 0.02;
			mat[0][3] = -1.01;
			mat[1][3] = -1.01;

			doi::write(span, mat);
			doi::write(span, std::uint32_t(gridSize_.x));

			vpp::DescriptorSetUpdate update(gfxDs_);
			update.uniform({{gfxUbo_.buffer(), gfxUbo_.offset(),
				gfxUbo_.size()}});
		}

		vpp::ShaderModule hexVert(dev, hex_vert_data);
		vpp::ShaderModule colorFrag(dev, incolor_frag_data);
		vpp::GraphicsPipelineInfo ginfo(renderer().renderPass(),
			gfxPipelineLayout_, {{
				{hexVert, vk::ShaderStageBits::vertex},
				{colorFrag, vk::ShaderStageBits::fragment}}});

		vk::VertexInputBindingDescription binding;
		binding.binding = 0;
		binding.inputRate = vk::VertexInputRate::instance;
		binding.stride = sizeof(nytl::Vec2f);

		vk::VertexInputAttributeDescription attribute;
		attribute.offset = 0;
		attribute.binding = 0;
		attribute.location = 0;
		attribute.format = vk::Format::r32g32Sfloat;

		ginfo.vertex.vertexBindingDescriptionCount = 1;
		ginfo.vertex.pVertexBindingDescriptions = &binding;
		ginfo.vertex.vertexAttributeDescriptionCount = 1;
		ginfo.vertex.pVertexAttributeDescriptions = &attribute;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {}, 1, ginfo.info(),
			nullptr, vkpipe);
		gfxPipeline_ = {dev, vkpipe};

		return true;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, compPipeline_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			compPipelineLayout_, 0, {compDs_}, {});
		vk::cmdDispatch(cb, gridSize_.x, gridSize_.y, 1);

		// ideally we could ping pong them but that is somewhat hard here
		// since we can't know for certain that we have an even number of
		// command buffers to record (or in which order they are executed)
		vk::BufferMemoryBarrier barrierOld(
			vk::AccessBits::shaderRead,
			vk::AccessBits::transferWrite,
			{}, {}, storageOld_.buffer(), storageOld_.offset(), bufSize());

		vk::BufferMemoryBarrier barrierNew(
			vk::AccessBits::shaderWrite,
			vk::AccessBits::transferRead,
			{}, {}, storageNew_.buffer(), storageNew_.offset(), bufSize());

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::transfer,
			{}, {}, {barrierOld, barrierNew}, {});

		vk::cmdCopyBuffer(cb, storageNew_.buffer(), storageOld_.buffer(),
			{{storageNew_.offset(), storageOld_.offset(), bufSize()}});

		// not really needed due to external dep in renderPass?
		// auto barrier3 = barrierOld;
		// barrier3.srcAccessMask = vk::AccessBits::transferWrite;
		// barrier3.dstAccessMask = vk::AccessBits::vertexAttributeRead;
		// vk::cmdPipelineBarrier(
		// 	cb,
		// 	vk::PipelineStageBits::transfer,
		// 	vk::PipelineStageBits::vertexInput,
		// 	{}, {}, {barrier3}, {});
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfxPipeline_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gfxPipelineLayout_, 0, {gfxDs_}, {});
		// vk::cmdBindVertexBuffers(cb, 0, {storageNew_.buffer()}, {storageNew_.offset()});
		vk::cmdBindVertexBuffers(cb, 0, {storageOld_.buffer()}, {storageOld_.offset()});
		vk::cmdDraw(cb, 6, gridSize_.x * gridSize_.y, 0, 0);
	}

	void update(double delta) override {
		App::update(delta);
	}

	vk::DeviceSize bufSize() const {
		return gridSize_.x * gridSize_.y * sizeof(float) * 2;
	}

protected:
	vpp::SubBuffer storageOld_;
	vpp::SubBuffer storageNew_;
	vpp::Pipeline compPipeline_;
	vpp::PipelineLayout compPipelineLayout_;
	vpp::TrDsLayout compDsLayout_;
	vpp::TrDs compDs_;

	vpp::SubBuffer gfxUbo_;
	vpp::TrDsLayout gfxDsLayout_;
	vpp::TrDs gfxDs_;
	vpp::Pipeline gfxPipeline_;
	vpp::PipelineLayout gfxPipelineLayout_;

	nytl::Vec2ui gridSize_;
};

int main(int argc, const char** argv) {
	AutomatonApp app;
	if(!app.init({"automaton", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
