#include "light.hpp"
#include <tkn/bits.hpp>

#include <vpp/formats.hpp>
#include <vpp/vk.hpp>
#include <vpp/debug.hpp>

#include <dlg/dlg.hpp>
#include <nytl/tmpUtil.hpp>
#include <nytl/vecOps.hpp>
#include <array>

#include <shaders/smooth_shadow.light.frag.h>
#include <shaders/smooth_shadow.light.vert.h>

#include <shaders/smooth_shadow.shadow.frag.h>
#include <shaders/smooth_shadow.shadow.vert.h>

constexpr auto lightUboSize = sizeof(float) * (4 + 2 + 1 + 1 + 1);
constexpr auto shadowFormat = vk::Format::r8Unorm;

uint32_t nextPowerOfTwo(uint32_t n) {
	--n;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	return n + 1;
}

// TODO: should depend on some scaling setting in the light system
// Tweak this function for better/more performant shadows
unsigned shadowBufSize(float bounds) {
	// for a light with bounds radius of 1 we use this size
	constexpr auto normSize = 192;
	auto next = nextPowerOfTwo(normSize * bounds);
	return std::clamp(next, 32u, 2048u);
}

float lightCutoff(float r, float strength, float lightThresh) {
	return r * (std::sqrt(strength / lightThresh ) - 1);
}

float lightBounds(float radius, float strength) {
	return 5.f; // TODO

	// if changed here, also change in geometry.glsl
	constexpr auto lightThresh = 0.005;
	return radius + lightCutoff(radius, strength, lightThresh);
}

// Light
void Light::writeUBO(nytl::Span<std::byte>& data) {
	tkn::write(data, color);
	tkn::write(data, position);
	tkn::write(data, radius_);
	tkn::write(data, strength_);
	tkn::write(data, bounds_);
}

Light::Light(LightSystem& system, nytl::Vec2f pos,
		float radius, float strength,
		nytl::Vec4f color) : position(pos), color(color),
	system_(system), radius_(radius), strength_(strength) {

		bounds_ = lightBounds(radius, strength);
		bufSize_ = shadowBufSize(bounds_);
		init();
	}

