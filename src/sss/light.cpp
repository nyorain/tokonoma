#include "light.hpp"
#include <stage/bits.hpp>

#include <vpp/formats.hpp>
#include <vpp/vk.hpp>
#include <vpp/debug.hpp>

#include <dlg/dlg.hpp>
#include <nytl/tmpUtil.hpp>
#include <nytl/vecOps.hpp>

#include <shaders/smooth_shadow.light.frag.h>
#include <shaders/smooth_shadow.light.vert.h>

#include <shaders/sss.sss_shadow.frag.h>
#include <shaders/sss.sss_shadow.vert.h>

constexpr auto lightUboSize = sizeof(float) * (4 + 2 + 1 + 1 + 1);
constexpr auto shadowFormat = vk::Format::r16g16Sfloat;

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
	constexpr auto normSize = 286;
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

nytl::Vec3f blackbody(unsigned temp) {
	// http://www.zombieprototypes.com/?p=210
	// this version based upon https://github.com/neilbartlett/color-temperature,
	// licensed under MIT.
	float t = temp / 100.f;
	float r, g, b;

	struct Coeffs {
		float a, b, c, off;
		float compute(float x) {
			auto r = a + b * x + c * std::log(x + off);
			return std::clamp(r, 0.f, 255.f);
		}
	};

	if(t < 66.0) {
		r = 255;
	} else {
		r = Coeffs {
			351.97690566805693,
			0.114206453784165,
			-40.25366309332127,
			-55
		}.compute(t);
	}

	if(t < 66.0) {
		g = Coeffs {
			-155.25485562709179,
			-0.44596950469579133,
			104.49216199393888,
			-2
		}.compute(t);
	} else {
		g = Coeffs {
			325.4494125711974,
			0.07943456536662342,
			-28.0852963507957,
			-50
		}.compute(t);
	}

	if(t >= 66.0) {
		b = 255;
	} else {

		if(t <= 20.0) {
			b = 0;
		} else {
			b = Coeffs {
				-254.76935184120902,
				0.8274096064007395,
				115.67994401066147,
				-10
			}.compute(t);

		}
	}

	return {r / 255.f, g / 255.f, b / 255.f};
}

// Light
void Light::writeUBO(nytl::Span<std::byte>& data) {
	doi::write(data, color);
	doi::write(data, position);
	doi::write(data, radius_);
	doi::write(data, strength_);
	doi::write(data, bounds_);
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

	auto targetInfo = vpp::ViewableImageCreateInfo::general(dev,
			{bufSize_, bufSize_, 1}, usage, {shadowFormat},
			vk::ImageAspectBits::color).value();
	shadowTarget_ = {dev, targetInfo};

	// framebuffer
	auto imgView = shadowTarget_.vkImageView();
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
	buffer_ = {bufalloc, lightUboSize, bufUsage, bufAlign, types};

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
		auto shadowVertex = vpp::ShaderModule(dev, sss_sss_shadow_vert_data);
		auto shadowFragment = vpp::ShaderModule(dev, sss_sss_shadow_frag_data);

		auto lightVertex = vpp::ShaderModule(dev, smooth_shadow_light_vert_data);
		auto lightFragment = vpp::ShaderModule(dev, smooth_shadow_light_frag_data);

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
		cmd.vertexCount = 4;

		doi::write(span, cmd);
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
		// if(!light.valid)  {
		// continue;
		// }

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
				shadowPipeLayout_, 1, {light.shadowDs()}, {});

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
	nytl::unused(light);
	return false;
}
