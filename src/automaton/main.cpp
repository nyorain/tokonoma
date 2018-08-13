#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/bits.hpp>
#include <stage/transform.hpp>

#include <vui/dat.hpp>

#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/formats.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/pipelineInfo.hpp>

#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/stringParam.hpp>

#include <shaders/predprey.comp.h>

// TODO: correct shaders
#include <shaders/hex.vert.h>
#include <shaders/fullscreen.vert.h>
#include <shaders/incolor.frag.h>
#include <shaders/texture.frag.h>

#include <random>
#include <optional>

class Automaton {
public:
	enum class GridType {
		quad,
		hex,
	};

	enum class BufferMode {
		single,
		doubled
	};

	enum class ResizeMode {
		scaleLinear,
		scaleNearest,
		clear
	};

public:
	virtual ~Automaton() = default;

	virtual void click(nytl::Vec2ui pos);
	virtual void settings(vui::dat::Folder&);

	virtual void compute(vk::CommandBuffer);
	virtual void render(vk::CommandBuffer);
	virtual void resize(nytl::Vec2ui);
	virtual void transform(nytl::Mat4f t) { transform_ = t; }
	virtual std::pair<bool, vk::Semaphore> updateDevice(double);

	const auto& size() const { return size_; }
	const auto& img() const { return img_; }
	GridType gridType() const { return gridType_; }

	vpp::Device& device() { return *dev_; }
	const vpp::Device& device() const { return *dev_; }

protected:
	struct Fill {
		vk::Offset3D offset;
		vk::Extent3D size;
		std::vector<std::byte> data;
		vpp::SubBuffer stage {};
	};

	Automaton() = default;
	void init(vpp::Device& dev,
		vk::RenderPass, nytl::Span<const std::uint32_t> compShader,
		nytl::Vec2ui size, BufferMode buffer, vk::Format,
		nytl::Span<const std::uint32_t> vert = {},
		nytl::Span<const std::uint32_t> frag = {},
		std::optional<unsigned> count = {},
		GridType grid = GridType::quad,
		ResizeMode = ResizeMode::scaleNearest);

	void rerecord() { rerecord_ = true; }
	void gridType(GridType);
	void resizeMode(ResizeMode);
	void dispatchCount(std::optional<unsigned>);
	void set(nytl::Vec2ui pos, std::vector<std::byte> data);
	void fill(Fill);
	vk::CommandBuffer getRecording();

	virtual void initCompPipe(nytl::Span<const std::uint32_t> shader);
	virtual void initGfxPipe(vk::RenderPass,
		nytl::Span<const std::uint32_t> vert,
		nytl::Span<const std::uint32_t> frag);
	virtual void initImages();
	virtual void initLayouts();
	virtual void initSampler();
	virtual void initBuffers(unsigned additionalGfxSize = 0u);

	virtual void compDsUpdate(vpp::DescriptorSetUpdate&);
	virtual void compDsLayout(std::vector<vk::DescriptorSetLayoutBinding>&);
	virtual void compPipeLayout(
		std::vector<vk::DescriptorSetLayout>&,
		std::vector<vk::PushConstantRange>&);

	virtual void gfxDsUpdate(vpp::DescriptorSetUpdate&);
	virtual void gfxDsLayout(std::vector<vk::DescriptorSetLayoutBinding>&);
	virtual void gfxPipeLayout(
		std::vector<vk::DescriptorSetLayout>&,
		std::vector<vk::PushConstantRange>&);
	virtual void writeGfxData(nytl::Span<std::byte>& data);

private:
	vpp::Device* dev_;
	vpp::ViewableImage img_; // compute shader writes this, rendering reads
	vpp::ViewableImage imgBack_; // potentially unused back buffer
	vpp::ViewableImage imgOld_; // for resizing, blitting

	vpp::TrDsLayout compDsLayout_;
	vpp::PipelineLayout compPipelineLayout_;
	vpp::Pipeline compPipeline_;

