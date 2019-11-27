#include <tkn/scene/environment.hpp>
#include <tkn/texture.hpp>
#include <tkn/bits.hpp>
#include <tkn/render.hpp>

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

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/tkn.skybox.vert.h>
#include <shaders/tkn.skybox.frag.h>

namespace tkn {

// Environment
void Environment::create(InitData& data, const WorkBatcher& wb,
		nytl::StringParam envMapPath, nytl::StringParam irradiancePath,
		vk::Sampler linear) {
	auto& dev = wb.dev;

	// textures
	tkn::TextureCreateParams params;
	params.cubemap = true;
	params.format = vk::Format::r16g16b16a16Sfloat;
	auto envProvider = tkn::read(envMapPath, true);
	convolutionMipmaps_ = envProvider->mipLevels();
	envMap_ = {data.initEnvMap, wb, std::move(envProvider), params};
	irradiance_ = {data.initIrradiance, wb, tkn::read(irradiancePath, true),
		params};

	// pipe
	// ds layout
	auto bindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &linear),
	};

	dsLayout_ = {dev, bindings};

	// ubo
	ds_ = {data.initDs, dev.descriptorAllocator(), dsLayout_};
}

void Environment::init(InitData& data, const WorkBatcher& wb) {
	envMap_.init(data.initEnvMap, wb);
	irradiance_.init(data.initIrradiance, wb);
	ds_.init(data.initDs);

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.imageSampler({{{}, envMap_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
}

void Environment::createPipe(const vpp::Device& dev,
		vk::DescriptorSetLayout camDsLayout, vk::RenderPass rp,
		unsigned subpass, vk::SampleCountBits samples,
		nytl::Span<const vk::PipelineColorBlendAttachmentState> battachments) {
	pipeLayout_ = {dev, {{camDsLayout, dsLayout_.vkHandle()}}, {}};

	vpp::ShaderModule vertShader(dev, tkn_skybox_vert_data);
	vpp::ShaderModule fragShader(dev, tkn_skybox_frag_data);
	vpp::GraphicsPipelineInfo gpi {rp, pipeLayout_, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}, subpass, samples};

	// enable depth testing to only write where it's really needed
	// (where no geometry was rendered yet)
	gpi.depthStencil.depthTestEnable = true;
	gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
	// culling not really needed here
	gpi.rasterization.cullMode = vk::CullModeBits::back;
	gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

	gpi.blend.attachmentCount = battachments.size();
	gpi.blend.pAttachments = battachments.begin();

	pipe_ = {dev, gpi.info()};
}

void Environment::render(vk::CommandBuffer cb) const {
	tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 1, {ds_});
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
	vk::cmdDrawIndexed(cb, 36, 1, 0, 0, 0);
}

} // namespace tkn
