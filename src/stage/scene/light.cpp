#include <stage/scene/light.hpp>
#include <stage/scene/primitive.hpp>
#include <stage/scene/scene.hpp>
#include <stage/scene/shape.hpp>
#include <stage/bits.hpp>
#include <stage/transform.hpp>
#include <vpp/vk.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/formats.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/device.hpp>
#include <vpp/queue.hpp>

#include <shaders/stage.shadowmap.vert.h>
#include <shaders/stage.shadowmap.frag.h>
#include <shaders/stage.shadowmapCube.vert.h>
#include <shaders/stage.shadowmapCube.frag.h>

// TODO: some duplication between dir and point light

namespace doi {

// needs to be the same as in shadowmapCube.vert
constexpr auto pcrOffsetFaceID = 44u;

ShadowData initShadowData(const vpp::Device& dev, vk::Format depthFormat,
		vk::DescriptorSetLayout lightDsLayout,
		vk::DescriptorSetLayout materialDsLayout,
		vk::DescriptorSetLayout primitiveDsLayout,
		vk::PushConstantRange materialPcr) {
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

	// TODO: this barrier is too strict, we don't really need that!
	// (mainly talking about deferred renderer but probably true for all)
	// we don't need one shadow rendering to finish before another starts,
	// they can run in parallel. Rather issue one (big) barrier before
	// starting with the light render pass.
	// We could also add an external -> subpass 0 dependency to the light
	// render pass but we may issue additional commands before that
	// render pass that we don't need to finish there i guess.
	vk::SubpassDependency dependency;
	dependency.srcSubpass = 0u; // last
	dependency.srcStageMask = vk::PipelineStageBits::allGraphics;
	dependency.srcAccessMask = vk::AccessBits::depthStencilAttachmentWrite;
	dependency.dstSubpass = vk::subpassExternal;
	dependency.dstStageMask = vk::PipelineStageBits::allGraphics;
	dependency.dstAccessMask = vk::AccessBits::memoryRead;

	vk::RenderPassCreateInfo rpi {};
	rpi.attachmentCount = 1;
	rpi.pAttachments = &depth;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &subpass;
	rpi.pDependencies = &dependency;
	rpi.dependencyCount = 1u;

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

	// pipeline layout
	dlg_assert(materialPcr.size == pcrOffsetFaceID);
	vk::PushConstantRange cubemapPcr;
	cubemapPcr.offset = pcrOffsetFaceID;
	cubemapPcr.stageFlags = vk::ShaderStageBits::vertex;
	cubemapPcr.size = 4u;

	data.pl = {dev,
		{lightDsLayout, materialDsLayout, primitiveDsLayout},
		{materialPcr, cubemapPcr}};

	// pipeline
	vpp::ShaderModule vertShader(dev, stage_shadowmap_vert_data);
	vpp::ShaderModule fragShader(dev, stage_shadowmap_frag_data);
	vpp::GraphicsPipelineInfo gpi {data.rp, data.pl, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}, 0, vk::SampleCountBits::e1};

	// see cubemap pipeline below
	gpi.flags(vk::PipelineCreateBits::allowDerivatives);

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

	// cubemap pipe
	// TODO: we could alternatively always use a uniform buffer with
	// dynamic offset for the light view projection... but then we
	// might have to pad that buffer (since vulkan has an alignment
	// requirement there) which adds complexity as well
	vpp::ShaderModule cubeVertShader(dev, stage_shadowmapCube_vert_data);
	vpp::ShaderModule cubeFragShader(dev, stage_shadowmapCube_frag_data);
	vpp::GraphicsPipelineInfo cgpi {data.rp, data.pl, {{
		{cubeVertShader, vk::ShaderStageBits::vertex},
		{cubeFragShader, vk::ShaderStageBits::fragment},
	}}, 0, vk::SampleCountBits::e1};

	cgpi.dynamic = gpi.dynamic;
	cgpi.blend = gpi.blend;
	cgpi.rasterization = gpi.rasterization;
	cgpi.depthStencil = gpi.depthStencil;
	cgpi.assembly = gpi.assembly;
	cgpi.vertex = gpi.vertex;
	cgpi.base(data.pipe); // basically the same, except vertex shader

	vk::createGraphicsPipelines(dev, {},
		1, cgpi.info(), NULL, vkpipe);
	data.pipeCube = {dev, vkpipe};

	return data;
}

