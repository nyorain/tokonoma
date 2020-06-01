#include <tkn/app2.hpp>
#include <tkn/taa.hpp>
#include <tkn/render.hpp>
#include <tkn/image.hpp>
#include <tkn/features.hpp>
#include <tkn/texture.hpp>
#include <tkn/shader.hpp>
#include <tkn/ccam.hpp>
#include <tkn/types.hpp>
#include <tkn/util.hpp>
#include <tkn/bits.hpp>
#include <tkn/scene/light.hpp>

#include <vpp/handles.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/debug.hpp>
#include <vpp/queue.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/submit.hpp>
#include <vpp/formats.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/init.hpp>
#include <vpp/vk.hpp>

#include <shaders/scatter.model.vert.h>
#include <shaders/scatter.model.frag.h>
#include <shaders/scatter.scatter.vert.h>
#include <shaders/scatter.scatter.frag.h>
#include <shaders/scatter.combine.comp.h>
#include <shaders/scatter.pp.frag.h>
#include <shaders/tkn.fullscreen.vert.h>

#include <shaders/tkn.shadow.vert.h>
#include <shaders/tkn.shadow-mv.vert.h>
#include <shaders/tkn.shadow.frag.h>
#include <shaders/tkn.shadowPoint.vert.h>
#include <shaders/tkn.shadowPoint-mv.vert.h>
#include <shaders/tkn.shadowPoint.frag.h>

#include <random>

// TODO:
// - add TAAPass. This allows us to sample longer rays
//   and go down with the steps. Investigate if this maybe
//   even means we can get rid of the dithering?
// - correctly resolve the dithering (4x4 blurs? maybe just use
//   a really small gaussian pass?) instead of the current
//   hack-mess in pp.frag
//   - 4x4 dithering is kinda bad. Rather use 3x3 and then
//     just blur in 3x3 neighborhood.
// - optimization: start and end ray at outer light radius,
//   instead of starting at camera and going to infinity.
//   Should *greatly* increase what we get for our samples

using namespace tkn::types;

struct RenderPassInfo {
	vk::RenderPassCreateInfo renderPass;
	std::vector<vk::AttachmentDescription> attachments;
	std::vector<vk::SubpassDescription> subpasses;
	std::vector<vk::SubpassDependency> dependencies;

	std::deque<std::vector<vk::AttachmentReference>> colorRefs;
	std::deque<vk::AttachmentReference> depthRefs;

	vk::RenderPassCreateInfo info() {
		renderPass.pAttachments = attachments.data();
		renderPass.attachmentCount = attachments.size();
		renderPass.pSubpasses = subpasses.data();
		renderPass.subpassCount = subpasses.size();
		renderPass.dependencyCount = dependencies.size();
		renderPass.pDependencies = dependencies.data();
		return renderPass;
	}
};

// - no dependencies or flags or something
// - initialLayout always 'undefined'
// - finalLayout always 'shaderReadOnlyOptimal'
// - clearOp clear, storeOp store
// Passes contains the ids of the attachments used by the passes.
// Depending on the format they will be attached as color or depth
// attachment. Input, preserve attachments or multisampling not
// supported here.
RenderPassInfo renderPassInfo(nytl::Span<const vk::Format> formats,
		nytl::Span<const nytl::Span<const unsigned>> passes) {
	RenderPassInfo rpi;
	for(auto f : formats) {
		auto& a = rpi.attachments.emplace_back();
		a.format = f;
		a.initialLayout = vk::ImageLayout::undefined;
		a.finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		a.loadOp = vk::AttachmentLoadOp::clear;
		a.storeOp = vk::AttachmentStoreOp::store;
		a.samples = vk::SampleCountBits::e1;
	}

	for(auto pass : passes) {
		auto& subpass = rpi.subpasses.emplace_back();
		auto& colorRefs = rpi.colorRefs.emplace_back();
		subpass.pipelineBindPoint = vk::PipelineBindPoint::graphics;

		bool depth = false;
		for(auto id : pass) {
			dlg_assert(id < rpi.attachments.size());
			auto format = formats[id];
			if(tkn::isDepthFormat(format)) {
				dlg_assertm(!depth, "More than one depth attachment");
				depth = true;
				auto& ref = rpi.depthRefs.emplace_back();
				ref.attachment = id;
				ref.layout = vk::ImageLayout::depthStencilAttachmentOptimal;
				subpass.pDepthStencilAttachment = &ref;
			} else {
				auto& ref = colorRefs.emplace_back();
				ref.attachment = id;
				ref.layout = vk::ImageLayout::colorAttachmentOptimal;
			}
		}

		subpass.pColorAttachments = colorRefs.data();
		subpass.colorAttachmentCount = colorRefs.size();
	}

	return rpi;
}


