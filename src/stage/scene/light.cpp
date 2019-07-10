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

namespace doi {

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
		vk::DescriptorSetLayout sceneDsLayout, bool multiview, bool depthClamp) {
	ShadowData data;
	data.depthFormat = depthFormat;
	data.multiview = multiview;

	// renderpass
	vk::AttachmentDescription depth {};
	depth.initialLayout = vk::ImageLayout::undefined;
	depth.finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
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
		// TODO: could be optimized i guess, both render passes
		// are the same here
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

	auto lightBindings = {
		vpp::descriptorBinding( // ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // shadowmap
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &data.sampler.vkHandle()),
	};

	data.dsLayout = {dev, lightBindings};

	// pipeline layout
	if(!multiview) {
		vk::PushConstantRange facePcr;
		facePcr.offset = 0u;
		facePcr.stageFlags = vk::ShaderStageBits::vertex;
		facePcr.size = 4u;

		data.pl = {dev, {{data.dsLayout.vkHandle(), sceneDsLayout}}, {{facePcr}}};
	} else {
		data.pl = {dev, {{data.dsLayout.vkHandle(), sceneDsLayout}}, {}};
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
	gpi.dynamic.dynamicStateCount = dynamicStates.size();

	gpi.blend.attachmentCount = 0;
	data.pipe = {dev, gpi.info()};

	// cubemap pipe
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
DirLight::DirLight(const WorkBatcher& wb, const ShadowData& data) {
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
	target_ = {wb.alloc.memDevice, info, dev.deviceMemoryTypes()};

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
	ds_ = {wb.alloc.ds, data.dsLayout};
	ubo_ = {wb.alloc.bufHost, lightUboSize,
		vk::BufferUsageBits::uniformBuffer, hostMem};

	vpp::DescriptorSetUpdate ldsu(ds_);
	ldsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
	ldsu.imageSampler({{{}, shadowMap(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ldsu.apply();
}

void DirLight::render(vk::CommandBuffer cb, const ShadowData& data,
		const doi::Scene& scene) {
	vk::ClearValue clearValue {};
	clearValue.depthStencil = {1.f, 0u};

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

		vk::cmdSetDepthBias(cb, data.depthBias, 0.f, data.depthBiasSlope);

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

		vk::cmdSetDepthBias(cb, data.depthBias, 0.f, data.depthBiasSlope);

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

// TODO: calculate stuff in update, not updateDevice
void DirLight::updateDevice(const Camera& camera) {
	nytl::normalize(this->data.dir);

	// calculate split depths
	// https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch10.html
	constexpr auto splitLambda = 0.5f; // higher: nearer at log split scheme
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

		// quantization to get stable shadow maps
		// this makes sure that even when moving the camera the shadows
		// stay the same.
		// FIXME: something here seems to not work correctly (maybe just
		// some kind of numerical error though?) since we still get
		// minimal flickering on individual pixels. Not too bad though

		// q: the size of on shadow map texel in view/world space
		// size_.x - 1 is (theoretically?) important to make sure we
		// really cover all space and there isn't space between the shadow
		// maps that is not covered now due to the quantization move
		auto q = (2.f * radius) / (size_.x - 1);

		auto viewMat = lookAtRH({}, data.dir, {0.f, 1.f, 0.f});
		frustCenter = multPos(viewMat, frustCenter);

		// Quantatizing in z is not really needed since that only
		// quantatizes the depth (not their projection onto the shadow
		// map) which isn't relaly important
		auto min = frustCenter - nytl::Vec3f{radius, radius, radius};
		min.x -= std::fmod(min.x, q);
		min.y -= std::fmod(min.y, q);
		// min.z -= std::fmod(min.z, q);

		auto max = frustCenter + nytl::Vec3f{radius, radius, radius};
		max.x -= std::fmod(max.x, q);
		max.y -= std::fmod(max.y, q);
		// max.z -= std::fmod(max.z, q);

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
}

// PointLight
PointLight::PointLight(const WorkBatcher& wb, const ShadowData& data,
		vk::ImageView noShadowMap) {
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
		shadowMap_ = {wb.alloc.memDevice, targetInfo, dev.deviceMemoryTypes()};

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
	}

	// setup light ds and ubo
	auto hostMem = dev.hostMemoryTypes();
	auto lightUboSize = 6 * sizeof(nytl::Mat4f) + // 6 * projection;view
		sizeof(this->data);
	ds_ = {wb.alloc.ds, data.dsLayout};
	ubo_ = {wb.alloc.bufHost, lightUboSize,
		vk::BufferUsageBits::uniformBuffer, hostMem};

	vpp::DescriptorSetUpdate ldsu(ds_);
	ldsu.uniform({{{ubo_}}});
	ldsu.imageSampler({{{},
		hasShadowMap() ? shadowMap() : noShadowMap,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ldsu.apply();

	updateDevice();
}

void PointLight::render(vk::CommandBuffer cb, const ShadowData& data,
		const Scene& scene) {
	vk::ClearValue clearValue {};
	clearValue.depthStencil = {1.f, 0u};

	if(data.multiview) {
		vk::cmdBeginRenderPass(cb, {
			data.rpPoint, fb_,
			{0u, 0u, size_.x, size_.y},
			1, &clearValue
		}, {});

		vk::cmdSetDepthBias(cb, data.depthBias, 0.f, data.depthBiasSlope);
		vk::Viewport vp {0.f, 0.f, (float) size_.x, (float) size_.y, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, size_.x, size_.y});

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

		auto pl = data.pl.vkHandle();
		vk::cmdSetDepthBias(cb, data.depthBias, 0.f, data.depthBiasSlope);
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
}

void PointLight::updateDevice() {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	doi::write(span, this->data);

	auto np = 0.1f;
	auto fp = this->data.radius;
	for(auto i = 0u; i < 6u; ++i) {
		auto mat = doi::cubeProjectionVP(this->data.position, i, np, fp);
		doi::write(span, mat);
	}
}

} // namespace doi
