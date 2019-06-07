#include <stage/scene/light.hpp>
#include <stage/scene/scene.hpp>
#include <stage/scene/shape.hpp>
#include <stage/camera.hpp>
#include <stage/bits.hpp>
#include <stage/transform.hpp>
#include <vpp/vk.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/formats.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/device.hpp>
#include <vpp/queue.hpp>

#include <shaders/stage.shadowmap.vert.h>
#include <shaders/stage.shadowmap.multiview.vert.h>
#include <shaders/stage.shadowmap.frag.h>
#include <shaders/stage.shadowmapCube.vert.h>
#include <shaders/stage.shadowmapCube.multiview.vert.h>
#include <shaders/stage.shadowmapCube.frag.h>

// TODO: some duplication between dir and point light
// TODO: use allocators from work batcher
// TODO: directional light: when depth clamp isn't supported, emulate.
//   see shadowmap.vert

namespace doi {

Frustum ndcFrustum() {
	return {{
		{-1.f, 1.f, 0.f},
		{1.f, 1.f, 0.f},
		{1.f, -1.f, 0.f},
		{-1.f, -1.f, 0.f},
		{-1.f, 1.f, 1.f},
		{1.f, 1.f, 1.f},
		{1.f, -1.f, 1.f},
		{-1.f, -1.f, 1.f},
	}};
}

/// Returns a u32 that has the last *count* bits set to 1, all others to 0.
/// Useful for vulkan masks. count must be < 32.
constexpr u32 nlastbits(u32 count) {
	u32 ret = 0;
	for(auto i = 0u; i < count; ++i) {
		ret |= (1 << i);
	}
	return ret;
}

ShadowData initShadowData(const vpp::Device& dev, vk::Format depthFormat,
		vk::DescriptorSetLayout lightDsLayout,
		vk::DescriptorSetLayout sceneDsLayout,
		bool multiview, bool depthClamp) {
	ShadowData data;
	data.depthFormat = depthFormat;
	data.multiview = multiview;

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
	dependency.srcAccessMask =
		vk::AccessBits::memoryWrite |
		vk::AccessBits::depthStencilAttachmentWrite |
		vk::AccessBits::depthStencilAttachmentRead;
	dependency.dstSubpass = vk::subpassExternal;
	dependency.dstStageMask = vk::PipelineStageBits::fragmentShader;
	dependency.dstAccessMask = vk::AccessBits::shaderRead;

	vk::RenderPassCreateInfo rpi {};
	rpi.attachmentCount = 1;
	rpi.pAttachments = &depth;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &subpass;
	rpi.pDependencies = &dependency;
	rpi.dependencyCount = 1u;

	if(multiview) {
		u32 viewMask = nlastbits(DirLight::cascadeCount);
		vk::RenderPassMultiviewCreateInfo rpm;
		rpm.subpassCount = 1u;
		rpm.pViewMasks = &viewMask;
		rpi.pNext = &rpm;
		data.rpDir = {dev, rpi};

		viewMask = nlastbits(6);
		rpm.pViewMasks = &viewMask;
		data.rpPoint = {dev, rpi};
	} else {
		// TODO: could be optimized i guess
		data.rpDir = {dev, rpi};
		data.rpPoint = {dev, rpi};
	}

	// sampler
	vk::SamplerCreateInfo sci {};
	sci.addressModeU = vk::SamplerAddressMode::clampToBorder;
	sci.addressModeV = vk::SamplerAddressMode::clampToBorder;
	sci.addressModeW = vk::SamplerAddressMode::clampToBorder;
	// white: region outside shadow map has light
	// black: region outside shadow map has no light
	sci.borderColor = vk::BorderColor::floatOpaqueWhite;
	// sci.borderColor = vk::BorderColor::floatOpaqueBlack;
	sci.mipmapMode = vk::SamplerMipmapMode::nearest;
	sci.compareEnable = true;
	sci.compareOp = vk::CompareOp::lessOrEqual;
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	// sci.magFilter = vk::Filter::nearest;
	// sci.minFilter = vk::Filter::nearest;
	data.sampler = {dev, sci};

	// pipeline layout
	if(!multiview) {
		vk::PushConstantRange facePcr;
		facePcr.offset = 0u;
		facePcr.stageFlags = vk::ShaderStageBits::vertex;
		facePcr.size = 4u;

		data.pl = {dev, {{lightDsLayout, sceneDsLayout}}, {{facePcr}}};
	} else {
		data.pl = {dev, {{lightDsLayout, sceneDsLayout}}, {}};
	}

	// pipeline
	vpp::ShaderModule vertShader;
	if(multiview) {
		vertShader = {dev, stage_shadowmap_multiview_vert_data};
	} else {
		vertShader = {dev, stage_shadowmap_vert_data};
	}

	vpp::ShaderModule fragShader(dev, stage_shadowmap_frag_data);
	vpp::GraphicsPipelineInfo gpi {data.rpDir, data.pl, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}, 0, vk::SampleCountBits::e1};

