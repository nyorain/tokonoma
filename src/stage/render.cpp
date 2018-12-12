// Copyright (c) 2017 nyorain
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt

#define DLG_DEFAULT_TAGS "render",

#include <stage/render.hpp>

#include <nytl/mat.hpp>
#include <vpp/vk.hpp>
#include <vpp/util/file.hpp>
#include <vpp/renderPass.hpp>
#include <vpp/swapchain.hpp>
#include <vpp/formats.hpp>

#include <dlg/dlg.hpp> // dlg

// TODO: support for reading depth target in shader

vpp::RenderPass createRenderPass(const vpp::Device&, vk::Format,
	vk::SampleCountBits, vk::Format depthFormat = vk::Format::undefined);

Renderer::Renderer(const RendererCreateInfo& info) :
	DefaultRenderer(info.present), sampleCount_(info.samples),
		clearColor_(info.clearColor) {

	vpp::SwapchainPreferences prefs {};
	if(info.vsync) {
		prefs.presentMode = vk::PresentModeKHR::fifo; // vsync
	}

	scInfo_ = vpp::swapchainCreateInfo(info.dev, info.surface,
		{info.size[0], info.size[1]}, prefs);

	depthFormat_ = vk::Format::undefined;
	if(info.depth) {
		// TODO: search for supported one
		depthFormat_ = vk::Format::d32Sfloat;
	}

	renderPass_ = createRenderPass(info.dev, scInfo_.imageFormat, samples(),
		depthFormat_);
	vpp::DefaultRenderer::init(renderPass_, scInfo_);
}

void Renderer::createDepthTarget(const vk::Extent2D& size) {
	auto width = size.width;
	auto height = size.height;

	// img
	vk::ImageCreateInfo img;
	img.imageType = vk::ImageType::e2d;
	img.format = depthFormat_;
	img.extent.width = width;
	img.extent.height = height;
	img.extent.depth = 1;
	img.mipLevels = 1;
	img.arrayLayers = 1;
	img.sharingMode = vk::SharingMode::exclusive;
	img.tiling = vk::ImageTiling::optimal;
	img.samples = sampleCount_;
	img.usage = vk::ImageUsageBits::depthStencilAttachment;
	img.initialLayout = vk::ImageLayout::undefined;

	// view
	vk::ImageViewCreateInfo view;
	view.viewType = vk::ImageViewType::e2d;
	view.format = img.format;
	view.components = {};
	view.subresourceRange.aspectMask = vk::ImageAspectBits::depth;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;

	// create the viewable image
	// will set the created image in the view info for us
	depthTarget_ = {device(), img, view};
}

void Renderer::createMultisampleTarget(const vk::Extent2D& size) {
	auto width = size.width;
	auto height = size.height;

	// img
	vk::ImageCreateInfo img;
	img.imageType = vk::ImageType::e2d;
	img.format = scInfo_.imageFormat;
	img.extent.width = width;
	img.extent.height = height;
	img.extent.depth = 1;
	img.mipLevels = 1;
	img.arrayLayers = 1;
	img.sharingMode = vk::SharingMode::exclusive;
	img.tiling = vk::ImageTiling::optimal;
	img.samples = sampleCount_;
	img.usage = vk::ImageUsageBits::transientAttachment | vk::ImageUsageBits::colorAttachment;
	img.initialLayout = vk::ImageLayout::undefined;

	// view
	vk::ImageViewCreateInfo view;
	view.viewType = vk::ImageViewType::e2d;
	view.format = img.format;
	view.components.r = vk::ComponentSwizzle::r;
	view.components.g = vk::ComponentSwizzle::g;
	view.components.b = vk::ComponentSwizzle::b;
	view.components.a = vk::ComponentSwizzle::a;
	view.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;

	// create the viewable image
	// will set the created image in the view info for us
	multisampleTarget_ = {device(), img, view};
}