	vpp::TrDs compDs_;
	bool rerecord_ {};
	vpp::Semaphore uploadSemaphore_ {};
	vpp::CommandBuffer uploadCb_ {};
	bool cbUsed_ {};

	vk::Format format_;
	GridType gridType_;
	ResizeMode resizeMode_;
	BufferMode bufferMode_;
	std::optional<unsigned> dispatchCount_;

	nytl::Vec2ui size_;
	std::optional<nytl::Vec2ui> resize_ {};

	std::vector<Fill> fill_;
	std::vector<Fill> oldFill_;

	// render
	vpp::SubBuffer gfxUbo_;
	vpp::Sampler sampler_;
	vpp::TrDsLayout gfxDsLayout_;
	vpp::PipelineLayout gfxPipeLayout_;
	vpp::Pipeline gfxPipe_;
	vpp::TrDs gfxDs_;
	std::optional<nytl::Mat4f> transform_;
};

void Automaton::click(nytl::Vec2ui) {}
void Automaton::settings(vui::dat::Folder&) {} // TODO: resize?

void Automaton::render(vk::CommandBuffer cb) {
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfxPipe_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		gfxPipeLayout_, 0, {gfxDs_}, {});

	if(gridType_ == GridType::hex) {
		vk::cmdDraw(cb, 6, size_.x * size_.y, 0, 0);
	} else if(gridType_ == GridType::quad) {
		vk::cmdDraw(cb, 4, 1, 0, 0);
	}
}

void Automaton::compute(vk::CommandBuffer cb) {
	// do we need an init barrier? for copy from last frame?
	// which had already the implicit renderpass barrier? probablby.

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, compPipeline_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		compPipelineLayout_, 0, {compDs_}, {});

	if(dispatchCount_) {
		vk::cmdDispatch(cb, *dispatchCount_, 1, 1);
	} else {
		vk::cmdDispatch(cb, size_.x, size_.y, 1);
	}

	if(bufferMode_ == BufferMode::doubled) {
		auto layout = vk::ImageLayout::general;
		auto subres = vk::ImageSubresourceRange {
			vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::ImageMemoryBarrier barrierOld(
			vk::AccessBits::shaderRead,
			vk::AccessBits::transferWrite,
			layout, layout, {}, {}, imgBack_.image(), subres);

		vk::ImageMemoryBarrier barrierNew(
			vk::AccessBits::shaderWrite,
			vk::AccessBits::transferRead,
			layout, layout, {}, {}, img_.image(), subres);

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {barrierOld, barrierNew});

		auto l = vk::ImageSubresourceLayers{vk::ImageAspectBits::color, 0, 0, 1};
		vk::cmdCopyImage(cb, img_.image(), layout,
			imgBack_.image(), layout, {{l, {}, l, {}, {size_.x, size_.y, 1}}});
	}
}

void Automaton::initSampler() {
	vk::SamplerCreateInfo samplerInfo;
	samplerInfo.magFilter = vk::Filter::linear;
	samplerInfo.minFilter = vk::Filter::linear;
	samplerInfo.mipmapMode = vk::SamplerMipmapMode::nearest;
	samplerInfo.addressModeU = vk::SamplerAddressMode::clampToBorder;
	samplerInfo.addressModeV = vk::SamplerAddressMode::clampToBorder;
	samplerInfo.addressModeW = vk::SamplerAddressMode::clampToBorder;
	samplerInfo.borderColor = vk::BorderColor::intOpaqueWhite;
	samplerInfo.mipLodBias = 0;
	samplerInfo.anisotropyEnable = false;
	samplerInfo.maxAnisotropy = 1.0;
	samplerInfo.compareEnable = false;
	samplerInfo.compareOp = {};
	samplerInfo.minLod = 0;
	samplerInfo.maxLod = 0.25;
	samplerInfo.borderColor = vk::BorderColor::intTransparentBlack;
	samplerInfo.unnormalizedCoordinates = false;
	sampler_ = {device(), samplerInfo};
}