	// see cubemap pipeline below
	gpi.flags(vk::PipelineCreateBits::allowDerivatives);
	gpi.vertex = doi::Scene::vertexInfo();
	gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

	gpi.depthStencil.depthTestEnable = true;
	gpi.depthStencil.depthWriteEnable = true;
	gpi.depthStencil.depthCompareOp = vk::CompareOp::less;

	// Pipeline-stage culling at all probably brings problems for
	// doubleSided primitives/materials so we cull dynamically
	// in fragment shader
	gpi.rasterization.cullMode = vk::CullModeBits::none;
	gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
	gpi.rasterization.depthBiasEnable = true;
	gpi.rasterization.depthClampEnable = depthClamp;

	auto dynamicStates = {
		vk::DynamicState::depthBias,
		vk::DynamicState::viewport,
		vk::DynamicState::scissor
	};
	gpi.dynamic.pDynamicStates = dynamicStates.begin();
	gpi.dynamic.dynamicStateCount = dynamicStates.end() - dynamicStates.begin();

	gpi.blend.attachmentCount = 0;
	data.pipe = {dev, gpi.info()};

	// cubemap pipe
	// TODO: we could alternatively always use a uniform buffer with
	// dynamic offset for the light view projection... but then we
	// might have to pad that buffer (since vulkan has an alignment
	// requirement there) which adds complexity as well.
	// We don't need the matrices when reading the shadow map though
	vpp::ShaderModule cubeVertShader;
	if(multiview) {
		cubeVertShader = {dev, stage_shadowmapCube_multiview_vert_data};
	} else {
		cubeVertShader = {dev, stage_shadowmapCube_vert_data};
	}
	vpp::ShaderModule cubeFragShader(dev, stage_shadowmapCube_frag_data);
	vpp::GraphicsPipelineInfo cgpi {data.rpPoint, data.pl, {{{
		{cubeVertShader, vk::ShaderStageBits::vertex},
		{cubeFragShader, vk::ShaderStageBits::fragment},
	}}}, 0, vk::SampleCountBits::e1};

	cgpi.dynamic = gpi.dynamic;
	cgpi.blend = gpi.blend;
	cgpi.rasterization = gpi.rasterization;
	cgpi.depthStencil = gpi.depthStencil;
	cgpi.assembly = gpi.assembly;
	cgpi.vertex = gpi.vertex;
	cgpi.base(data.pipe); // basically the same, except vertex shader
	cgpi.rasterization.depthBiasEnable = true;
	cgpi.rasterization.depthClampEnable = depthClamp;
	data.pipeCube = {dev, cgpi.info()};

	return data;
}

