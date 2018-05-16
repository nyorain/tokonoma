#include "light.hpp"

#include <vpp/formats.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/vk.hpp>
#include <vpp/debug.hpp>

#include <dlg/dlg.hpp>
#include <nytl/tmpUtil.hpp>
#include <nytl/vecOps.hpp>

#include <shaders/light.frag.h>
#include <shaders/light.vert.h>

#include <shaders/shadow.frag.h>
#include <shaders/shadow.vert.h>

constexpr auto lightUboSize = sizeof(float) * (4 + 2 + 1 + 1);
constexpr auto shadowBufSize = 512;

template<typename T>
void write(nytl::Span<std::byte>& span, T&& data) {
	dlg_assert(span.size() >= sizeof(data));
	std::memcpy(span.data(), &data, sizeof(data));
	span = span.slice(sizeof(data), span.size() - sizeof(data));
}

void Light::writeUBO(nytl::Span<std::byte>& data) {
	write(data, color);
	write(data, position);
	write(data, radius);
	write(data, strength);
}

Light::Light(LightSystem& system) {
	init(system);
}

void Light::init(LightSystem& system) {
	auto& dev = system.device();

	// target
	constexpr auto usage = vk::ImageUsageBits::colorAttachment |
		vk::ImageUsageBits::sampled;

	auto targetInfo = vpp::ViewableImageCreateInfo::general(dev,
		{shadowBufSize, shadowBufSize, 1}, usage, {vk::Format::r8Unorm},
		vk::ImageAspectBits::color).value();
	shadowTarget_ = {dev, targetInfo};

	// framebuffer
	auto imgView = shadowTarget_.vkImageView();;
	vk::FramebufferCreateInfo fbinfo;
	fbinfo.attachmentCount = 1;
	fbinfo.pAttachments = &imgView;
	fbinfo.width = shadowBufSize;
	fbinfo.height = shadowBufSize;
	fbinfo.layers = 1;
	fbinfo.renderPass = system.shadowPass();

	framebuffer_ = {dev, fbinfo};

	// ds
	auto& dsalloc = dev.descriptorAllocator();
	shadowDs_ = {dsalloc, system.shadowDsLayout()};
	lightDs_ = {dsalloc, system.lightDsLayout()};

	auto& bufalloc = dev.bufferAllocator();
	auto bufAlign = std::max<vk::DeviceSize>(4u,
		dev.properties().limits.minUniformBufferOffsetAlignment);
	constexpr auto bufUsage = vk::BufferUsageBits::uniformBuffer;
	auto types = dev.hostMemoryTypes();
	auto size = lightUboSize;
	buffer_ = {bufalloc, size, bufUsage, bufAlign, types};

	vpp::DescriptorSetUpdate update(shadowDs_);
	update.uniform({{buffer_.buffer(), buffer_.offset(), size}});

	vpp::DescriptorSetUpdate update2(lightDs_);
	update2.uniform({{buffer_.buffer(), buffer_.offset(), size}});
	update2.imageSampler({{{}, shadowTarget_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
}

bool Light::updateDevice() {
	auto map = buffer_.memoryMap();
	auto span = map.span();
	writeUBO(span);
	return false;
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

	vk::RenderPassCreateInfo rpinfo;
	rpinfo.attachmentCount = 1;
	rpinfo.pAttachments = attachments;
	rpinfo.subpassCount = 1;
	rpinfo.pSubpasses = subpasses;

	lightPass_ = {dev, rpinfo};

	// shadow pass
	attachments[0].format = vk::Format::r8Unorm;
	shadowPass_ = {dev, rpinfo};

	// framebuffer
	renderSize_ = {1920, 1080};
	// renderSize_ *= 0.5f;

	auto usage = vk::ImageUsageBits::colorAttachment |
		vk::ImageUsageBits::inputAttachment |
		vk::ImageUsageBits::sampled;

	auto targetInfo = vpp::ViewableImageCreateInfo::color(dev,
		{renderSize_[0], renderSize_[1], 1}, usage, {lightFormat}).value();
	renderTarget_ = {dev, targetInfo};

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
	samplerInfo.addressModeU = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.addressModeV = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.addressModeW = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.mipLodBias = 0;
	samplerInfo.anisotropyEnable = false;
	samplerInfo.maxAnisotropy = 1.0;
	samplerInfo.compareEnable = false;
	samplerInfo.compareOp = {};
	samplerInfo.minLod = 0;
	samplerInfo.maxLod = 0.25;
	samplerInfo.borderColor = vk::BorderColor::intTransparentBlack;
	samplerInfo.unnormalizedCoordinates = false;
	sampler_ = {dev, samplerInfo};

	// ds
	auto shadowBindings = {
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment)
	};

	auto lightBindings = {
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle())
	};

	shadowDsLayout_ = {dev, shadowBindings};
	auto shadowSets = {viewLayout_, shadowDsLayout_.vkHandle()};
	vk::PipelineLayoutCreateInfo plInfo;
	plInfo.setLayoutCount = shadowSets.size();
	plInfo.pSetLayouts = shadowSets.begin();
	shadowPipeLayout_ = {dev, plInfo};

	lightDsLayout_ = {dev, lightBindings};
	auto lightSets = {viewLayout_, lightDsLayout_.vkHandle()};
	plInfo.setLayoutCount = lightSets.size();
	plInfo.pSetLayouts = lightSets.begin();
	lightPipeLayout_ = {dev, plInfo};

	// pipeline
	auto shadowVertex = vpp::ShaderModule(dev, shadow_vert_data);
	auto shadowFragment = vpp::ShaderModule(dev, shadow_frag_data);

	auto lightVertex = vpp::ShaderModule(dev, light_vert_data);
	auto lightFragment = vpp::ShaderModule(dev, light_frag_data);

	vpp::GraphicsPipelineInfo shadowInfo(shadowPass_,
		shadowPipeLayout_, vpp::ShaderProgram({
			{shadowVertex, vk::ShaderStageBits::vertex},
			{shadowFragment, vk::ShaderStageBits::fragment}
	}));

	vpp::GraphicsPipelineInfo lightInfo(lightPass_,
		lightPipeLayout_, vpp::ShaderProgram({
			{lightVertex, vk::ShaderStageBits::vertex},
			{lightFragment, vk::ShaderStageBits::fragment}
	}));

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
	shadowBlendAttachment.colorWriteMask = vk::ColorComponentBits::r;
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
		vk::ColorComponentBits::b;
	lightInfo.blend.attachmentCount = 1;
	lightInfo.blend.pAttachments = &lightBlendAttachment;

	auto pipes = vk::createGraphicsPipelines(dev, {}, {
			shadowInfo.info(),
			lightInfo.info()},
		nullptr);
	shadowPipe_ = {dev, pipes[0]};
	lightPipe_ = {dev, pipes[1]};

	// buffer
	auto startSize = sizeof(vk::DrawIndirectCommand);
	startSize += sizeof(ShadowSegment) * 128;
	auto hostTypes = dev.hostMemoryTypes();
	vertexBuffer_ = {dev.bufferAllocator(), startSize,
		vk::BufferUsageBits::vertexBuffer |
		vk::BufferUsageBits::indirectBuffer, 4, hostTypes};
}

void LightSystem::addSegment(const ShadowSegment& seg) {
	segments_.push_back(seg);
}

void LightSystem::update(double) {
}

bool LightSystem::updateDevice() {
	bool rerecord = false;
	for(auto& light : lights_) {
		if(light.valid) {
			rerecord |= light.updateDevice();
		}
	}

	auto neededSize = sizeof(vk::DrawIndirectCommand);
	neededSize += segments_.size() * sizeof(segments_[0]);
	if(vertexBuffer_.size() < neededSize) {
		auto memBits = device().hostMemoryTypes();
		vertexBuffer_ = {};
		vertexBuffer_ = {device().bufferAllocator(), neededSize * 2,
			vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::indirectBuffer, 4, memBits};
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

		write(span, cmd);
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

	vk::cmdBindVertexBuffers(cmdBuf, 0, {vertexBuffer_.buffer()},
		{vertexBuffer_.offset() + sizeof(vk::DrawIndirectCommand)});
	vk::cmdBindPipeline(cmdBuf, vk::PipelineBindPoint::graphics,
		lightPipe_);

	// render normal lights
	for(auto& light : lights_) {
		if(!light.valid)  {
			continue;
		}

		// vpp::DebugRegion dr(device(), cmdBuf, "light", light.color);
		vk::cmdBindDescriptorSets(cmdBuf, vk::PipelineBindPoint::graphics,
			lightPipeLayout_, 1, {light.lightDs()}, {});
		vk::cmdDraw(cmdBuf, 4, 1, 0, 0);
	}

	vk::cmdEndRenderPass(cmdBuf);
}

void LightSystem::renderShadowBuffers(vk::CommandBuffer cmdBuf) {
	static const vk::ClearValue clearValue = {{0., 0., 0., 0.f}};

	// vpp::DebugRegion rShadow(device(), cmdBuf, "shadowBuffers");

	vk::cmdBindPipeline(cmdBuf, vk::PipelineBindPoint::graphics,
		shadowPipe_);
	vk::cmdBindVertexBuffers(cmdBuf, 0, {vertexBuffer_.buffer().vkHandle()},
		{vertexBuffer_.offset() + sizeof(vk::DrawIndirectCommand)});
	vk::Viewport vp {
		0.f, 0.f,
		(float) shadowBufSize, (float) shadowBufSize,
		0.f, 1.f};

	// render normal lights into their buffers
	// vpp::DebugRegion rNormal(device(), cmdBuf, "normal");

	for(auto& light : lights_) {
		if(!light.valid) {
			continue;
		}

		vk::cmdBeginRenderPass(cmdBuf, {
			shadowPass_,
			light.framebuffer(),
			{0u, 0u, shadowBufSize, shadowBufSize},
			1,
			&clearValue
		}, {});

		vk::cmdSetViewport(cmdBuf, 0, 1, vp);
		vk::cmdSetScissor(cmdBuf, 0, 1, {0, 0, shadowBufSize, shadowBufSize});
		vk::cmdBindDescriptorSets(cmdBuf, vk::PipelineBindPoint::graphics,
			shadowPipeLayout_, 1, {light.shadowDs()}, {});

		vk::cmdDrawIndirect(cmdBuf, vertexBuffer_.buffer(),
			vertexBuffer_.offset(), 1, 0);
		vk::cmdEndRenderPass(cmdBuf);
	}
}

Light& LightSystem::addLight() {
	return lights_.emplace_back(*this);
}

bool LightSystem::removeLight(Light& light) {
	// TODO
	nytl::unused(light);
	return false;
}