void Renderer::record(const RenderBuffer& buf) {
	const auto width = scInfo_.imageExtent.width;
	const auto height = scInfo_.imageExtent.height;

	auto clearCount = 1u;
	vk::ClearValue clearValues[3] {};
	clearValues[0].color.float32 = clearColor_;
	if(sampleCount_ != vk::SampleCountBits::e1) { // msaa attachment
		clearValues[clearCount].color.float32 = clearColor_;
		++clearCount;
	}

	if(depthFormat_ != vk::Format::undefined) {
		clearValues[clearCount].depthStencil = {1.f, 0u};
		++clearCount;
	}

	auto cmdBuf = buf.commandBuffer;
	vk::beginCommandBuffer(cmdBuf, {});

	if(beforeRender) {
		beforeRender(cmdBuf);
	}

	vk::cmdBeginRenderPass(cmdBuf, {
		renderPass(),
		buf.framebuffer,
		{0u, 0u, width, height},
		clearCount,
		clearValues
	}, {});

	vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
	vk::cmdSetViewport(cmdBuf, 0, 1, vp);
	vk::cmdSetScissor(cmdBuf, 0, 1, {0, 0, width, height});

	if(onRender) {
		onRender(cmdBuf);
	}

	vk::cmdEndRenderPass(cmdBuf);

	if(afterRender) {
		afterRender(cmdBuf);
	}

	vk::endCommandBuffer(cmdBuf);
}

void Renderer::resize(nytl::Vec2ui size) {
	vpp::DefaultRenderer::recreate({size[0], size[1]}, scInfo_);
}

void Renderer::samples(vk::SampleCountBits samples) {
	sampleCount_ = samples;
	if(sampleCount_ != vk::SampleCountBits::e1) {
		createMultisampleTarget(scInfo_.imageExtent);
	}

	renderPass_ = createRenderPass(device(), scInfo_.imageFormat, samples);
	vpp::DefaultRenderer::renderPass_ = renderPass_;

	initBuffers(scInfo_.imageExtent, renderBuffers_);
	invalidate();
}

void Renderer::initBuffers(const vk::Extent2D& size,
		nytl::Span<RenderBuffer> bufs) {
	std::vector<vk::ImageView> views {vk::ImageView {}}; // swapchain image
	if(sampleCount_ != vk::SampleCountBits::e1) {
		createMultisampleTarget(scInfo_.imageExtent);
		views.push_back(multisampleTarget_.vkImageView());
	}

	if(depthFormat_ != vk::Format::undefined) {
		createDepthTarget(scInfo_.imageExtent);
		views.push_back(depthTarget_.vkImageView());
	}

	vpp::DefaultRenderer::initBuffers(size, bufs, views);
}

