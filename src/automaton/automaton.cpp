#include "automaton.hpp"
#include <stage/bits.hpp>

#include <vpp/vk.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/formats.hpp>

#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>

#include <shaders/hex.vert.h>
#include <shaders/hex_line.vert.h>
#include <shaders/fullscreen_transform.vert.h>
#include <shaders/incolor.frag.h>
#include <shaders/texture.frag.h>

constexpr float cospi6 = 0.86602540378; // cos(pi/6) or sqrt(3)/2

void Automaton::click(std::optional<nytl::Vec2ui>) {}

void Automaton::display(vui::dat::Folder&) {}

void Automaton::render(vk::CommandBuffer cb) {
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfxPipe_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		gfxPipeLayout_, 0, {gfxDs_}, {});

	if(gridType_ == GridType::hex) {
		vk::cmdDraw(cb, 6, size_.x * size_.y, 0, 0);

		if(hex_.lines) {
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics,
				gfxPipeLines_);
			vk::cmdDraw(cb, 6, size_.x * size_.y, 0, 0);
		}
	} else if(gridType_ == GridType::quad) {
		vk::cmdDraw(cb, 4, 1, 0, 0);
	}
}

void Automaton::compute(vk::CommandBuffer cb) {
	// do we need an init barrier? for copy from last frame?
	// which had already the implicit renderpass barrier? probablby.

	if(bufferMode_ == BufferMode::doubled) {
		auto layout = vk::ImageLayout::general;
		auto l = vk::ImageSubresourceLayers{vk::ImageAspectBits::color, 0, 0, 1};
		vk::cmdCopyImage(cb, img_.image(), layout,
			imgBack_.image(), layout, {{l, {}, l, {}, {size_.x, size_.y, 1}}});

		auto subres = vk::ImageSubresourceRange {
			vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::ImageMemoryBarrier barrierBack(
			vk::AccessBits::transferWrite,
			vk::AccessBits::shaderRead,
			layout, layout, {}, {}, imgBack_.image(), subres);

		vk::ImageMemoryBarrier barrierNew(
			vk::AccessBits::transferRead,
			vk::AccessBits::shaderWrite,
			layout, layout, {}, {}, img_.image(), subres);

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {barrierBack, barrierNew});
	}

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, compPipeline_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		compPipelineLayout_, 0, {compDs_}, {});

	if(dispatchCount_) {
		vk::cmdDispatch(cb, *dispatchCount_, 1, 1);
	} else {
		vk::cmdDispatch(cb, size_.x, size_.y, 1);
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

		// center hexagons
		auto radius = 1.f;
		auto off = nytl::Vec {0.f, 0.f};
		float width = radius * 2 * cospi6 * size().x;
		float height = radius * 1.5 * size().y;
		if(height > width) {
			radius = 2 / width;
			off.y = -(radius * 1.5 * size().y - 2) / 2;
		} else {
			radius = 2 / height;
			off.x = -(radius * 2 * cospi6 * size().x - 2) / 2;
		}

		hex_.radius = radius;
		hex_.off = off;

		doi::write(data, off);
		doi::write(data, radius);
	}
}

std::pair<bool, vk::Semaphore> Automaton::updateDevice() {
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
			f.stage = vpp::fillStaging(cb, img_.image(),
				format_, vk::ImageLayout::general, f.size, f.data,
				{vk::ImageAspectBits::color}, f.offset);
			f.data = {};
		}

		oldFill_ = std::move(fill_);
	}

	if(!retrieve_.empty()) {
		auto cb = getRecording();
		for(auto& r : retrieve_) {
			dlg_assert(r.dst);
			*r.dst = vpp::retrieveStaging(cb, img_.image(),
				format_, vk::ImageLayout::general, r.size,
				{vk::ImageAspectBits::color}, r.offset);
		}

		retrieve_ = {};
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
		size += 3 * sizeof(float);
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
			vert = fullscreen_transform_vert_data;
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

	std::vector<vk::GraphicsPipelineCreateInfo> infos = {ginfo.info()};

	std::optional<vpp::GraphicsPipelineInfo> lineInfo;
	vpp::ShaderModule lineVertShader;
	if(gridType_ == GridType::hex) {
		lineVertShader = {device(), hex_line_vert_data};
		lineInfo = {renderPass,
			gfxPipeLayout_, {{
				{lineVertShader, vk::ShaderStageBits::vertex},
				{fragShader, vk::ShaderStageBits::fragment}}}};
		lineInfo->assembly.topology = vk::PrimitiveTopology::lineStrip;
		infos.push_back(lineInfo->info());
	}

	auto pipes = vk::createGraphicsPipelines(device(), {}, infos);
	gfxPipe_ = {device(), pipes[0]};

	if(gridType_ == GridType::hex) {
		gfxPipeLines_ = {device(), pipes[1]};
	}
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

void Automaton::retrieve(Retrieve r) {
	retrieve_.emplace_back(std::move(r));
}

void Automaton::get(nytl::Vec2ui pos, vpp::SubBuffer* dst) {
	auto ipos = nytl::Vec2i(pos);
	retrieve({{ipos.x, ipos.y, 0}, {1, 1, 1}, dst});
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

void Automaton::hexLines(bool set) {
	dlg_assert(gridType() == GridType::hex);
	hex_.lines = set;
}

void Automaton::worldClick(std::optional<nytl::Vec2f> pos) {
	if(!pos) {
		click({});
	}

	auto p = nytl::Vec2f{*pos};
	if(gridType_ == GridType::hex) {
		p += nytl::Vec {1.f, 1.f} - hex_.off;
		unsigned x = p.x / (hex_.radius * 2 * cospi6);
		unsigned y = p.y / (hex_.radius * 1.5);
		if(x >= size_.x || y >= size_.y) {
			click({});
		} else {
			click({{x, y}});
		}
	} else {
		dlg_fatal("unimplemented!");
	}
}

