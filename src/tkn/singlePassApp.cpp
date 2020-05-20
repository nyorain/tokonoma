#include <tkn/singlePassApp.hpp>
#include <tkn/render.hpp>
#include <vpp/vk.hpp>
#include <vpp/formats.hpp>
#include <dlg/dlg.hpp>
#include <argagg.hpp>

namespace tkn {

bool SinglePassApp::doInit(nytl::Span<const char*> args, Args& out) {
	if(!App::doInit(args, out)) {
		return false;
	}

	if(needsDepth()) {
		depthFormat_ = findDepthFormat(vkDevice());
		if(depthFormat_ == vk::Format::undefined) {
			dlg_error("No depth format supported");
			return false;
		}
	}

	// TODO: verify that the requested number of samples is supported

	rp_ = createRenderPass();
	return true;
}

void SinglePassApp::initBuffers(const vk::Extent2D& size,
		nytl::Span<RenderBuffer> bufs) {
	std::vector<vk::ImageView> attachments {vk::ImageView {}};
	auto scPos = 0u; // attachments[scPos]: swapchain image

	if(samples() != vk::SampleCountBits::e1) {
		msTarget_ = createMultisampleTarget(size);
		attachments.push_back(msTarget_.vkImageView());
	}

	if(depthFormat() != vk::Format::undefined && needsDepth()) {
		depthTarget_ = createDepthTarget(size);
		attachments.push_back(depthTarget_.vkImageView());
	}

	for(auto& buf : bufs) {
		attachments[scPos] = buf.imageView;
		vk::FramebufferCreateInfo info ({},
			renderPass(),
			attachments.size(),
			attachments.data(),
			size.width,
			size.height,
			1);
		buf.framebuffer = {vkDevice(), info};
	}
}

void SinglePassApp::record(const RenderBuffer& buf) {
	const auto width = swapchainInfo().imageExtent.width;
	const auto height = swapchainInfo().imageExtent.height;

	auto cb = buf.commandBuffer;
	vk::beginCommandBuffer(cb, {});
	beforeRender(cb);

	auto cv = clearValues();
	vk::cmdBeginRenderPass(cb, {
		renderPass(),
		buf.framebuffer,
		{0u, 0u, width, height},
		std::uint32_t(cv.size()), cv.data()
	}, {});

	vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
	vk::cmdSetViewport(cb, 0, 1, vp);
	vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

	render(cb);
	vk::cmdEndRenderPass(cb);
	afterRender(cb);
	vk::endCommandBuffer(cb);
}

vpp::RenderPass SinglePassApp::createRenderPass() {
	vk::AttachmentDescription attachments[3] {};
	auto msaa = samples() != vk::SampleCountBits::e1;

	auto aid = 0u;
	auto depthid = 0u;
	auto resolveid = 0u;
	auto colorid = 0u;

	// swapchain color attachments
	// msaa: we resolve to this
	// otherwise this is directly rendered
	attachments[aid].format = swapchainInfo().imageFormat;
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

	// optional multisample render target
	if(msaa) {
		// multisample color attachment
		attachments[aid].format = swapchainInfo().imageFormat;
		attachments[aid].samples = samples();
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
	if(depthFormat() != vk::Format::undefined && needsDepth()) {
		// depth attachment
		attachments[aid].format = depthFormat();
		attachments[aid].samples = samples();
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
	if(depthFormat() != vk::Format::undefined && needsDepth()){
		subpass.pDepthStencilAttachment = &depthReference;
	}

	if(samples() != vk::SampleCountBits::e1) {
		subpass.pResolveAttachments = &resolveReference;
	}

	vk::RenderPassCreateInfo renderPassInfo;
	renderPassInfo.attachmentCount = aid;
	renderPassInfo.pAttachments = attachments;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = 0;
	renderPassInfo.pDependencies = nullptr;

	return {vkDevice(), renderPassInfo};
}

vpp::ViewableImage SinglePassApp::createDepthTarget(const vk::Extent2D& size) {
	// We don't allow the depth attachment to be sampled, since
	// apps deriving from this will be simple, only having
	// one render pass.
	auto usage = vk::ImageUsageBits::depthStencilAttachment;
	auto info = vpp::ViewableImageCreateInfo(depthFormat(),
		vk::ImageAspectBits::depth, size, usage);

	// create the viewable image
	// will set the created image in the view info for us
	return {vkDevice().devMemAllocator(), info, vkDevice().deviceMemoryTypes()};
}

vpp::ViewableImage SinglePassApp::createMultisampleTarget(const vk::Extent2D& size) {
	auto width = size.width;
	auto height = size.height;

	// img
	vk::ImageCreateInfo img;
	img.imageType = vk::ImageType::e2d;
	img.format = swapchainInfo().imageFormat;
	img.extent.width = width;
	img.extent.height = height;
	img.extent.depth = 1;
	img.mipLevels = 1;
	img.arrayLayers = 1;
	img.sharingMode = vk::SharingMode::exclusive;
	img.tiling = vk::ImageTiling::optimal;
	img.samples = samples();
	img.usage =
		vk::ImageUsageBits::transientAttachment |
		vk::ImageUsageBits::colorAttachment;
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
	return {vkDevice().devMemAllocator(), img, view,
		vkDevice().deviceMemoryTypes()};
}

std::vector<vk::ClearValue> SinglePassApp::clearValues() {
	std::vector<vk::ClearValue> clearValues;
	clearValues.reserve(3);
	vk::ClearValue c {{0.0f, 0.0f, 0.0f, 1.f}};

	clearValues.push_back(c); // clearColor (value unused for msaa)
	if(samples() != vk::SampleCountBits::e1) { // msaa attachment
		clearValues.push_back({c});
	}

	if(depthFormat() != vk::Format::undefined && needsDepth()) {
		// clearValues.emplace_back(c).depthStencil = {1.f, 0u};
		clearValues.emplace_back(c).depthStencil = {0.f, 0u};
	}

	return clearValues;
}

argagg::parser SinglePassApp::argParser() const {
	auto parser = App::argParser();
	if(supportsMultisampling()) {
		parser.definitions.push_back({
			"multisamples", {"--multisamples", "-m"},
			"Sets the samples to use", 1
		});
	}
	return parser;
}

bool SinglePassApp::handleArgs(const argagg::parser_results& result, Args& out) {
	if(!App::handleArgs(result, out)) {
		return false;
	}

	samples_ = vk::SampleCountBits::e1;
	if(supportsMultisampling() && result.has_option("multisamples") > 0) {
		auto samples = result["multisamples"].as<unsigned>();

		switch(samples) {
			case 1u: samples_ = vk::SampleCountBits::e1; break;
			case 2u: samples_ = vk::SampleCountBits::e2; break;
			case 4u: samples_ = vk::SampleCountBits::e4; break;
			case 8u: samples_ = vk::SampleCountBits::e8; break;
			case 16u: samples_ = vk::SampleCountBits::e16; break;
			default:
				dlg_fatal("Invalid multisample parameter");
				return false;
		}
	}

	return true;
}

} // namespace tkn