void Automaton::resize(nytl::Vec2ui size) {
	if(size != size_) {
		resize_ = size;
	}
}

void Automaton::writeGfxData(nytl::Span<std::byte>& data) {
	if(transform_) {
		doi::write(data, *transform_);
		transform_ = {};
	} else { // skip
		auto s = sizeof(nytl::Mat4f);
		data.slice(s, data.size() - s);
	}

	if(gridType_ == GridType::hex) {
		doi::write(data, std::uint32_t(size_.x));
		doi::write(data, std::uint32_t(size_.y));
	}
}

std::pair<bool, vk::Semaphore> Automaton::updateDevice(double) {
	oldFill_.clear();
	imgOld_ = {};

	auto rec = rerecord_;
	if(resize_) {
		size_ = *resize_;
		resize_ = {};
		rec = true;

		if(resizeMode_ != ResizeMode::clear) {
			imgOld_ = std::move(img_);
		}

		initImages();

		{
			vpp::DescriptorSetUpdate update(compDs_);
			compDsUpdate(update);
		}

		{
			vpp::DescriptorSetUpdate update(gfxDs_);
			gfxDsUpdate(update);
		}

		// TODO: copy (blit) data
	}

	if(transform_) {
		auto map = gfxUbo_.memoryMap();
		auto span = map.span();
		writeGfxData(span);
	}

	if(!fill_.empty()) {
		auto cb = getRecording();
		for(auto& f : fill_) {
			f.stage = vpp::fillStaging(cb, imgBack_.image(),
				format_, vk::ImageLayout::general, f.size, f.data,
				{vk::ImageAspectBits::color}, f.offset);
			f.data = {};

		}

		oldFill_ = std::move(fill_);
	}

	vk::Semaphore sem {};
	if(cbUsed_) {
		vk::endCommandBuffer(uploadCb_);

		vk::SubmitInfo si;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &uploadCb_.vkHandle();
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &uploadSemaphore_.vkHandle();
		device().queueSubmitter().add(si);

		sem = uploadSemaphore_;
		cbUsed_ = false;
	}

	rerecord_ = false;
	return {rec, sem};
}

vk::CommandBuffer Automaton::getRecording() {
	if(!cbUsed_) {
		vk::beginCommandBuffer(uploadCb_, {});
		cbUsed_ = true;
	}

	return uploadCb_;
}

void Automaton::initBuffers(unsigned additionalSize) {
	auto size = sizeof(nytl::Mat4f) + additionalSize;
	if(gridType_ == GridType::hex) {
		size += 2 * sizeof(std::uint32_t);
	}

	auto memBits = device().hostMemoryTypes();
	gfxUbo_ = {device().bufferAllocator(), size,
		vk::BufferUsageBits::uniformBuffer, 16u, memBits};
}

void Automaton::initCompPipe(nytl::Span<const std::uint32_t> shader) {
	// pipeline
	auto computeShader = vpp::ShaderModule(device(), shader);

	vk::ComputePipelineCreateInfo info;
	info.layout = compPipelineLayout_;
	info.stage.module = computeShader;
	info.stage.pName = "main";
	info.stage.stage = vk::ShaderStageBits::compute;

	vk::Pipeline vkPipeline;
	vk::createComputePipelines(device(), {}, 1, info, nullptr, vkPipeline);
	compPipeline_ = {device(), vkPipeline};
}

void Automaton::initGfxPipe(vk::RenderPass renderPass,
		nytl::Span<const std::uint32_t> vert,
		nytl::Span<const std::uint32_t> frag) {

	if(vert.empty()) {
		if(gridType_ == GridType::hex) {
			vert = hex_vert_data;
		} else {
			vert = fullscreen_vert_data;
		}
	}

	if(frag.empty()) {
		if(gridType_ == GridType::hex) {
			frag = incolor_frag_data;
		} else {
			frag = texture_frag_data;
		}
	}

	vpp::ShaderModule vertShader(device(), vert);
	vpp::ShaderModule fragShader(device(), frag);
	vpp::GraphicsPipelineInfo ginfo(renderPass,
		gfxPipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment}}});

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(device(), {}, 1, ginfo.info(),
		nullptr, vkpipe);
	gfxPipe_ = {device(), vkpipe};
}