// util
vpp::RenderPass createRenderPass(const vpp::Device& dev,
		vk::Format format, vk::SampleCountBits sampleCount,
		vk::Format depthFormat) {

	vk::AttachmentDescription attachments[3] {};
	auto msaa = sampleCount != vk::SampleCountBits::e1;

	auto aid = 0u;
	auto depthid = 0u;
	auto resolveid = 0u;
	auto colorid = 0u;

	// swapchain color attachments
	// msaa: we resolve to this
	// otherwise this is directly rendered
	attachments[aid].format = format;
	attachments[aid].samples = vk::SampleCountBits::e1;
	attachments[aid].storeOp = vk::AttachmentStoreOp::store;
	attachments[aid].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[aid].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[aid].initialLayout = vk::ImageLayout::undefined;
	attachments[aid].finalLayout = vk::ImageLayout::presentSrcKHR;
	if(msaa) {
		attachments[aid].loadOp = vk::AttachmentLoadOp::dontCare;
		resolveid = aid;
	} else {
		attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
		colorid = aid;
	}
	++aid;

	// optiontal multisampled render target
	if(msaa) {
		// multisample color attachment
		attachments[aid].format = format;
		attachments[aid].samples = sampleCount;
		attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
		attachments[aid].storeOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachments[aid].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].initialLayout = vk::ImageLayout::undefined;
		attachments[aid].finalLayout = vk::ImageLayout::presentSrcKHR;

		colorid = aid;
		++aid;
	}

	// optional depth target
	if(depthFormat != vk::Format::undefined) {
		// depth attachment
		attachments[aid].format = depthFormat;
		attachments[aid].samples = sampleCount;
		attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
		attachments[aid].storeOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].stencilLoadOp = vk::AttachmentLoadOp::clear;
		attachments[aid].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].initialLayout = vk::ImageLayout::undefined;
		attachments[aid].finalLayout = vk::ImageLayout::depthStencilAttachmentOptimal;

		depthid = aid;
		++aid;
	}

	// refs
	vk::AttachmentReference colorReference;
	colorReference.attachment = colorid;
	colorReference.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::AttachmentReference resolveReference;
	resolveReference.attachment = resolveid;
	resolveReference.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::AttachmentReference depthReference;
	depthReference.attachment = depthid;
	depthReference.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

	// deps
	std::vector<vk::SubpassDependency> dependencies;

	// TODO: do we really need this? isn't this detected by default?
	if(msaa) {
		dependencies.resize(2);

		dependencies[0].srcSubpass = vk::subpassExternal;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = vk::PipelineStageBits::bottomOfPipe;
		dependencies[0].dstStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dependencies[0].srcAccessMask = vk::AccessBits::memoryRead;
		dependencies[0].dstAccessMask = vk::AccessBits::colorAttachmentRead |
			vk::AccessBits::colorAttachmentWrite;
		dependencies[0].dependencyFlags = vk::DependencyBits::byRegion;

		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = vk::subpassExternal;
		dependencies[1].srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dependencies[1].dstStageMask = vk::PipelineStageBits::bottomOfPipe;
		dependencies[1].srcAccessMask = vk::AccessBits::colorAttachmentRead |
			vk::AccessBits::colorAttachmentWrite;
		dependencies[1].dstAccessMask = vk::AccessBits::memoryRead;
		dependencies[1].dependencyFlags = vk::DependencyBits::byRegion;
	}

	// only subpass
	vk::SubpassDescription subpass {};
	subpass.pipelineBindPoint = vk::PipelineBindPoint::graphics;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorReference;
	if(depthFormat != vk::Format::undefined) {
		subpass.pDepthStencilAttachment = &depthReference;
	}

	if(sampleCount != vk::SampleCountBits::e1) {
		subpass.pResolveAttachments = &resolveReference;
	}

	// most general dependency
	// should cover almost all cases of external access to data that
	// is read during a render pass (host, transfer, compute shader)
	vk::SubpassDependency dependency;
	dependency.srcSubpass = vk::subpassExternal;
	dependency.srcStageMask =
		vk::PipelineStageBits::host |
		vk::PipelineStageBits::computeShader |
		vk::PipelineStageBits::colorAttachmentOutput |
		vk::PipelineStageBits::transfer;
	dependency.srcAccessMask = vk::AccessBits::hostWrite |
		vk::AccessBits::shaderWrite |
		vk::AccessBits::transferWrite |
		vk::AccessBits::colorAttachmentWrite;
	dependency.dstSubpass = 0u;
	dependency.dstStageMask = vk::PipelineStageBits::allGraphics;
	dependency.dstAccessMask = vk::AccessBits::uniformRead |
		vk::AccessBits::vertexAttributeRead |
		vk::AccessBits::indirectCommandRead |
		vk::AccessBits::shaderRead;
	dependencies.push_back(dependency);

	vk::RenderPassCreateInfo renderPassInfo;
	renderPassInfo.attachmentCount = aid;
	renderPassInfo.pAttachments = attachments;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = dependencies.size();
	renderPassInfo.pDependencies = dependencies.data();

	return {dev, renderPassInfo};
}