// DirLight
DirLight::DirLight(const WorkBatcher& wb, const vpp::TrDsLayout& dsLayout,
		const vpp::TrDsLayout&, const ShadowData& data, unsigned) {
	auto& dev = wb.dev;

	// target
	auto targetUsage = vk::ImageUsageBits::depthStencilAttachment |
		vk::ImageUsageBits::sampled;
	auto info = vpp::ViewableImageCreateInfo(data.depthFormat,
		vk::ImageAspectBits::depth, {size_.x, size_.y}, targetUsage);
	info.img.arrayLayers = cascadeCount;
	info.view.subresourceRange.layerCount = cascadeCount;
	info.view.viewType = vk::ImageViewType::e2dArray;
	dlg_assert(vpp::supported(dev, info.img));
	target_ = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

	// framebuffer
	vk::FramebufferCreateInfo fbi {};
	fbi.width = size_.x;
	fbi.height = size_.y;
	fbi.renderPass = data.rpDir;
	fbi.layers = 1; // for multiview: specified by vulkan spec
	fbi.attachmentCount = 1;
	if(data.multiview) {
		fbi.pAttachments = &target_.vkImageView();
		fb_ = {dev, fbi};
	} else {
		info.view.subresourceRange.layerCount = 1;
		info.view.image = target_.image();
		cascades_.resize(cascadeCount);
		for(auto i = 0u; i < cascadeCount; ++i) {
			auto& c = cascades_[i];
			info.view.subresourceRange.baseArrayLayer = i;
			c.view = {dev, info.view};

			fbi.pAttachments = &c.view.vkHandle();
			c.fb = {dev, fbi};
		}
	}

	// setup light ds and ubo
	auto hostMem = dev.hostMemoryTypes();
	auto lightUboSize = sizeof(this->data) +
		sizeof(nytl::Mat4f) * cascadeCount +
		sizeof(float) * vpp::align(cascadeCount, 4u);
	ds_ = {dev.descriptorAllocator(), dsLayout};
	ubo_ = {dev.bufferAllocator(), lightUboSize,
		vk::BufferUsageBits::uniformBuffer, hostMem};

	vpp::DescriptorSetUpdate ldsu(ds_);
	ldsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
	ldsu.imageSampler({{data.sampler, shadowMap(),
		vk::ImageLayout::depthStencilReadOnlyOptimal}});
	ldsu.apply();

	// light ball
	// auto cube = doi::Cube{{}, {0.2f, 0.2f, 0.2f}};
	// auto shape = doi::generate(cube);
	// lightBall_ = {wb, shape, primitiveDsLayout, 0u,
	// 	lightBallMatrix(viewPos), id};

	// updateDevice(viewPos);
}

void DirLight::render(vk::CommandBuffer cb, const ShadowData& data,
		const doi::Scene& scene) {
	vk::ClearValue clearValue {};
	clearValue.depthStencil = {1.f, 0u};

	// TODO: fine tune depth bias
	// should be scene dependent, configurable!
	// also dependent on shadow map size (the larger the shadow map, the
	// smaller are the values we need)

	// render into shadow map
	if(data.multiview) {
		vk::cmdBeginRenderPass(cb, {
			data.rpDir, fb_,
			{0u, 0u, size_.x, size_.y},
			1, &clearValue
		}, {});

		vk::Viewport vp {0.f, 0.f, (float) size_.x, (float) size_.y, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, size_.x, size_.y});

		vk::cmdSetDepthBias(cb, 2.0, 0.f, 8.0);

		auto pl = data.pl.vkHandle();
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, data.pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pl, 0, {{ds_.vkHandle()}}, {});

		scene.render(cb, pl, false);
		vk::cmdEndRenderPass(cb);
	} else {
		vk::Viewport vp {0.f, 0.f, (float) size_.x, (float) size_.y, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, size_.x, size_.y});

		vk::cmdSetDepthBias(cb, 2.0, 0.f, 8.0);

		auto pl = data.pl.vkHandle();
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, data.pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pl, 0, {{ds_.vkHandle()}}, {});

		for(u32 i = 0u; i < cascadeCount; ++i) {
			vk::cmdBeginRenderPass(cb, {
				data.rpDir, cascades_[i].fb,
				{0u, 0u, size_.x, size_.y},
				1, &clearValue
			}, {});

			vk::cmdPushConstants(cb, data.pl, vk::ShaderStageBits::fragment,
				0, 4, &i);
			scene.render(cb, pl, false);
			vk::cmdEndRenderPass(cb);
		}
	}
}

/*
nytl::Mat4f DirLight::lightBallMatrix(nytl::Vec3f viewPos) const {
	return translateMat(viewPos - 5.f * nytl::normalized(this->data.dir));
}

nytl::Mat4f DirLight::lightMatrix(nytl::Vec3f viewPos) const {
	// TODO: sizes should be configurable; depend on scene size
	// TODO: can be opimtized, better position
	auto pos = viewPos - 5 * this->data.dir;
	// auto pos = -this->data.dir;

	// discretize
	// using namespace nytl::vec::cw;
	// using namespace nytl::vec::operators;
	auto frustSize = 5.f; // TODO
	// auto step = 2048 * frustSize / size_.x;
	// auto step = 4.f;
	// pos = ceil(pos / step) * step;

	auto mat = doi::ortho3Sym(frustSize, frustSize, 0.5f, 30.f);
	// mat = mat * doi::lookAtRH(pos, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
	mat = mat * doi::lookAtRH(pos, viewPos, {0.f, 1.f, 0.f});
	return mat;
}
*/