void Automaton::initLayouts() {
	// compute
	std::vector<vk::DescriptorSetLayoutBinding> bindings;
	compDsLayout(bindings);
	compDsLayout_ = {device(), bindings};

	std::vector<vk::PushConstantRange> pcr;
	std::vector<vk::DescriptorSetLayout> layouts;
	compPipeLayout(layouts, pcr);
	compPipelineLayout_ = {device(), layouts, pcr};

	// graphics
	bindings.clear();
	pcr.clear();
	layouts.clear();

	gfxDsLayout(bindings);
	gfxDsLayout_ = {device(), bindings};

	gfxPipeLayout(layouts, pcr);
	gfxPipeLayout_ = {device(), layouts, pcr};
}

void Automaton::initImages() {
	// storage buffer for data
	auto mem = device().memoryTypeBits(vk::MemoryPropertyBits::deviceLocal);
	auto usage = vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::storage |
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::transferDst;

	auto imgInfo = vpp::ViewableImageCreateInfo::color(device(),
		{size_.x, size_.y, 1}, usage, {format_}).value();
	img_ = {device(), imgInfo, mem};

	auto cb = getRecording();
	vpp::changeLayout(cb, img_.image(),
		vk::ImageLayout::undefined, vk::PipelineStageBits::topOfPipe, {},
		vk::ImageLayout::general, vk::PipelineStageBits::transfer,
			vk::AccessBits::transferWrite,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});

	if(bufferMode_ == BufferMode::doubled) {
		imgBack_ = {device(), imgInfo, mem};
		vpp::changeLayout(cb, imgBack_.image(),
			vk::ImageLayout::undefined, vk::PipelineStageBits::topOfPipe, {},
			vk::ImageLayout::general, vk::PipelineStageBits::transfer,
				vk::AccessBits::transferWrite,
				{vk::ImageAspectBits::color, 0, 1, 0, 1});
	}
}

void Automaton::compDsUpdate(vpp::DescriptorSetUpdate& update) {
	auto layout = vk::ImageLayout::general;
	update.storage({{{}, img_.imageView(), layout}});
	if(bufferMode_ == BufferMode::doubled) {
		update.storage({{{}, imgBack_.imageView(), layout}});
	}
}

void Automaton::gfxDsUpdate(vpp::DescriptorSetUpdate& update) {
	auto layout = vk::ImageLayout::general;
	update.uniform({{gfxUbo_.buffer(), gfxUbo_.offset(), gfxUbo_.size()}});
	if(gridType_ == GridType::hex) {
		update.imageSampler({{{}, img_.imageView(), layout}});
	} else {
		update.imageSampler({{{}, img_.imageView(), layout}});
	}
}

void Automaton::init(vpp::Device& dev, vk::RenderPass rp,
		nytl::Span<const std::uint32_t> shader,
		nytl::Vec2ui size, BufferMode buffer, vk::Format format,
		nytl::Span<const std::uint32_t> vert,
		nytl::Span<const std::uint32_t> frag,
		std::optional<unsigned> count, GridType grid, ResizeMode resize) {

	dev_ = &dev;
	format_ = format;
	bufferMode_ = buffer;
	size_ = size;
	resizeMode_ = resize;
	dispatchCount_ = count;
	gridType_ = grid;

	uploadSemaphore_ = {device()};
	uploadCb_ = device().commandAllocator().get(
		device().queueSubmitter().queue().family());

	initSampler();
	initLayouts();
	initCompPipe(shader);
	initGfxPipe(rp, vert, frag);
	initBuffers();
	initImages();

	compDs_ = {dev.descriptorAllocator(), compDsLayout_};
	gfxDs_ = {dev.descriptorAllocator(), gfxDsLayout_};

	{
		vpp::DescriptorSetUpdate update(compDs_);
		compDsUpdate(update);
	}

	{
		vpp::DescriptorSetUpdate update(gfxDs_);
		gfxDsUpdate(update);
	}
}