class ScatterApp : public tkn::App {
public:
	// TODO: can probably make this unorm with right encoding
	static constexpr auto velFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto combineGroupDimSize = 8u;

	static constexpr auto scatterDownscale = 0u;

	struct CamUbo {
		Mat4f vp;
		Mat4f vpPrev;
		Mat4f vpInv;
		Vec3f camPos;
		u32 frameCount;
		Vec2f jitter;
		float near;
		float far;
	};

public:
	bool init(nytl::Span<const char*> args) override {
		if(!App::init(args)) {
			return false;
		}

		auto& dev = vkDevice();
		depthFormat_ = tkn::findDepthFormat(dev);
		if(depthFormat_ == vk::Format::undefined) {
			dlg_error("No depth format supported");
			return false;
		}

		// sampler
		vk::SamplerCreateInfo sci;
		sci.addressModeU = vk::SamplerAddressMode::clampToEdge;
		sci.addressModeV = vk::SamplerAddressMode::clampToEdge;
		sci.addressModeW = vk::SamplerAddressMode::clampToEdge;
		sci.magFilter = vk::Filter::linear;
		sci.minFilter = vk::Filter::linear;
		sci.mipmapMode = vk::SamplerMipmapMode::linear;
		sci.minLod = 0.0;
		sci.maxLod = 100.0;
		sci.anisotropyEnable = false;
		// scene rendering has its own samplers
		// sci.anisotropyEnable = anisotropy_;
		// sci.maxAnisotropy = dev.properties().limits.maxSamplerAnisotropy;
		sampler_ = {dev, sci};
		vpp::nameHandle(sampler_, "linearSampler");

		// camera ubo
		auto camUboSize = sizeof(CamUbo);
		camUbo_ = {dev.bufferAllocator(), camUboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		initGeom();
		initScatter();
		initCombine();
		taa_.init(dev, sampler_);
		taa_.params.velWeight = 0.f;
		taa_.params.flags = taa_.flagClosestDepth |
			taa_.flagColorClip;
		initPP();

		// init first light
		auto wb = tkn::WorkBatcher::createDefault(dev);
		auto& l = lights_.emplace_back(wb, shadowData_);
		l.data.position = {1.f, 1.f, 1.f};
		l.data.radius = 10.f;
		l.updateDevice();

		return true;
	}

	void initGeom() {
		// upload geometry
		std::vector<Vec3f> vertices = {
			// outer rect
			{-1.f, -1.f, 0.f},
			{1.f, -1.f, 0.f},
			{1.f, 1.f, 0.f},
			{-1.f, 1.f, 0.f},

			// inner rect
			{-0.2f, -0.2f, 0.f},
			{0.2f, -0.2f, 0.f},
			{0.2f, 0.2f, 0.f},
			{-0.2f, 0.2f, 0.f},
		};

		// we render using triangle list
		std::vector<u32> indices = {
			0, 1, 4,
			5, 4, 1,
			1, 2, 5,
			6, 5, 2,
			2, 3, 6,
			7, 6, 3,
			3, 0, 7,
			4, 7, 0,
		};

		geom_.numIndices = indices.size();

		auto vertSpan = tkn::bytes(vertices);
		auto indSpan = tkn::bytes(indices);

		auto& dev = vkDevice();
		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());

		geom_.vertices = {dev.bufferAllocator(), vertSpan.size(),
			vk::BufferUsageBits::vertexBuffer | vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes()};
		geom_.indices = {dev.bufferAllocator(), indSpan.size(),
			vk::BufferUsageBits::indexBuffer | vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes()};

		vk::beginCommandBuffer(cb, {});
		vpp::fillDirect(cb, geom_.vertices, vertSpan);
		vpp::fillDirect(cb, geom_.indices, indSpan);
		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		// create render pass
		auto pass0 = {0u, 1u, 2u};
		auto rpi = renderPassInfo(
			{{vk::Format::r16g16b16a16Sfloat, velFormat, depthFormat_}},
			{{{pass0}}});
		vk::SubpassDependency dep;
		dep.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dep.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
		dep.dstStageMask = vk::PipelineStageBits::computeShader;
		dep.dstAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		dep.srcSubpass = 0u;
		dep.dstSubpass = vk::subpassExternal;
		rpi.dependencies.push_back(dep);
		rpi.attachments[0].finalLayout = vk::ImageLayout::general;
		geom_.rp = {dev, rpi.info()};
		vpp::nameHandle(geom_.rp, "geom.rp");

		// layouts
		auto dsBindings = {
			vpp::descriptorBinding( // ubo
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		geom_.dsLayout = {dev, dsBindings};
		vpp::nameHandle(geom_.dsLayout, "geom.dsLayout");

		// vertex description
		constexpr vk::VertexInputBindingDescription bindings[] = {
			{0, sizeof(Vec3f), vk::VertexInputRate::vertex}, // positions
			// {1, sizeof(Vec3f), vk::VertexInputRate::vertex}, // normals
		};

		constexpr vk::VertexInputAttributeDescription attributes[] = {
			{0, 0, vk::Format::r32g32b32Sfloat, 0}, // pos
		};

		const vk::PipelineVertexInputStateCreateInfo vertex = {{},
			1, bindings,
			1, attributes
		};

		// init shadow data
		tkn::ShadowPipelineInfo spi;
		spi.dir = {
			tkn_shadow_vert_data,
			tkn_shadow_mv_vert_data,
			tkn_shadow_frag_data
		};
		spi.point = {
			tkn_shadowPoint_vert_data,
			tkn_shadowPoint_mv_vert_data,
			tkn_shadowPoint_frag_data
		};
		spi.vertex = vertex;
		shadowData_ = tkn::initShadowData(dev, depthFormat_,
			features_.multiview, features_.depthClamp, spi, {});
		shadowData_.depthBias = 50.f;
		shadowData_.depthBiasSlope = 100.f;

		geom_.pipeLayout = {dev, {{geom_.dsLayout.vkHandle(),
			shadowData_.dsLayout.vkHandle()}}, {}};
		vpp::nameHandle(geom_.pipeLayout, "geom.pipeLayout");

		// create pipeline
		vpp::ShaderModule vert{dev, scatter_model_vert_data};
		vpp::ShaderModule frag{dev, scatter_model_frag_data};
		vpp::GraphicsPipelineInfo gpi{geom_.rp, geom_.pipeLayout, {{{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment},
		}}}, 0};

		gpi.vertex = vertex;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
		gpi.rasterization.cullMode = vk::CullModeBits::none;

		auto bas = {
			tkn::defaultBlendAttachment(),
			tkn::defaultBlendAttachment(),
		};

		gpi.blend.attachmentCount = bas.size();
		gpi.blend.pAttachments = bas.begin();

		geom_.pipe = {dev, gpi.info()};
		vpp::nameHandle(geom_.pipe, "geom.pipe");

		// ds
		geom_.ds = {dev.descriptorAllocator(), geom_.dsLayout};
		vpp::nameHandle(geom_.ds, "geom.ds");

		vpp::DescriptorSetUpdate dsu(geom_.ds);
		dsu.uniform({{{camUbo_}}});
	}

	void initScatter() {
		auto& dev = vkDevice();

		// rp
		auto pass0 = {0u};
		auto rpi = renderPassInfo(
			{{vk::Format::r16g16b16a16Sfloat}},
			{{{pass0}}});
		vk::SubpassDependency dep;
		dep.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dep.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
		dep.dstStageMask = vk::PipelineStageBits::fragmentShader;
		dep.dstAccessMask = vk::AccessBits::shaderRead;
		dep.srcSubpass = 0u;
		dep.dstSubpass = vk::subpassExternal;
		rpi.dependencies.push_back(dep);
		scatter_.rp = {dev, rpi.info()};
		vpp::nameHandle(scatter_.rp, "scatter.rp");

		// layout
		auto bindings = {
			vpp::descriptorBinding( // depthTex
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment,
				-1, 1, &sampler_.vkHandle()),
			vpp::descriptorBinding( // noiseTex
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment,
				-1, 1, &sampler_.vkHandle())
		};

		scatter_.dsLayout = {dev, bindings};
		scatter_.pipeLayout = {dev, {{
			geom_.dsLayout.vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			scatter_.dsLayout.vkHandle(),
		}}, {}};

		vpp::nameHandle(scatter_.dsLayout, "scatter.dsLayout");
		vpp::nameHandle(scatter_.pipeLayout, "scatter.pipeLayout");

		// pipeline
		vpp::ShaderModule vert{dev, scatter_scatter_vert_data};
		vpp::ShaderModule frag{dev, scatter_scatter_frag_data};
		vpp::GraphicsPipelineInfo gpi{scatter_.rp, scatter_.pipeLayout, {{{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment},
		}}}, 0};

		gpi.assembly.topology = vk::PrimitiveTopology::triangleStrip;
		gpi.rasterization.cullMode = vk::CullModeBits::back;
		gpi.rasterization.frontFace = vk::FrontFace::clockwise; // sry

		auto ba = tkn::defaultBlendAttachment();
		ba.dstColorBlendFactor = vk::BlendFactor::one;
		ba.srcColorBlendFactor = vk::BlendFactor::one;

		gpi.blend.attachmentCount = 1u;
		gpi.blend.pAttachments = &ba;

		scatter_.pipe = {dev, gpi.info()};
		vpp::nameHandle(scatter_.pipe, "scatter.pipe");

		// noise tex
		std::array<std::string, 64u> spaths;
		std::array<const char*, 64u> paths;
		for(auto i = 0u; i < 64; ++i) {
			spaths[i] = dlg::format("noise/LDR_LLL1_{}.png", i);
			paths[i] = spaths[i].data();
		}

		auto layers = tkn::read(paths);

		tkn::TextureCreateParams tcp;
		// TODO: fix blitting!
		// tcp.format = vk::Format::r8Unorm;
		tcp.format = vk::Format::r8g8b8a8Unorm;
		tcp.srgb = false; // TODO: not sure tbh
		auto tex = tkn::Texture(dev, tkn::read(paths), tcp);
		scatter_.noise = std::move(tex.viewableImage());

		// ds
		scatter_.ds = {dev.descriptorAllocator(), scatter_.dsLayout};
	}

