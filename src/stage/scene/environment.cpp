#include <stage/scene/environment.hpp>
#include <stage/texture.hpp>
#include <stage/bits.hpp>

#include <vpp/bufferOps.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/submit.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/debug.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/handles.hpp>
#include <vpp/formats.hpp>
#include <vpp/vk.hpp>

#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>

#include <shaders/stage.fullscreen.vert.h>
#include <shaders/stage.skybox.vert.h>
#include <shaders/stage.skybox.frag.h>

namespace doi {

// Environment
void Environment::create(InitData& data, const WorkBatcher& wb,
		nytl::StringParam envMapPath, nytl::StringParam irradiancePath,
		vk::Sampler linear) {
	auto& dev = wb.dev;

	// textures
	doi::TextureCreateParams params;
	params.cubemap = true;
	params.format = vk::Format::r16g16b16a16Sfloat;
	auto envProvider = doi::read(envMapPath, true);
	convolutionMipmaps_ = envProvider->mipLevels();
	envMap_ = {data.initEnvMap, wb, std::move(envProvider), params};
	irradiance_ = {data.initIrradiance, wb, doi::read(irradiancePath, true),
		params};

	// pipe
	// ds layout
	auto bindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex),
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &linear),
	};

	dsLayout_ = {dev, bindings};
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};

	// ubo
	auto uboSize = sizeof(nytl::Mat4f);
	ubo_ = {data.initUbo, dev.bufferAllocator(), uboSize,
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

	ds_ = {data.initDs, dev.descriptorAllocator(), dsLayout_};
}

void Environment::init(InitData& data, const WorkBatcher& wb) {
	envMap_.init(data.initEnvMap, wb);
	irradiance_.init(data.initIrradiance, wb);
	ubo_.init(data.initUbo);
	ds_.init(data.initDs);

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
	dsu.imageSampler({{{}, envMap_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
}

void Environment::createPipe(const vpp::Device& dev, vk::RenderPass rp,
		unsigned subpass, vk::SampleCountBits samples) {
	vpp::ShaderModule vertShader(dev, stage_skybox_vert_data);
	vpp::ShaderModule fragShader(dev, stage_skybox_frag_data);
	vpp::GraphicsPipelineInfo gpi {rp, pipeLayout_, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}, subpass, samples};

	// TODO: some of these rather temporary workarounds for the
	// deferred renderer
	gpi.depthStencil.depthTestEnable = true;
	gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
	// gpi.rasterization.cullMode = vk::CullModeBits::back;
	// gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

	vk::PipelineColorBlendAttachmentState blendAttachment;
	blendAttachment.blendEnable = true;
	blendAttachment.colorBlendOp = vk::BlendOp::add;
	blendAttachment.srcColorBlendFactor = vk::BlendFactor::one;
	blendAttachment.dstColorBlendFactor = vk::BlendFactor::one;
	blendAttachment.colorWriteMask =
			vk::ColorComponentBits::r |
			vk::ColorComponentBits::g |
			vk::ColorComponentBits::b |
			vk::ColorComponentBits::a;
	gpi.blend.pAttachments = &blendAttachment;

	pipe_ = {dev, gpi.info()};
}

void Environment::render(vk::CommandBuffer cb) const {
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pipeLayout_, 0, {{ds_.vkHandle()}}, {});
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
	vk::cmdDrawIndexed(cb, 36, 1, 0, 0, 0);
}

void Environment::updateDevice(const nytl::Mat4f& viewProj) {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	doi::write(span, viewProj);
}

} // namespace doi