void Automaton::gridType(GridType grid) {
	gridType_ = grid;
}

void Automaton::resizeMode(ResizeMode mode) {
	resizeMode_ = mode;
}

void Automaton::dispatchCount(std::optional<unsigned> count) {
	if(count == dispatchCount_) {
		return;
	}

	dispatchCount_ = count;
	rerecord_ = true;
}

void Automaton::set(nytl::Vec2ui pos, std::vector<std::byte> data) {
	auto ipos = nytl::Vec2i(pos);
	auto f = Fill {{ipos.x, ipos.y, 0}, {1, 1, 1}, std::move(data)};
	fill_.emplace_back(std::move(f));
}

void Automaton::fill(Fill f) {
	fill_.emplace_back(std::move(f));
}

void Automaton::compDsLayout(std::vector<vk::DescriptorSetLayoutBinding>& bs) {
	bs.push_back(vpp::descriptorBinding(vk::DescriptorType::storageImage,
		vk::ShaderStageBits::compute));
	if(bufferMode_ == BufferMode::doubled) {
		bs.push_back(vpp::descriptorBinding(vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute));
	}
}

void Automaton::gfxDsLayout(std::vector<vk::DescriptorSetLayoutBinding>& bs) {
	bs.push_back(vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
		vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment));
	bs.push_back(vpp::descriptorBinding(
		vk::DescriptorType::combinedImageSampler,
		vk::ShaderStageBits::fragment | vk::ShaderStageBits::vertex,
		-1, 1, &sampler_.vkHandle()));
}

void Automaton::compPipeLayout(
		std::vector<vk::DescriptorSetLayout>& ds,
		std::vector<vk::PushConstantRange>&) {
	ds.push_back(compDsLayout_);
}

void Automaton::gfxPipeLayout(
		std::vector<vk::DescriptorSetLayout>& ds,
		std::vector<vk::PushConstantRange>&) {
	ds.push_back(gfxDsLayout_);
}

class PredPrey : public Automaton {
public:
	PredPrey() = default;
	PredPrey(vpp::Device& dev, vk::RenderPass);
	void init(vpp::Device& dev, vk::RenderPass);

	// void click(nytl::Vec2ui pos) override;
	// void settings(vui::dat::Folder&) override;
	std::pair<bool, vk::Semaphore> updateDevice(double) override;

	void initBuffers(unsigned) override;
	void compDsUpdate(vpp::DescriptorSetUpdate&) override;
	void compDsLayout(std::vector<vk::DescriptorSetLayoutBinding>&) override;

protected:
	struct {
		float preyBirth = 0.1;
		float preyDeathPerPred = 0.2;
		float predBirthPerPrey = 0.1;
		float predDeath = 0.04;
		float predWander = 0.01;
		float preyWander = 0.001;
	} params_;
	bool paramsChanged_ {true};
	vpp::SubBuffer ubo_;
};

PredPrey::PredPrey(vpp::Device& dev, vk::RenderPass rp) {
	init(dev, rp);
}

void PredPrey::init(vpp::Device& dev, vk::RenderPass rp) {
	auto size = nytl::Vec2ui {256, 256};
	Automaton::init(dev, rp, predprey_comp_data, size,
		BufferMode::doubled, vk::Format::r8g8b8a8Unorm,
		{}, {}, {}, GridType::hex);

	std::vector<std::byte> data(sizeof(nytl::Vec4u8) * size.x * size.y);

	std::mt19937 rgen;
	rgen.seed(std::time(nullptr));
	std::uniform_int_distribution<std::uint8_t> distr(0, 255);

	auto* ptr = reinterpret_cast<nytl::Vec4u8*>(data.data());
	for(auto i = 0u; i < size.x * size.y; ++i) {
		ptr[i] = {distr(rgen), distr(rgen), 0u, 0u};
	}

	fill({{0, 0, 0}, {size.x, size.y, 1}, data});
}

