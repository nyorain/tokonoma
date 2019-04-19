#include "light.hpp"
#include <stage/scene/primitive.hpp>
#include <stage/bits.hpp>
#include <stage/transform.hpp>
#include <vpp/vk.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/formats.hpp>

#include <shaders/shadowmap.vert.h>
#include <shaders/shadowmap.frag.h>

ShadowData initShadowData(const vpp::Device& dev, vk::PipelineLayout pl,
		vk::Format depthFormat) {
	ShadowData data;
	data.depthFormat = depthFormat;

	// renderpass
	vk::AttachmentDescription depth {};
	depth.initialLayout = vk::ImageLayout::undefined;
	depth.finalLayout = vk::ImageLayout::depthStencilReadOnlyOptimal;
	depth.format = depthFormat;
	depth.loadOp = vk::AttachmentLoadOp::clear;
	depth.storeOp = vk::AttachmentStoreOp::store;
	depth.samples = vk::SampleCountBits::e1;

	vk::AttachmentReference depthRef {};
	depthRef.attachment = 0;
	depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

	vk::SubpassDescription subpass {};
	subpass.pDepthStencilAttachment = &depthRef;

	vk::RenderPassCreateInfo rpi {};
	rpi.attachmentCount = 1;
	rpi.pAttachments = &depth;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &subpass;

	data.rp = {dev, rpi};

	// sampler
	vk::SamplerCreateInfo sci {};
	sci.addressModeU = vk::SamplerAddressMode::clampToBorder;
	sci.addressModeV = vk::SamplerAddressMode::clampToBorder;
	sci.addressModeW = vk::SamplerAddressMode::clampToBorder;
	sci.borderColor = vk::BorderColor::floatOpaqueWhite;
	sci.mipmapMode = vk::SamplerMipmapMode::nearest;
	sci.compareEnable = true;
	sci.compareOp = vk::CompareOp::lessOrEqual;
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	sci.minLod = 0.0;
	sci.maxLod = 0.25;
	data.sampler = {dev, sci};

	// pipeline
	vpp::ShaderModule vertShader(dev, shadowmap_vert_data);
	vpp::ShaderModule fragShader(dev, shadowmap_frag_data);

	vpp::GraphicsPipelineInfo gpi {data.rp, pl, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}, 0, vk::SampleCountBits::e1};

	constexpr auto stride = sizeof(doi::Primitive::Vertex);
	vk::VertexInputBindingDescription bufferBindings[2] = {
		{0, stride, vk::VertexInputRate::vertex},
		{1, sizeof(float) * 2, vk::VertexInputRate::vertex} // uv
	};

	vk::VertexInputAttributeDescription attributes[2];
	attributes[0].format = vk::Format::r32g32b32Sfloat; // pos

	attributes[1].format = vk::Format::r32g32Sfloat; // uv
	attributes[1].location = 1;
	attributes[1].binding = 1;

	gpi.vertex.pVertexAttributeDescriptions = attributes;
	gpi.vertex.vertexAttributeDescriptionCount = 2u;
	gpi.vertex.pVertexBindingDescriptions = bufferBindings;
	gpi.vertex.vertexBindingDescriptionCount = 2u;

	gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

	gpi.depthStencil.depthTestEnable = true;
	gpi.depthStencil.depthWriteEnable = true;
	gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

	// NOTE: we could use front face culling instead of a depth bias
	// but that only works for solid objects, won't work for planes
	// Pipeline-stage culling at all probably brings problems for
	// doubleSided primitives/materials
	// gpi.rasterization.cullMode = vk::CullModeBits::front;
	// gpi.rasterization.cullMode = vk::CullModeBits::back;
	gpi.rasterization.cullMode = vk::CullModeBits::none;
	gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
	gpi.rasterization.depthBiasEnable = true;

	auto dynamicStates = {
		vk::DynamicState::depthBias,
		vk::DynamicState::viewport,
		vk::DynamicState::scissor
	};
	gpi.dynamic.pDynamicStates = dynamicStates.begin();
	gpi.dynamic.dynamicStateCount = dynamicStates.end() - dynamicStates.begin();

	gpi.blend.attachmentCount = 0;

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(dev, {},
		1, gpi.info(), NULL, vkpipe);

	data.pipe = {dev, vkpipe};
	return data;
}

DirLight::DirLight(const vpp::Device& dev, const vpp::TrDsLayout& dsLayout,
		const ShadowData& data) {
	auto extent = vk::Extent3D{size_.x, size_.y, 1u};

	// target
	auto targetUsage = vk::ImageUsageBits::depthStencilAttachment |
		vk::ImageUsageBits::sampled;
	auto targetInfo = vpp::ViewableImageCreateInfo::general(dev,
		extent, targetUsage, {data.depthFormat},
		vk::ImageAspectBits::depth);
	target_ = {dev, *targetInfo};

	// framebuffer
	vk::FramebufferCreateInfo fbi {};
	fbi.attachmentCount = 1;
	fbi.width = extent.width;
	fbi.height = extent.height;
	fbi.layers = 1u;
	fbi.pAttachments = &target_.vkImageView();
	fbi.renderPass = data.rp;
	fb_ = {dev, fbi};

	// setup light ds and ubo
	auto hostMem = dev.hostMemoryTypes();
	auto lightUboSize = sizeof(nytl::Mat4f) + // projection, view
		sizeof(this->data);
	ds_ = {dev.descriptorAllocator(), dsLayout};
	ubo_ = {dev.bufferAllocator(), lightUboSize,
		vk::BufferUsageBits::uniformBuffer, 0, hostMem};
	updateDevice();

	vpp::DescriptorSetUpdate ldsu(ds_);
	ldsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
	ldsu.imageSampler({{data.sampler, shadowMap(),
		vk::ImageLayout::depthStencilReadOnlyOptimal}});
	vpp::apply({ldsu});
}

void DirLight::render(vk::CommandBuffer cb, vk::PipelineLayout pl,
		const ShadowData& data, const doi::Scene& scene) {
	vk::ClearValue clearValue {};
	clearValue.depthStencil = {1.f, 0u};

	// draw shadow map!
	vk::cmdBeginRenderPass(cb, {
		data.rp, fb_,
		{0u, 0u, size_.x, size_.y},
		1, &clearValue
	}, {});

	vk::Viewport vp {0.f, 0.f, (float) size_.x, (float) size_.y, 0.f, 1.f};
	vk::cmdSetViewport(cb, 0, 1, vp);
	vk::cmdSetScissor(cb, 0, 1, {0, 0, size_.x, size_.y});

	// TODO: fine tune these values!
	// maybe they should be scene dependent?
	vk::cmdSetDepthBias(cb, 0.25, 0.f, 4.0);

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, data.pipe);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pl, 3, {ds_}, {});

	scene.render(cb, pl);
	vk::cmdEndRenderPass(cb);
}

nytl::Mat4f DirLight::lightMatrix() const {
	// TODO: not correct, just for testing.
	// TODO: sizes should be configurable; depend on scene size
	auto mat = doi::ortho3Sym(8.f, 8.f, 0.5f, 100.f);
	// auto mat = doi::perspective3RH<float>
	// 	(0.25 * nytl::constants::pi, 1.f, 0.01f, 30.f);
	mat = mat * doi::lookAtRH(this->data.pd,
		{0.f, 0.f, 0.f}, // always looks at center
		{0.f, 1.f, 0.f});
	return mat;
}

void DirLight::updateDevice() {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	doi::write(span, lightMatrix());
	doi::write(span, this->data);
}