	void initCombine() {
		auto& dev = vkDevice();

		auto s = combineGroupDimSize;
		tkn::ComputeGroupSizeSpec cgs(s, s);

		auto bindings = {
			vpp::descriptorBinding( // color (input/output)
				vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding( // scatter tex
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute,
				-1, 1, &sampler_.vkHandle())
		};

		combine_.dsLayout = {dev, bindings};
		combine_.pipeLayout = {dev, {{combine_.dsLayout.vkHandle()}}, {}};
		vpp::nameHandle(combine_.dsLayout, "combine.dsLayout");
		vpp::nameHandle(combine_.pipeLayout, "combine.pipeLayout");

		vpp::ShaderModule mod(dev, scatter_combine_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = combine_.pipeLayout;
		cpi.stage.module = mod;
		cpi.stage.pName = "main";
		cpi.stage.pSpecializationInfo = &cgs.spec;
		cpi.stage.stage = vk::ShaderStageBits::compute;
		combine_.pipe = {dev, cpi};

		combine_.ds = {dev.descriptorAllocator(), combine_.dsLayout};
	}

	void initPP() {
		auto& dev = vkDevice();

		// rp
		auto pass0 = {0u};
		auto rpi = renderPassInfo(
			{{swapchainInfo().imageFormat}},
			{{{pass0}}});
		rpi.attachments[0].finalLayout = vk::ImageLayout::presentSrcKHR;
		pp_.rp = {dev, rpi.info()};
		vpp::nameHandle(pp_.rp, "pp.rp");

		// layout
		auto bindings = {
			vpp::descriptorBinding( // colorTex
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		};

		pp_.dsLayout = {dev, bindings};
		pp_.pipeLayout = {dev, {{
			pp_.dsLayout.vkHandle(),
		}}, {}};

		vpp::nameHandle(pp_.dsLayout, "pp.dsLayout");
		vpp::nameHandle(pp_.pipeLayout, "pp.pipeLayout");

		// pipeline
		vpp::ShaderModule vert{dev, tkn_fullscreen_vert_data};
		vpp::ShaderModule frag{dev, scatter_pp_frag_data};
		vpp::GraphicsPipelineInfo gpi{pp_.rp, pp_.pipeLayout, {{{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment},
		}}}, 0};

		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;

		pp_.pipe = {dev, gpi.info()};
		vpp::nameHandle(pp_.pipe, "pp.pipe");

		// ds
		pp_.ds = {dev.descriptorAllocator(), pp_.dsLayout};
	}

	void initBuffers(const vk::Extent2D& size,
			nytl::Span<RenderBuffer> bufs) override {
		auto& dev = vkDevice();
		auto hsize = size;
		hsize.width = std::max(hsize.width >> scatterDownscale, 1u);
		hsize.height = std::max(hsize.height >> scatterDownscale, 1u);

		// offscreen targets
		auto cinfo = vpp::ViewableImageCreateInfo(
			vk::Format::r16g16b16a16Sfloat,
			vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::colorAttachment |
				vk::ImageUsageBits::storage |
				vk::ImageUsageBits::sampled);
		vpp::Init<vpp::ViewableImage> initColor(dev.devMemAllocator(),
			cinfo.img, dev.deviceMemoryTypes());

		auto sinfo = vpp::ViewableImageCreateInfo(
			vk::Format::r16g16b16a16Sfloat,
			vk::ImageAspectBits::color, hsize,
			vk::ImageUsageBits::colorAttachment |
				vk::ImageUsageBits::sampled);
		vpp::Init<vpp::ViewableImage> initScatter(dev.devMemAllocator(),
			sinfo.img, dev.deviceMemoryTypes());

		auto vinfo = vpp::ViewableImageCreateInfo(
			velFormat, vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::colorAttachment |
				vk::ImageUsageBits::sampled);
		vpp::Init<vpp::ViewableImage> initVel(dev.devMemAllocator(),
			vinfo.img, dev.deviceMemoryTypes());

		auto dinfo = vpp::ViewableImageCreateInfo(
			depthFormat_,
			vk::ImageAspectBits::depth, size,
			vk::ImageUsageBits::depthStencilAttachment |
				vk::ImageUsageBits::sampled);
		vpp::Init<vpp::ViewableImage> initDepth(dev.devMemAllocator(),
			dinfo.img, dev.deviceMemoryTypes());

		geom_.colorTarget = initColor.init(cinfo.view);
		geom_.depthTarget = initDepth.init(dinfo.view);
		geom_.velTarget = initVel.init(vinfo.view);
		scatter_.target = initScatter.init(sinfo.view);

		taa_.initBuffers(size,
			geom_.colorTarget.imageView(),
			geom_.depthTarget.imageView(),
			geom_.velTarget.imageView());

		// framebuffers
		vk::FramebufferCreateInfo fbi;
		fbi.width = size.width;
		fbi.height = size.height;
		fbi.layers = 1;

		for(auto& b : bufs) {
			auto attachments = {b.imageView.vkHandle()};
			fbi.renderPass = pp_.rp;
			fbi.attachmentCount = attachments.size();
			fbi.pAttachments = attachments.begin();
			b.framebuffer = {dev, fbi};
		}

		auto gatts = {
			geom_.colorTarget.vkImageView(),
			geom_.velTarget.vkImageView(),
			geom_.depthTarget.vkImageView()};
		fbi.renderPass = geom_.rp;
		fbi.attachmentCount = gatts.size();
		fbi.pAttachments = gatts.begin();
		geom_.fb = {dev, fbi};

		auto satts = {scatter_.target.vkImageView()};
		fbi.renderPass = scatter_.rp;
		fbi.attachmentCount = satts.size();
		fbi.pAttachments = satts.begin();
		fbi.width = hsize.width;
		fbi.height = hsize.height;
		scatter_.fb = {dev, fbi};

		// descriptors
		auto sdsu = vpp::DescriptorSetUpdate(scatter_.ds);
		sdsu.imageSampler({{{}, geom_.depthTarget.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		sdsu.imageSampler({{{}, scatter_.noise.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});

		auto pdsu = vpp::DescriptorSetUpdate(pp_.ds);
		pdsu.imageSampler({{{}, taa_.targetView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});

		auto cdsu = vpp::DescriptorSetUpdate(combine_.ds);
		cdsu.storage({{{}, geom_.colorTarget.vkImageView(),
			vk::ImageLayout::general}});
		cdsu.imageSampler({{{}, scatter_.target.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});

		vpp::apply({{{sdsu}, {pdsu}, {cdsu}}});
	}

	void record(const RenderBuffer& rb) override {
		auto [width, height] = windowSize();
		auto& cb = rb.commandBuffer;
		auto& dev = vkDevice();

		vk::beginCommandBuffer(cb, {});

		// bind geometry
		vk::cmdBindVertexBuffers(cb, 0, {{geom_.vertices.buffer().vkHandle()}},
			{{geom_.vertices.offset()}});
		vk::cmdBindIndexBuffer(cb, geom_.indices.buffer(),
			geom_.indices.offset(), vk::IndexType::uint32);

		// prepass: shadow
		{
			vpp::DebugLabel lbl(dev, cb, "shadow");
			auto renderScene = [this](vk::CommandBuffer cb,
					vk::PipelineLayout) {
				vk::cmdDrawIndexed(cb, geom_.numIndices, 1, 0, 0, 0);
			};

			for(auto& l : lights_) {
				l.render(cb, shadowData_, renderScene);
			}
		}

		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		// render scene
		{
			vpp::DebugLabel lbl(dev, cb, "geometry");
			std::array<vk::ClearValue, 3u> cv {};
			cv[0] = {0.f, 0.f, 0.f, 0.f}; // color, rgba16f
			cv[1] = {0.f, 0.f, 0.f, 0.f}; // vel, rgba16f
			cv[2].depthStencil = {1.f, 0u}; // depth

			vk::cmdBeginRenderPass(cb, {geom_.rp, geom_.fb,
				{0u, 0u, width, height},
				u32(cv.size()), cv.data()}, {});

			// NOTE: we just lit the objects by the first light
			dlg_assert(!lights_.empty());

			tkn::cmdBindGraphicsDescriptors(cb, geom_.pipeLayout, 0,
				{geom_.ds, lights_[0].ds()});
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, geom_.pipe);
			vk::cmdDrawIndexed(cb, geom_.numIndices, 1, 0, 0, 0);

			vk::cmdEndRenderPass(cb);
		}

		// render scattering
		{
			vpp::DebugLabel lbl(dev, cb, "scattering");

			auto hwidth = std::max(width >> scatterDownscale, 1u);
			auto hheight = std::max(height >> scatterDownscale, 1u);

			vk::Viewport vp{0.f, 0.f, (float) hwidth, (float) hheight, 0.f, 1.f};
			vk::cmdSetViewport(cb, 0, 1, vp);
			vk::cmdSetScissor(cb, 0, 1, {0, 0, hwidth, hheight});

			std::array<vk::ClearValue, 1u> cv {};
			cv[0] = {0.f, 0.f, 0.f, 0.f}; // color
			vk::cmdBeginRenderPass(cb, {scatter_.rp, scatter_.fb,
				{0u, 0u, hwidth, hheight},
				u32(cv.size()), cv.data()}, {});

			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, scatter_.pipe);
			tkn::cmdBindGraphicsDescriptors(cb, scatter_.pipeLayout, 0,
				{{geom_.ds}});
			tkn::cmdBindGraphicsDescriptors(cb, scatter_.pipeLayout, 2,
				{{scatter_.ds}});
			for(auto& l : lights_) {
				tkn::cmdBindGraphicsDescriptors(cb, scatter_.pipeLayout, 1,
					{{l.ds()}});
				vk::cmdDraw(cb, 14, 1, 0, 0); // cube
			}

			vk::cmdEndRenderPass(cb);
		}

		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		// combine
		{
			// basically ceil(width / groupSizeX)
			auto cx = (width + combineGroupDimSize - 1) / combineGroupDimSize;
			auto cy = (height + combineGroupDimSize - 1) / combineGroupDimSize;

			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, combine_.pipe);
			tkn::cmdBindComputeDescriptors(cb, combine_.pipeLayout, 0, {combine_.ds});
			vk::cmdDispatch(cb, cx, cy, 1);

			tkn::barrier(cb, geom_.colorTarget.image(),
				tkn::SyncScope::computeReadWrite(), taa_.dstScopeInput());
		}

		// taa
		taa_.record(cb, {width, height});
		tkn::barrier(cb, taa_.targetImage(), taa_.srcScope(),
			tkn::SyncScope::fragmentSampled());

		// post process
		{
			vpp::DebugLabel lbl(dev, cb, "post process");
			std::array<vk::ClearValue, 1u> cv {};
			cv[0] = {0.f, 0.f, 0.f, 0.f}; // color

			vk::cmdBeginRenderPass(cb, {pp_.rp, rb.framebuffer,
				{0u, 0u, width, height},
				u32(cv.size()), cv.data()}, {});
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
			tkn::cmdBindGraphicsDescriptors(cb, pp_.pipeLayout, 0,
				{{pp_.ds}});
			vk::cmdDraw(cb, 4, 1, 0, 0); // fulllscreen quad
			vk::cmdEndRenderPass(cb);
		}

		vk::endCommandBuffer(cb);
	}

	void update(double dt) override {
		App::update(dt);
		camera_.update(swaDisplay(), dt);
		App::scheduleRedraw();
	}

	void updateDevice() override {
		App::updateDevice();

		// camera always needs update
		{
			auto map = camUbo_.memoryMap();
			auto span = map.span();

			auto [width, height] = swapchainInfo().imageExtent;

			using namespace nytl::vec::cw::operators;
			auto disable = false;
			auto sample = disable ? nytl::Vec2f{0.f, 0.f} : taa_.nextSample();
			auto pixSize = Vec2f{1.f / width, 1.f / height};
			auto off = sample * pixSize;

			CamUbo ubo;
			ubo.vp = camera_.viewProjectionMatrix();
			ubo.vpInv = nytl::Mat4f(nytl::inverse(ubo.vp));
			ubo.vpPrev = lastVP_;
			ubo.camPos = camera_.position();
			ubo.jitter = off;
			ubo.frameCount = ++frameCount_;
			ubo.near = -camera_.near();
			ubo.far = -camera_.far();

			tkn::write(span, ubo);
			map.flush();

			lastVP_ = ubo.vp;
		}

		taa_.updateDevice(-camera_.near(), -camera_.far());
	}

	bool features(tkn::Features& enable,
			const tkn::Features& supported) override {
		if(!App::features(enable, supported)) {
			return false;
		}

		if(supported.multiview.multiview) {
			features_.multiview = true;
			enable.multiview.multiview = true;
		} else {
			dlg_warn("Multiview not supported");
		}

		if(supported.base.features.depthClamp) {
			features_.depthClamp = true;
			enable.base.features.depthClamp = true;
		} else {
			dlg_warn("DepthClamp not supported");
		}

		return true;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		App::mouseMove(ev);
		camera_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	bool mouseWheel(float dx, float dy) override {
		if(App::mouseWheel(dx, dy)) {
			return true;
		}

		camera_.mouseWheel(dy);
		return true;
	}

	void resize(unsigned w, unsigned h) override {
		App::resize(w, h);
		camera_.aspect({w, h});
	}

	bool key(const swa_key_event& ev) override {
		if(App::key(ev)) {
			return true;
		}

		static std::default_random_engine rnd(std::time(nullptr));
		if(ev.pressed) {
			switch(ev.keycode) {
			case swa_key_n: {
				std::uniform_int_distribution<unsigned> td(1000, 8000);
				std::uniform_real_distribution<float> rd(0.f, 20.f);

				auto wb = tkn::WorkBatcher::createDefault(vkDevice());
				auto& l = lights_.emplace_back(wb, shadowData_);
				l.data.color = tkn::blackbody(td(rnd));
				l.data.position = camera_.position();
				l.data.radius = rd(rnd);
				l.updateDevice();
				App::scheduleRerecord();
				break;
			} case swa_key_t: {
				using Ctrl = tkn::ControlledCamera::ControlType;
				swa_cursor c {};
				if(camera_.controlType() == Ctrl::firstPerson) {
					camera_.useControl(Ctrl::arcball);
					c.type = swa_cursor_default;
				} else if(camera_.controlType() == Ctrl::arcball) {
					camera_.useControl(Ctrl::firstPerson);
					c.type = swa_cursor_none;
				}

				swa_window_set_cursor(swaWindow(), c);
				break;
			} case swa_key_r: {
				taa_.resetHistory = true;
				break;
			} default:
				return false;
			}

			return true;
		}

		return false;
	}

	const char* name() const override { return "scatter"; }

protected:
	vk::Format depthFormat_;
	vpp::SubBuffer camUbo_;
	vpp::Sampler sampler_;

	tkn::ControlledCamera camera_;
	Mat4f lastVP_;
	unsigned frameCount_ {};

	struct {
		bool multiview {};
		bool depthClamp {};
	} features_;

	// pass 0: shadow
	tkn::ShadowData shadowData_;
	std::vector<tkn::PointLight> lights_;

	// pass 1: render geometry
	struct {
		vpp::RenderPass rp;
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
		vpp::ViewableImage depthTarget;
		vpp::ViewableImage colorTarget;
		vpp::ViewableImage velTarget;
		vpp::Framebuffer fb;

		vpp::SubBuffer vertices;
		vpp::SubBuffer indices;
		unsigned numIndices {};
	} geom_;

	// pass 2: render volumetric scattering
	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
		vpp::ViewableImage target;
		vpp::Framebuffer fb;
		vpp::RenderPass rp;
		vpp::ViewableImage noise;
	} scatter_;

	// pass 3: combine
	// just adds scatter_.target onto geom_.colorTarget
	// using a compute shader
	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
	} combine_;

	// pass 4: temporal antialiasing
	tkn::TAAPass taa_;

	// pass 5: post processing
	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
		vpp::SubBuffer ubo;
		vpp::RenderPass rp;
	} pp_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<ScatterApp>(argc, argv);
}