void PredPrey::initBuffers(unsigned add) {
	Automaton::initBuffers(add);
	auto memBits = device().hostMemoryTypes();
	ubo_ = {device().bufferAllocator(), sizeof(float) * 6,
		vk::BufferUsageBits::uniformBuffer, 16u, memBits};
}

std::pair<bool, vk::Semaphore> PredPrey::updateDevice(double delta) {
	// TODO: insert host memory barrier for ubo in compute
	if(paramsChanged_) {
		auto map = ubo_.memoryMap();
		auto span = map.span();
		doi::write(span, params_);
	}

	return Automaton::updateDevice(delta);
}

void PredPrey::compDsUpdate(vpp::DescriptorSetUpdate& update) {
	Automaton::compDsUpdate(update);
	update.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
}

void PredPrey::compDsLayout(std::vector<vk::DescriptorSetLayoutBinding>& b) {
	Automaton::compDsLayout(b);
	b.push_back(vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
		vk::ShaderStageBits::compute));
}

class Ant : public Automaton {
public:
	Ant(vpp::Device& dev);

protected:
	struct {
		unsigned count;
		std::vector<nytl::Vec4f> colors;
		std::vector<uint32_t> movement;
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

		automaton_.init(vulkanDevice(), renderer().renderPass());

		auto mat = nytl::identity<4, float>();
		doi::scale(mat, nytl::Vec{2.f, 2.f});
		doi::translate(mat, nytl::Vec{-1.f, -1.f});
		automaton_.transform(mat);

		return true;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		automaton_.compute(cb);
	}

	void render(vk::CommandBuffer cb) override {
		automaton_.render(cb);
	}

	void update(double delta) override {
		App::update(delta);
		delta_ = delta;
	}

	void updateDevice() override {
		auto [rec, sem] = automaton_.updateDevice(delta_);
		if(rec) {
			rerecord();
		}

		if(sem) {
			addSemaphore(sem, vk::PipelineStageBits::topOfPipe);
		}
	}

protected:
	double delta_ {};
	PredPrey automaton_;
};

int main(int argc, const char** argv) {
	AutomatonApp app;
	if(!app.init({"automaton", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}


/*
class AutomatonRenderer {
public:
	AutomatonRenderer() = default;
	AutomatonRenderer(const Automaton&);

	void render(vk::CommandBuffer);

	void automaton(const Automaton&);
	const Automaton& automaton() const { return *automaton_; }

	const vpp::Device& device() const { return automaton().device(); }

protected:
	struct {
		vpp::Pipeline fullTex;
		vpp::Pipeline hex;
		// vpp::Pipeline hexLines; // TODO
	} pipes_;

	vpp::Sampler sampler_;
	vpp::DescriptorSetLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;

	const Automaton* automaton_ {};
	vpp::DescriptorSet ds_;
};

AutomatonRenderer::AutomatonRenderer(const Automaton& automaton) {
	this->automaton(automaton);
}

void AutomatonRenderer::render(vk::CommandBuffer cb) {
	dlg_assert(automaton_);
	auto size = automaton().size();
	if(automaton().gridType() == Automaton::GridType::hex) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipes_.hex);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {ds_}, {});
		vk::cmdDraw(cb, size.x * size.y, 1, 0, 0);
	} else if(automaton().gridType() == Automaton::GridType::quad) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipes_.hex);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {ds_}, {});
		vk::cmdDraw(cb, 6, size.x * size.y, 0, 0);
	}
}

void AutomatonRenderer::automaton(const Automaton& automaton) {
	if(!automaton_) {
		auto bindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::vertex),
		};

		pipeLayout_ = {dev, gbindings};
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

	} else {
		dlg_assert(&sampler_.device() == &automaton.device());
	}

	automaton_ = &automaton;
}
*/