void Light::createBuf() {
	auto& dev = system().device();
	constexpr auto usage = vk::ImageUsageBits::colorAttachment |
		vk::ImageUsageBits::sampled;

	auto targetInfo = vpp::ViewableImageCreateInfo(shadowFormat,
			vk::ImageAspectBits::color, {bufSize_, bufSize_}, usage);
	dlg_assert(vpp::supported(dev, targetInfo.img));
	shadowTarget_ = {dev.devMemAllocator(), targetInfo};

	// framebuffer
	auto imgView = shadowTarget_.vkImageView();;
	vk::FramebufferCreateInfo fbinfo;
	fbinfo.attachmentCount = 1;
	fbinfo.pAttachments = &imgView;
	fbinfo.width = bufSize_;
	fbinfo.height = bufSize_;
	fbinfo.layers = 1;
	fbinfo.renderPass = system().shadowPass();

	framebuffer_ = {dev, fbinfo};

	// update ds
	vpp::DescriptorSetUpdate update2(lightDs_);
	update2.uniform({{buffer_.buffer(), buffer_.offset(), lightUboSize}});
	update2.imageSampler({{{}, shadowTarget_.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
}

void Light::init() {
	auto& dev = system().device();

	auto& dsalloc = dev.descriptorAllocator();
	shadowDs_ = {dsalloc, system().shadowDsLayout()};
	lightDs_ = {dsalloc, system().lightDsLayout()};

	auto& bufalloc = dev.bufferAllocator();
	auto bufAlign = std::max<vk::DeviceSize>(4u,
			dev.properties().limits.minUniformBufferOffsetAlignment);
	constexpr auto bufUsage = vk::BufferUsageBits::uniformBuffer;
	auto types = dev.hostMemoryTypes();
	buffer_ = {bufalloc, lightUboSize, bufUsage, types, bufAlign};

	vpp::DescriptorSetUpdate update(shadowDs_);
	update.uniform({{buffer_.buffer(), buffer_.offset(), lightUboSize}});

	createBuf();
}

bool Light::updateDevice() {
	auto map = buffer_.memoryMap();
	auto span = map.span();
	writeUBO(span);

	auto rec = false;
	if(recreate_) {
		auto ns = shadowBufSize(bounds_);

		if(ns != bufSize_) {
			bufSize_ = ns;
			recreate_ = false;
			createBuf();
			rec = true;
		}
	}

	return rec;
}

void Light::radius(float radius, bool recreate) {
	radius_ = radius;
	bounds_ = lightBounds(radius, strength());
	recreate_ |= recreate;
}

void Light::strength(float strength, bool recreate) {
	strength_ = strength;
	bounds_ = lightBounds(radius(), strength);
	recreate_ |= recreate;
}

// LightSystem
LightSystem::LightSystem(vpp::Device& dev, vk::DescriptorSetLayout viewLayout)
	: dev_(dev), viewLayout_(viewLayout) {

		// we use this format for hdr tone mapping
		// guaranteed to be supported for our usage by the standard
		constexpr auto lightFormat = vk::Format::r16g16b16a16Sfloat;
		// constexpr auto lightFormat = vk::Format::r8g8b8a8Unorm;

		// renderpass setup
		// light pass
		vk::AttachmentDescription attachments[1] {};
		attachments[0].format = lightFormat;
		attachments[0].loadOp = vk::AttachmentLoadOp::clear;
		attachments[0].storeOp = vk::AttachmentStoreOp::store;
		attachments[0].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachments[0].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[0].initialLayout = vk::ImageLayout::undefined;
		attachments[0].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		attachments[0].samples = vk::SampleCountBits::e1;

		vk::AttachmentReference colorRef;
		colorRef.attachment = 0;
		colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

		vk::SubpassDescription subpasses[1] {};
		subpasses[0].colorAttachmentCount = 1;
		subpasses[0].pColorAttachments = &colorRef;
		subpasses[0].pipelineBindPoint = vk::PipelineBindPoint::graphics;

		vk::SubpassDependency dependency;
		dependency.srcSubpass = vk::subpassExternal;
		dependency.srcStageMask =
			vk::PipelineStageBits::host |
			vk::PipelineStageBits::transfer;
		dependency.srcAccessMask = vk::AccessBits::hostWrite |
			vk::AccessBits::transferWrite;
		dependency.dstSubpass = 0u;
		dependency.dstStageMask = vk::PipelineStageBits::allGraphics;
		dependency.dstAccessMask = vk::AccessBits::uniformRead |
			vk::AccessBits::vertexAttributeRead |
			vk::AccessBits::indirectCommandRead |
			vk::AccessBits::shaderRead;

		vk::RenderPassCreateInfo rpinfo;
		rpinfo.attachmentCount = 1;
		rpinfo.pAttachments = attachments;
		rpinfo.subpassCount = 1;
		rpinfo.pSubpasses = subpasses;
		rpinfo.dependencyCount = 1;
		rpinfo.pDependencies = &dependency;

		lightPass_ = {dev, rpinfo};

		// shadow pass
		attachments[0].format = shadowFormat;
		shadowPass_ = {dev, rpinfo};

		// framebuffer
		renderSize_ = {1920, 1080};
		// renderSize_ *= 0.5f;

		auto usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::inputAttachment |
			vk::ImageUsageBits::sampled;

		auto targetInfo = vpp::ViewableImageCreateInfo(lightFormat,
			vk::ImageAspectBits::color, {renderSize_[0], renderSize_[1]}, usage);
		dlg_assert(vpp::supported(dev, targetInfo.img));
		renderTarget_ = {dev.devMemAllocator(), targetInfo};

		auto imgView = renderTarget_.vkImageView();;
		vk::FramebufferCreateInfo fbinfo;
		fbinfo.attachmentCount = 1;
		fbinfo.pAttachments = &imgView;
		fbinfo.width = renderSize_[0];
		fbinfo.height = renderSize_[1];
		fbinfo.layers = 1;
		fbinfo.renderPass = lightPass_;

		framebuffer_ = {dev, fbinfo};

		// layouts
		vk::SamplerCreateInfo samplerInfo;
		samplerInfo.magFilter = vk::Filter::linear;
		samplerInfo.minFilter = vk::Filter::linear;
		samplerInfo.mipmapMode = vk::SamplerMipmapMode::nearest;
		// samplerInfo.addressModeU = vk::SamplerAddressMode::clampToEdge;
		// samplerInfo.addressModeV = vk::SamplerAddressMode::clampToEdge;
		// samplerInfo.addressModeW = vk::SamplerAddressMode::clampToEdge;
		samplerInfo.addressModeU = vk::SamplerAddressMode::clampToBorder;
		samplerInfo.addressModeV = vk::SamplerAddressMode::clampToBorder;
		samplerInfo.addressModeW = vk::SamplerAddressMode::clampToBorder;
		// samplerInfo.borderColor = vk::BorderColor::floatOpaqueWhite;
		samplerInfo.borderColor = vk::BorderColor::intOpaqueBlack;
		samplerInfo.mipLodBias = 0;
		samplerInfo.anisotropyEnable = false;
		samplerInfo.maxAnisotropy = 1.0;
		samplerInfo.compareEnable = false;
		samplerInfo.compareOp = {};
		samplerInfo.minLod = 0;
		samplerInfo.maxLod = 0.25;
		samplerInfo.unnormalizedCoordinates = false;
		sampler_ = {dev, samplerInfo};

		// ds
		auto shadowBindings = std::array{
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
					vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment)
		};

		auto lightBindings = std::array{
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
					vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
					vk::ShaderStageBits::fragment, &sampler_.vkHandle())
		};

		shadowDsLayout_.init(dev, shadowBindings);
		auto shadowSets = {viewLayout_, shadowDsLayout_.vkHandle()};
		vk::PipelineLayoutCreateInfo plInfo;
		plInfo.setLayoutCount = shadowSets.size();
		plInfo.pSetLayouts = shadowSets.begin();
		shadowPipeLayout_ = {dev, plInfo};

		lightDsLayout_.init(dev, lightBindings);
		auto lightSets = {viewLayout_, lightDsLayout_.vkHandle()};
		plInfo.setLayoutCount = lightSets.size();
		plInfo.pSetLayouts = lightSets.begin();
		lightPipeLayout_ = {dev, plInfo};

		// pipeline
		auto shadowVertex = vpp::ShaderModule(dev, smooth_shadow_shadow_vert_data);
		auto shadowFragment = vpp::ShaderModule(dev,
			smooth_shadow_shadow_frag_data);

		auto lightVertex = vpp::ShaderModule(dev, smooth_shadow_light_vert_data);
		auto lightFragment = vpp::ShaderModule(dev,
			smooth_shadow_light_frag_data);

		vpp::GraphicsPipelineInfo shadowInfo(shadowPass_, shadowPipeLayout_, {{{
				{shadowVertex, vk::ShaderStageBits::vertex},
				{shadowFragment, vk::ShaderStageBits::fragment}
		}}});

		vpp::GraphicsPipelineInfo lightInfo(lightPass_, lightPipeLayout_, {{{
			{lightVertex, vk::ShaderStageBits::vertex},
			{lightFragment, vk::ShaderStageBits::fragment}
		}}});

		// shadow
		constexpr auto stride = sizeof(float) * 5; // inPointA, inPointB, opacity
		auto bufferBinding = vk::VertexInputBindingDescription {
			0, stride, vk::VertexInputRate::instance
		};

		vk::VertexInputAttributeDescription attributes[3];
		attributes[0].format = vk::Format::r32g32Sfloat;

		attributes[1].format = vk::Format::r32g32Sfloat;
		attributes[1].offset = sizeof(float) * 2;
		attributes[1].location = 1;

		attributes[2].format = vk::Format::r32Sfloat;
		attributes[2].offset = sizeof(float) * 4;
		attributes[2].location = 2;

		shadowInfo.vertex.vertexBindingDescriptionCount = 1;
		shadowInfo.vertex.pVertexBindingDescriptions = &bufferBinding;
		shadowInfo.vertex.vertexAttributeDescriptionCount = 3;
		shadowInfo.vertex.pVertexAttributeDescriptions = attributes;
		shadowInfo.assembly.topology = vk::PrimitiveTopology::triangleStrip;

		vk::PipelineColorBlendAttachmentState shadowBlendAttachment;
		shadowBlendAttachment.blendEnable = true;
		shadowBlendAttachment.colorBlendOp = vk::BlendOp::add;
		shadowBlendAttachment.srcColorBlendFactor = vk::BlendFactor::one;
		shadowBlendAttachment.dstColorBlendFactor = vk::BlendFactor::one;
		shadowBlendAttachment.colorWriteMask =
			vk::ColorComponentBits::r |
			vk::ColorComponentBits::g |
			vk::ColorComponentBits::b |
			vk::ColorComponentBits::a;
		shadowInfo.blend.attachmentCount = 1;
		shadowInfo.blend.pAttachments = &shadowBlendAttachment;

		// light
		lightInfo.assembly.topology = vk::PrimitiveTopology::triangleFan;
		lightInfo.flags(vk::PipelineCreateBits::allowDerivatives);

		vk::PipelineColorBlendAttachmentState lightBlendAttachment;
		lightBlendAttachment.blendEnable = true;
		lightBlendAttachment.srcColorBlendFactor = vk::BlendFactor::srcAlpha;
		lightBlendAttachment.dstColorBlendFactor = vk::BlendFactor::one;
		lightBlendAttachment.colorWriteMask =
			vk::ColorComponentBits::r |
			vk::ColorComponentBits::g |
			vk::ColorComponentBits::b |
			vk::ColorComponentBits::a;
		lightInfo.blend.attachmentCount = 1;
		lightInfo.blend.pAttachments = &lightBlendAttachment;

		auto pipes = vk::createGraphicsPipelines(dev, {}, {{
				shadowInfo.info(),
				lightInfo.info()
			}}, nullptr);
		shadowPipe_ = {dev, pipes[0]};
		lightPipe_ = {dev, pipes[1]};

		// buffer
		auto startSize = sizeof(vk::DrawIndirectCommand);
		startSize += sizeof(ShadowSegment) * 128;
		auto hostTypes = dev.hostMemoryTypes();
		vertexBuffer_ = {dev.bufferAllocator(), startSize,
			vk::BufferUsageBits::vertexBuffer |
				vk::BufferUsageBits::indirectBuffer, hostTypes, 4};
	}

void LightSystem::addSegment(const ShadowSegment& seg) {
	segments_.push_back(seg);
}

bool LightSystem::updateDevice() {
	bool rerecord = false;
	for(auto& light : lights_) {
		// if(light.valid) {
		rerecord |= light.updateDevice();
		// }
	}

	auto neededSize = sizeof(vk::DrawIndirectCommand);
	neededSize += segments_.size() * sizeof(segments_[0]);
	if(vertexBuffer_.size() < neededSize) {
		auto memBits = device().hostMemoryTypes();
		vertexBuffer_ = {};
		vertexBuffer_ = {device().bufferAllocator(), neededSize * 2,
			vk::BufferUsageBits::vertexBuffer |
				vk::BufferUsageBits::indirectBuffer, memBits, 4};
		rerecord = true;
	}

	{
		auto map = vertexBuffer_.memoryMap();
		auto span = map.span();

		vk::DrawIndirectCommand cmd;
		cmd.firstInstance = 0;
		cmd.firstVertex = 0;
		cmd.instanceCount = segments_.size();
		cmd.vertexCount = 6;

		tkn::write(span, cmd);
		memcpy(span.data(), segments_.data(),
				segments_.size() * sizeof(segments_[0]));
	}

	return rerecord;
}

void LightSystem::renderLights(vk::CommandBuffer cmdBuf) {
	// render shadows into per-light shadow buffer
	renderShadowBuffers(cmdBuf);

	// render lights into one light buffer
	// vpp::DebugRegion rLights(device(), cmdBuf, "renderLights");

	static const vk::ClearValue clearValue = {{0.f, 0.f, 0.f, 0.0f}};
	auto width = renderSize_[0];
	auto height = renderSize_[1];
	vk::cmdBeginRenderPass(cmdBuf, {
		lightPass_,
		framebuffer_,
		{0u, 0u, width, height},
		1,
		&clearValue
	}, {});

	vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
	vk::cmdSetViewport(cmdBuf, 0, 1, vp);
	vk::cmdSetScissor(cmdBuf, 0, 1, {0, 0, width, height});

	vk::cmdBindVertexBuffers(cmdBuf, 0, 1, vertexBuffer_.buffer(),
		vertexBuffer_.offset() + sizeof(vk::DrawIndirectCommand));
	vk::cmdBindPipeline(cmdBuf, vk::PipelineBindPoint::graphics,
		lightPipe_);

	// render normal lights
	for(auto& light : lights_) {
		// if(!light.valid)  {
		// continue;
		// }

		// vpp::DebugRegion dr(device(), cmdBuf, "light", light.color);
		vk::cmdBindDescriptorSets(cmdBuf, vk::PipelineBindPoint::graphics,
			lightPipeLayout_, 1, {{light.lightDs()}}, {});
		vk::cmdDraw(cmdBuf, 4, 1, 0, 0);
	}

	vk::cmdEndRenderPass(cmdBuf);
}

void LightSystem::renderShadowBuffers(vk::CommandBuffer cmdBuf) {
	static const vk::ClearValue clearValue = {{0., 0., 0., 0.f}};

	// vpp::DebugRegion rShadow(device(), cmdBuf, "shadowBuffers");

	vk::cmdBindPipeline(cmdBuf, vk::PipelineBindPoint::graphics,
		shadowPipe_);
	vk::cmdBindVertexBuffers(cmdBuf, 0, 1, vertexBuffer_.buffer().vkHandle(),
		vertexBuffer_.offset() + sizeof(vk::DrawIndirectCommand));

	// render normal lights into their buffers
	// vpp::DebugRegion rNormal(device(), cmdBuf, "normal");

	for(auto& light : lights_) {
		// if(!light.valid) {
		// continue;
		// }

		// NOTE: might make sense to store this in light..
		auto bufSize = light.bufSize();
		vk::Viewport vp {
			0.f, 0.f,
			(float) bufSize, (float) bufSize,
			0.f, 1.f};

		vk::cmdBeginRenderPass(cmdBuf, {
				shadowPass_,
				light.framebuffer(),
				{0u, 0u, bufSize, bufSize},
				1,
				&clearValue
				}, {});

		vk::cmdSetViewport(cmdBuf, 0, 1, vp);
		vk::cmdSetScissor(cmdBuf, 0, 1, {0, 0, bufSize, bufSize});
		vk::cmdBindDescriptorSets(cmdBuf, vk::PipelineBindPoint::graphics,
			shadowPipeLayout_, 1, {{light.shadowDs()}}, {});

		vk::cmdDrawIndirect(cmdBuf, vertexBuffer_.buffer(),
			vertexBuffer_.offset(), 1, 0);
		vk::cmdEndRenderPass(cmdBuf);
	}
}

Light& LightSystem::addLight() {
	return lights_.emplace_back(*this, nytl::Vec2f {});
}

bool LightSystem::removeLight(Light& light) {
	// TODO
	dlg_warn("removeLight not implemented");
	nytl::unused(light);
	return false;
}