// DirLight
DirLight::DirLight(const vpp::Device& dev, const vpp::TrDsLayout& dsLayout,
		const vpp::TrDsLayout& primitiveDsLayout, const ShadowData& data,
		nytl::Vec3f viewPos, const Material& mat) {
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
	auto lightUboSize = sizeof(this->data) +
		sizeof(nytl::Mat4f); // projection, view
	ds_ = {dev.descriptorAllocator(), dsLayout};
	ubo_ = {dev.bufferAllocator(), lightUboSize,
		vk::BufferUsageBits::uniformBuffer, 0, hostMem};

	vpp::DescriptorSetUpdate ldsu(ds_);
	ldsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
	ldsu.imageSampler({{data.sampler, shadowMap(),
		vk::ImageLayout::depthStencilReadOnlyOptimal}});
	vpp::apply({ldsu});

	// light ball
	auto cube = doi::Cube{{}, {1.f, 1.f, 1.f}};
	auto shape = doi::generate(cube);
	lightBall_ = {dev, shape, primitiveDsLayout,
		mat, lightBallMatrix(viewPos), 0xFF}; // TODO: id

	updateDevice(viewPos);
}

void DirLight::render(vk::CommandBuffer cb, const ShadowData& data,
		const doi::Scene& scene) {
	vk::ClearValue clearValue {};
	clearValue.depthStencil = {1.f, 0u};

	// render into shadow map
	vk::cmdBeginRenderPass(cb, {
		data.rp, fb_,
		{0u, 0u, size_.x, size_.y},
		1, &clearValue
	}, {});

	vk::Viewport vp {0.f, 0.f, (float) size_.x, (float) size_.y, 0.f, 1.f};
	vk::cmdSetViewport(cb, 0, 1, vp);
	vk::cmdSetScissor(cb, 0, 1, {0, 0, size_.x, size_.y});

	// TODO: fine tune these values!
	// maybe they should be scene dependent? dependent on size/scale?
	vk::cmdSetDepthBias(cb, 1.0, 0.f, 4.0);

	auto pl = data.pl.vkHandle();
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, data.pipe);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pl, 0, {ds_}, {});

	scene.render(cb, pl);
	vk::cmdEndRenderPass(cb);
}

nytl::Mat4f DirLight::lightBallMatrix(nytl::Vec3f viewPos) const {
	return translateMat(viewPos - 25.f * nytl::normalized(this->data.dir));
}

nytl::Mat4f DirLight::lightMatrix(nytl::Vec3f viewPos) const {
	// TODO: sizes should be configurable; depend on scene size
	// TODO: can be opimtized, better position
	auto pos = viewPos - 10 * this->data.dir;
	// auto pos = -this->data.dir;

	// discretize
	// using namespace nytl::vec::cw;
	// using namespace nytl::vec::operators;
	auto frustSize = 12.f; // TODO
	// auto step = 2048 * frustSize / size_.x;
	// auto step = 4.f;
	// pos = ceil(pos / step) * step;

	auto mat = doi::ortho3Sym(frustSize, frustSize, 0.5f, 30.f);
	// mat = mat * doi::lookAtRH(pos, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
	mat = mat * doi::lookAtRH(pos, viewPos, {0.f, 1.f, 0.f});
	return mat;
}

void DirLight::updateDevice(nytl::Vec3f viewPos) {
	nytl::normalize(this->data.dir);

	auto map = ubo_.memoryMap();
	auto span = map.span();
	doi::write(span, this->data);
	doi::write(span, lightMatrix(viewPos));

	lightBall_.matrix = lightBallMatrix(viewPos);
	lightBall_.updateDevice();
}

