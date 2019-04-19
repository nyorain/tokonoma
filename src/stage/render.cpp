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

#include <dlg/dlg.hpp>

namespace doi {

// TODO: support for reading depth target in shader
Renderer::Renderer(const vpp::Queue& present) : vpp::Renderer(present) {
	// don't call init here
}

Renderer::Renderer(const RendererCreateInfo& info) :
	vpp::Renderer(info.present), sampleCount_(info.samples),
		clearColor_(info.clearColor) {
	init(info);
}

void Renderer::init(const RendererCreateInfo& info) {
	dlg_assert(present_);

	sampleCount_ = info.samples;
	clearColor_ = info.clearColor;

	vpp::SwapchainPreferences prefs {};
	if(info.vsync) {
		prefs.presentMode = vk::PresentModeKHR::fifo; // vsync
	}

	scInfo_ = vpp::swapchainCreateInfo(info.dev, info.surface,
		{info.size[0], info.size[1]}, prefs);

	depthFormat_ = vk::Format::undefined;
	if(info.depth) {
		// find supported depth format
		vk::ImageCreateInfo img; // dummy for property checking
		img.extent = {1, 1, 1};
		img.mipLevels = 1;
		img.arrayLayers = 1;
		img.imageType = vk::ImageType::e2d;
		img.sharingMode = vk::SharingMode::exclusive;
		img.tiling = vk::ImageTiling::optimal;
		img.samples = sampleCount_;
		img.usage = vk::ImageUsageBits::depthStencilAttachment;
		img.initialLayout = vk::ImageLayout::undefined;

		auto fmts = {
			vk::Format::d32Sfloat,
			vk::Format::d32SfloatS8Uint,
			vk::Format::d24UnormS8Uint,
			vk::Format::d16Unorm,
			vk::Format::d16UnormS8Uint,
		};
		auto features = vk::FormatFeatureBits::depthStencilAttachment |
			vk::FormatFeatureBits::sampledImage;
		depthFormat_ = vpp::findSupported(device(), fmts, img, features);
		if(depthFormat_ == vk::Format::undefined) {
			throw std::runtime_error("No depth format supported");
		}
	}

	createRenderPass();
	vpp::Renderer::init(scInfo_);
}

void Renderer::createRenderPass() {
	vk::AttachmentDescription attachments[3] {};
	auto msaa = sampleCount_ != vk::SampleCountBits::e1;

	auto aid = 0u;
	auto depthid = 0u;
	auto resolveid = 0u;
	auto colorid = 0u;

	// swapchain color attachments
	// msaa: we resolve to this
	// otherwise this is directly rendered
	attachments[aid].format = scInfo_.imageFormat;
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
		attachments[aid].format = scInfo_.imageFormat;
		attachments[aid].samples = sampleCount_;
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
	if(depthFormat_ != vk::Format::undefined) {
		// depth attachment
		attachments[aid].format = depthFormat_;
		attachments[aid].samples = sampleCount_;
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
	if(depthFormat_ != vk::Format::undefined) {
		subpass.pDepthStencilAttachment = &depthReference;
	}

	if(sampleCount_ != vk::SampleCountBits::e1) {
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

	renderPass_ = {device(), renderPassInfo};
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

	auto cmdBuf = buf.commandBuffer;
	vk::beginCommandBuffer(cmdBuf, {});

	if(beforeRender) {
		beforeRender(cmdBuf);
	}

	auto cv = clearValues();
	vk::cmdBeginRenderPass(cmdBuf, {
		renderPass(),
		buf.framebuffer,
		{0u, 0u, width, height},
		std::uint32_t(cv.size()), cv.data()
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
	vpp::Renderer::recreate({size[0], size[1]}, scInfo_);
}

std::vector<vk::ClearValue> Renderer::clearValues() {
	std::vector<vk::ClearValue> clearValues;
	clearValues.reserve(3);
	vk::ClearValue c {{0.f, 0.f, 0.f, 0.f}};

	clearValues.push_back({clearColor_});
	if(sampleCount_ != vk::SampleCountBits::e1) { // msaa attachment
		clearValues.push_back({clearColor_});
	}

	if(depthFormat_ != vk::Format::undefined) {
		clearValues.emplace_back(c).depthStencil = {1.f, 0u};
	}

	return clearValues;
}

void Renderer::samples(vk::SampleCountBits samples) {
	sampleCount_ = samples;
	if(sampleCount_ != vk::SampleCountBits::e1) {
		createMultisampleTarget(scInfo_.imageExtent);
	}

	createRenderPass();

	initBuffers(scInfo_.imageExtent, renderBuffers_);
	invalidate();
}

void Renderer::initBuffers(const vk::Extent2D& size,
		nytl::Span<RenderBuffer> bufs) {
	std::vector<vk::ImageView> attachments {vk::ImageView {}};
	auto scPos = 0u; // attachments[scPos]: swapchain image

	if(sampleCount_ != vk::SampleCountBits::e1) {
		createMultisampleTarget(scInfo_.imageExtent);
		attachments.push_back(multisampleTarget_.vkImageView());
	}

	if(depthFormat_ != vk::Format::undefined) {
		createDepthTarget(scInfo_.imageExtent);
		attachments.push_back(depthTarget_.vkImageView());
	}

	for(auto& buf : bufs) {
		attachments[scPos] = buf.imageView;
		vk::FramebufferCreateInfo info ({},
			renderPass_,
			attachments.size(),
			attachments.data(),
			size.width,
			size.height,
			1);
		buf.framebuffer = {device(), info};
	}
}

} // namespace doi