// TODO: calculate stuff in update, not updateDevice
void DirLight::updateDevice(const Camera& camera) {
	nytl::normalize(this->data.dir);

	// calculate split depths
	// https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch10.html
	constexpr auto splitLambda = 0.65f; // higher: nearer at log split scheme
	const auto near = camera.perspective.near;
	const auto far = camera.perspective.far;

	// 1: calculate split depths
	std::array<float, cascadeCount> splits;
	for(auto i = 0u; i < cascadeCount; ++i) {
		float fi = (i + 1) / float(cascadeCount);
		float uniform = near + (far - near) * fi;
		float log = near * std::pow(far / near, fi);
		splits[i] = nytl::mix(uniform, log, splitLambda);
	}

	// 2: (un)project frustum to world space
	auto invVP = nytl::Mat4f(nytl::inverse(matrix(camera)));
	auto frustum = ndcFrustum();
	for(auto& p : frustum) {
		p = doi::multPos(invVP, p);
	}

	// 3: calculate cascade projections
	std::array<nytl::Mat4f, cascadeCount> projs;
	float splitBegin = near;
	for(auto i = 0u; i < cascadeCount; ++i) {
		auto splitEnd = (splits[i] - near) / (far - near);
		Frustum frusti;

		// get frustum for this split
		nytl::Vec3f frustCenter {0.f, 0.f, 0.f};
		for(auto j = 0u; j < 4u; ++j) {
			auto diff = frustum[j + 4] - frustum[j];
			frusti[j] = frustum[j] + splitBegin * diff;
			frusti[j + 4] = frustum[j] + splitEnd * diff;

			frustCenter += frusti[j];
			frustCenter += frusti[j + 4];
		}
		frustCenter *= 1 / 8.f;

		float radius = 0.f;
		for(auto& p : frusti) {
			radius = std::max(radius, length(p - frustCenter));
		}

		// quantization
		auto q = (2.f * radius) / (size_.x - 1);

		auto viewMat = lookAtRH({}, data.dir, {0.f, 1.f, 0.f});
		frustCenter = multPos(viewMat, frustCenter);

		auto min = frustCenter - nytl::Vec3f{radius, radius, radius};
		min.x -= std::fmod(min.x, q);
		min.y -= std::fmod(min.y, q);
		min.z -= std::fmod(min.z, q);

		auto max = frustCenter + nytl::Vec3f{radius, radius, radius};
		max.x -= std::fmod(max.x, q);
		max.y -= std::fmod(max.y, q);
		max.z -= std::fmod(max.z, q);

		auto projMat = ortho3(min.x, max.x, max.y, min.y, -max.z, -min.z);
		projs[i] = projMat * viewMat;

		// for next iteration
		splitBegin = splitEnd;
	}

	auto map = ubo_.memoryMap();
	auto span = map.span();
	doi::write(span, this->data);
	doi::write(span, projs);
	doi::write(span, splits);

	// lightBall_.matrix = lightBallMatrix(viewPos);
	// lightBall_.updateDevice();
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
PointLight::PointLight(const WorkBatcher& wb, const vpp::TrDsLayout& dsLayout,
		const vpp::TrDsLayout&, const ShadowData& data,
		unsigned, vk::ImageView noShadowMap) {
	auto& dev = wb.dev;
	if(!noShadowMap) {
		this->data.flags |= lightFlagShadow;

		// target
		auto targetUsage = vk::ImageUsageBits::depthStencilAttachment |
			vk::ImageUsageBits::sampled;
		auto targetInfo = vpp::ViewableImageCreateInfo(data.depthFormat,
			vk::ImageAspectBits::depth, {size_.x, size_.y}, targetUsage);
		targetInfo.img.arrayLayers = 6u;
		targetInfo.img.flags = vk::ImageCreateBits::cubeCompatible;
		targetInfo.view.subresourceRange.layerCount = 6u;
		targetInfo.view.viewType = vk::ImageViewType::cube;
		dlg_assert(vpp::supported(dev, targetInfo.img));
		shadowMap_ = {dev.devMemAllocator(), targetInfo, dev.deviceMemoryTypes()};

		// framebuffer
		vk::FramebufferCreateInfo fbi {};
		fbi.attachmentCount = 1;
		fbi.width = size_.x;
		fbi.height = size_.y;
		fbi.layers = 1u;
		fbi.renderPass = data.rpPoint;
		if(data.multiview) {
			fbi.pAttachments = &shadowMap_.vkImageView();
			fb_ = {dev, fbi};
		} else {
			targetInfo.view.subresourceRange.layerCount = 1;
			targetInfo.view.image = shadowMap_.image();
			faces_.resize(6u);
			for(auto i = 0u; i < 6u; ++i) {
				auto& f = faces_[i];
				targetInfo.view.subresourceRange.baseArrayLayer = i;
				f.view = {dev, targetInfo.view};

				fbi.pAttachments = &f.view.vkHandle();
				f.fb = {dev, fbi};
			}
		}

		// // shadow map
		// targetInfo.img.arrayLayers = 6u;
		// targetInfo.img.flags = vk::ImageCreateBits::cubeCompatible;
		// targetInfo.img.usage = vk::ImageUsageBits::sampled |
		// 	vk::ImageUsageBits::transferDst;
		// targetInfo.view.subresourceRange.layerCount = 6u;
		// targetInfo.view.components = {};
		// targetInfo.view.viewType = vk::ImageViewType::cube;
		// shadowMap_ = {dev.devMemAllocator(), targetInfo};

		// initial layout change
		/*
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
		*/
	}

	// setup light ds and ubo
	auto hostMem = dev.hostMemoryTypes();
	auto lightUboSize = 6 * sizeof(nytl::Mat4f) + // 6 * projection;view
		sizeof(this->data);
	ds_ = {dev.descriptorAllocator(), dsLayout};
	ubo_ = {dev.bufferAllocator(), lightUboSize,
		vk::BufferUsageBits::uniformBuffer, hostMem};

	vpp::DescriptorSetUpdate ldsu(ds_);
	ldsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
	ldsu.imageSampler({{data.sampler, hasShadowMap() ? shadowMap() : noShadowMap,
		vk::ImageLayout::depthStencilReadOnlyOptimal}});
	ldsu.apply();

	// primitive
	// auto sphere = doi::Sphere{{}, {0.02f, 0.02f, 0.02f}};
	// auto shape = doi::generateUV(sphere);
	// lightBall_ = {wb, shape, primitiveDsLayout, 0u, lightBallMatrix(), id};

	updateDevice();
}

nytl::Mat4f PointLight::lightBallMatrix() const {
	// slight offset since otherwise we end up in ball after
	// setting it to current position
	return translateMat(this->data.position);
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
	// auto fp = this->data.farPlane;
	auto fp = this->data.radius;
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
	vk::ClearValue clearValue {};
	clearValue.depthStencil = {1.f, 0u};

	if(data.multiview) {
		vk::cmdBeginRenderPass(cb, {
			data.rpPoint, fb_,
			{0u, 0u, size_.x, size_.y},
			1, &clearValue
		}, {});

		vk::Viewport vp {0.f, 0.f, (float) size_.x, (float) size_.y, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, size_.x, size_.y});

		// vk::cmdSetDepthBias(cb, 2.0, 0.f, 8.0);

		auto pl = data.pl.vkHandle();
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, data.pipeCube);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pl, 0, {{ds_.vkHandle()}}, {});

		scene.render(cb, pl, false);
		vk::cmdEndRenderPass(cb);
	} else {
		vk::Viewport vp {0.f, 0.f, (float) size_.x, (float) size_.y, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, size_.x, size_.y});

		// vk::cmdSetDepthBias(cb, 2.0, 0.f, 8.0);

		auto pl = data.pl.vkHandle();
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, data.pipeCube);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pl, 0, {{ds_.vkHandle()}}, {});

		for(u32 i = 0u; i < 6u; ++i) {
			vk::cmdBeginRenderPass(cb, {
				data.rpPoint, faces_[i].fb,
				{0u, 0u, size_.x, size_.y},
				1, &clearValue
			}, {});

			vk::cmdPushConstants(cb, data.pl, vk::ShaderStageBits::fragment,
				0, 4, &i);
			scene.render(cb, pl, false);
			vk::cmdEndRenderPass(cb);
		}
	}

	// auto pl = data.pl.vkHandle();
	// vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, data.pipeCube);
	// vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
	// 	pl, 0, {{ds_.vkHandle()}}, {});


	/*
	vk::ImageMemoryBarrier cubeBarrier;
	cubeBarrier.image = shadowMap_.image();
	cubeBarrier.oldLayout = vk::ImageLayout::undefined;
	cubeBarrier.srcAccessMask = vk::AccessBits::shaderRead;
	cubeBarrier.newLayout = vk::ImageLayout::transferDstOptimal;
	cubeBarrier.dstAccessMask = vk::AccessBits::transferWrite;
	cubeBarrier.subresourceRange = {vk::ImageAspectBits::depth, 0, 1, 0, 6u};
	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::allGraphics,
		vk::PipelineStageBits::transfer,
		{}, {}, {}, {{cubeBarrier}});

	// vpp::changeLayout(cb, shadowMap_.image(),
	// 	vk::ImageLayout::shaderReadOnlyOptimal,
	// 	vk::ImageLayout::transferDstOptimal,
	// 	vk::AccessBits::transferWrite,
	// 	{vk::ImageAspectBits::depth, 0, 1, 0, 6u});

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

		vk::ImageMemoryBarrier barrier;
		barrier.image = target_.image();
		barrier.oldLayout = vk::ImageLayout::depthStencilReadOnlyOptimal;
		barrier.srcAccessMask = vk::AccessBits::depthStencilAttachmentWrite;
		barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.dstAccessMask = vk::AccessBits::transferRead;
		barrier.subresourceRange = {vk::ImageAspectBits::depth, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::allGraphics,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrier}});

		// TODO: transitions could be done in extra render pass
		// copy from render target to shadow cube map
		// vpp::changeLayout(cb, target_.image(),
		// 	vk::ImageLayout::depthStencilReadOnlyOptimal,
		// 	vk::PipelineStageBits::allGraphics, {},
		// 	vk::ImageLayout::transferSrcOptimal,
		// 	vk::PipelineStageBits::transfer,
		// 	vk::AccessBits::transferRead,
		// 	{vk::ImageAspectBits::depth, 0, 1, 0, 1});

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
			{{copy}});

		barrier.oldLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.newLayout = vk::ImageLayout::depthStencilAttachmentOptimal;
		barrier.srcAccessMask = vk::AccessBits::transferRead;
		barrier.dstAccessMask =	vk::AccessBits::depthStencilAttachmentWrite |
			vk::AccessBits::depthStencilAttachmentRead,
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::allGraphics,
			{}, {}, {}, {{barrier}});

		// vpp::changeLayout(cb, target_.image(),
		// 	vk::ImageLayout::transferSrcOptimal,
		// 	vk::PipelineStageBits::transfer,
		// 	vk::AccessBits::transferRead,
		// 	vk::ImageLayout::depthStencilAttachmentOptimal,
		// 	vk::PipelineStageBits::allGraphics,
		// 	vk::AccessBits::depthStencilAttachmentWrite |
		// 		vk::AccessBits::depthStencilAttachmentRead,
		// 	{vk::ImageAspectBits::depth, 0, 1, 0, 1});
	}

	cubeBarrier.oldLayout = vk::ImageLayout::transferDstOptimal;
	cubeBarrier.srcAccessMask = vk::AccessBits::transferWrite;
	cubeBarrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	cubeBarrier.dstAccessMask = vk::AccessBits::shaderRead;
	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::transfer,
		vk::PipelineStageBits::fragmentShader |
			vk::PipelineStageBits::computeShader,
		{}, {}, {}, {{cubeBarrier}});

	// vpp::changeLayout(cb, shadowMap_.image(),
	// 	vk::ImageLayout::transferDstOptimal,
	// 	vk::PipelineStageBits::transfer,
	// 	vk::AccessBits::transferWrite,
	// 	vk::ImageLayout::shaderReadOnlyOptimal,
	// 	vk::PipelineStageBits::fragmentShader,
	// 	vk::AccessBits::shaderRead,
	// 	{vk::ImageAspectBits::depth, 0, 1, 0, 6u});
	*/
}

void PointLight::updateDevice() {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	doi::write(span, this->data);
	for(auto i = 0u; i < 6u; ++i) {
		doi::write(span, lightMatrix(i));
	}

	// lightBall_.matrix = lightBallMatrix();
	// lightBall_.updateDevice();
}

} // namespace doi