// PointLight
// implementors NOTE: this way of rendering the shadow map seemed *really*
// inefficient to me first (6 seperate render passes with copies in between)
// but it probably is the best we can get with vulkan... We can't create a
// render pass with 6 subpasses (one for each surface) since which subpassIndex
// specify in the depth pipeline then? we'd need 6 pipelines which seems
// unrealistic. For rendering directly into the cube map we'd need 6
// framebuffers which may also waste resources (but that may be an alternative
// worth investigating... we'd still need 6 seperate renderpasses though)
PointLight::PointLight(const vpp::Device& dev, const vpp::TrDsLayout& dsLayout,
		const vpp::TrDsLayout& primitiveDsLayout, const ShadowData& data,
		const Material& mat) {
	auto extent = vk::Extent3D{size_.x, size_.y, 1u};

	// target
	auto targetUsage = vk::ImageUsageBits::depthStencilAttachment |
		vk::ImageUsageBits::transferSrc;
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

	// shadow map
	targetInfo->img.arrayLayers = 6u;
	targetInfo->img.flags = vk::ImageCreateBits::cubeCompatible;
	targetInfo->img.usage = vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::transferDst;
	targetInfo->view.subresourceRange.layerCount = 6u;
	targetInfo->view.components = {};
	targetInfo->view.viewType = vk::ImageViewType::cube;
	shadowMap_ = {dev, *targetInfo};

	// initial layout change
	auto cb = dev.commandAllocator().get(dev.queueSubmitter().queue().family());
	vk::beginCommandBuffer(cb, {});
	vpp::changeLayout(cb, shadowMap_.image(),
		vk::ImageLayout::undefined, vk::PipelineStageBits::topOfPipe, {},
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::topOfPipe, {},
		{vk::ImageAspectBits::depth, 0, 1, 0, 6u});
	vk::endCommandBuffer(cb);

	// TODO: don't wait here... batch that work
	vk::SubmitInfo submission;
	submission.commandBufferCount = 1;
	submission.pCommandBuffers = &cb.vkHandle();
	dev.queueSubmitter().add(submission);
	dev.queueSubmitter().wait(dev.queueSubmitter().current());

	// setup light ds and ubo
	auto hostMem = dev.hostMemoryTypes();
	auto lightUboSize = 6 * sizeof(nytl::Mat4f) + // 6 * projection;view
		sizeof(this->data);
	ds_ = {dev.descriptorAllocator(), dsLayout};
	ubo_ = {dev.bufferAllocator(), lightUboSize,
		vk::BufferUsageBits::uniformBuffer, 0, hostMem};

	vpp::DescriptorSetUpdate ldsu(ds_);
	ldsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
	ldsu.imageSampler({{data.sampler, shadowMap(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	vpp::apply({ldsu});

	// primitive
	auto sphere = doi::Sphere{{}, {0.1f, 0.1f, 0.1f}};
	auto shape = doi::generateUV(sphere);
	lightBall_ = {dev, shape, primitiveDsLayout,
		mat, lightBallMatrix(), 0xFF}; // TODO: id

	updateDevice();
}

nytl::Mat4f PointLight::lightBallMatrix() const {
	// slight offset since otherwise we end up in ball after
	// setting it to current position
	return translateMat(this->data.position + nytl::Vec3f{0.2, 0.2, 0.2});
}

nytl::Mat4f PointLight::lightMatrix(unsigned i) const {
	// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
	// NOTE: we can't use y inversion in the vertex shader since that
	// produce wrong results for the +y and -y faces.
	// we instead flip up for all other faces
	static constexpr struct {
		nytl::Vec3f normal;
		nytl::Vec3f up;
	} views[6] = {
		{{1.f, 0.f, 0.f}, {0.f, -1.f, 0.f}},
		{{-1.f, 0.f, 0.f}, {0.f, -1.f, 0.f}},
		{{0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}},
		{{0.f, -1.f, 0.f}, {0.f, 0.f, -1.f}},
		{{0.f, 0.f, 1.f}, {0.f, -1.f, 0.f}},
		{{0.f, 0.f, -1.f}, {0.f, -1.f, 0.f}},
	};

	// NOTE: we could also just generate matrix once and then rotate
	auto fov = 0.5 * nytl::constants::pi;
	auto aspect = 1.f;
	auto np = 0.1f;
	auto fp = this->data.farPlane;
	auto mat = doi::perspective3RH<float>(fov, aspect, np, fp);
	mat = mat * doi::lookAtRH(this->data.position,
		this->data.position + views[i].normal,
		views[i].up);
	return mat;

	/*
	// alternative implementation that uses manual rotations (and translation)
	// instead lookAt
	// in parts from
	// https://github.com/SaschaWillems/Vulkan/blob/master/examples/shadowmappingomni/shadowmappingomni.cpp
	auto pi = float(nytl::constants::pi);
	auto fov = 0.5 * pi;
	auto aspect = 1.f;
	auto np = 0.1f;
	auto fp = this->data.farPlane;
	auto mat = doi::perspective3RH<float>(fov, aspect, np, fp);
	auto viewMat = nytl::identity<4, float>();

	switch(i) {
	case 0:
		// viewMat = doi::rotateMat({1.f, 0.f, 0.f}, pi)
		viewMat = doi::rotateMat({0.f, 1.f, 0.f}, pi / 2);
		break;
	case 1:	// NEGATIVE_X
		// viewMat = doi::rotateMat({1.f, 0.f, 0.f}, pi)
		viewMat = doi::rotateMat({0.f, 1.f, 0.f}, -pi / 2);
		break;
	case 2:	// POSITIVE_Y
		viewMat = doi::rotateMat({1.f, 0.f, 0.f}, pi / 2);
		break;
	case 3:	// NEGATIVE_Y
		viewMat = doi::rotateMat({1.f, 0.f, 0.f}, -pi / 2);
		break;
	case 4:	// POSITIVE_Z
		viewMat = doi::rotateMat({1.f, 0.f, 0.f}, pi);
		break;
	case 5:	// NEGATIVE_Z
		viewMat = doi::rotateMat({0.f, 0.f, 1.f}, pi);
		break;
	}

	return mat * viewMat * doi::translateMat({-this->data.position});
	*/
}

void PointLight::render(vk::CommandBuffer cb, const ShadowData& data,
		const Scene& scene) {
	// TODO: fine tune these values!
	// maybe they should be scene dependent? dependent on size/scale?
	// TODO: not sure if this has an effect here. Read up in vulkan spec
	// if applied even when fragment shader sets gl_FragDepth manually
	vk::cmdSetDepthBias(cb, 1.0, 0.f, 8.0);

	auto pl = data.pl.vkHandle();
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, data.pipeCube);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pl, 0, {ds_}, {});

	vpp::changeLayout(cb, shadowMap_.image(),
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::allGraphics,
		vk::AccessBits::shaderRead,
		vk::ImageLayout::transferDstOptimal,
		vk::PipelineStageBits::transfer,
		vk::AccessBits::transferWrite,
		{vk::ImageAspectBits::depth, 0, 1, 0, 6u});

	// for(auto& normal : normals) {
	for(std::uint32_t i = 0u; i < 6u; ++i) {
		vk::ClearValue clearValue {};
		clearValue.depthStencil = {1.f, 0u};

		// render into shadow map
		vk::cmdBeginRenderPass(cb, {
			data.rp, fb_,
			{0u, 0u, size_.x, size_.y},
			1, &clearValue
		}, {});

		vk::Viewport vp {0.f, 0.f, (float) size_.x, (float) size_.y, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, size_.x, size_.y});

		// needed so vertex shader knows which projection matrix to use
		vk::cmdPushConstants(cb, data.pl, vk::ShaderStageBits::vertex,
			pcrOffsetFaceID, 4u, &i);
		scene.render(cb, pl);
		vk::cmdEndRenderPass(cb);

		// TODO: transitions could be done in extra render pass
		// copy from render target to shadow cube map
		vpp::changeLayout(cb, target_.image(),
			vk::ImageLayout::depthStencilReadOnlyOptimal,
			vk::PipelineStageBits::allGraphics, {},
			vk::ImageLayout::transferSrcOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			{vk::ImageAspectBits::depth, 0, 1, 0, 1});
		vk::ImageCopy copy {};
		copy.extent = {size_.x, size_.y, 1};
		copy.dstOffset = {0u, 0u, 0u};
		copy.srcOffset = {0u, 0u, 0u};
		copy.srcSubresource.aspectMask = vk::ImageAspectBits::depth;
		copy.srcSubresource.layerCount = 1u;
		copy.srcSubresource.baseArrayLayer = 0u;
		copy.dstSubresource.aspectMask = vk::ImageAspectBits::depth;
		copy.dstSubresource.baseArrayLayer = i;
		copy.dstSubresource.layerCount = 1u;
		vk::cmdCopyImage(cb, target_.vkImage(),
			vk::ImageLayout::transferSrcOptimal,
			shadowMap_.vkImage(), vk::ImageLayout::transferDstOptimal,
			{copy});
		// TODO: needed?
		vpp::changeLayout(cb, target_.image(),
			vk::ImageLayout::transferSrcOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			vk::ImageLayout::depthStencilAttachmentOptimal,
			vk::PipelineStageBits::allGraphics,
			vk::AccessBits::depthStencilAttachmentWrite |
				vk::AccessBits::depthStencilAttachmentRead,
			{vk::ImageAspectBits::depth, 0, 1, 0, 1});
	}

	// TODO
	vpp::changeLayout(cb, shadowMap_.image(),
		vk::ImageLayout::transferDstOptimal,
		vk::PipelineStageBits::transfer,
		vk::AccessBits::transferWrite,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::allGraphics,
		vk::AccessBits::shaderRead,
		{vk::ImageAspectBits::depth, 0, 1, 0, 6u});
}

void PointLight::updateDevice() {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	doi::write(span, this->data);
	for(auto i = 0u; i < 6u; ++i) {
		doi::write(span, lightMatrix(i));
	}

	lightBall_.matrix = lightBallMatrix();
	lightBall_.updateDevice();
}

} // namespace doi
