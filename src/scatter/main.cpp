#include <tkn/app2.hpp>
#include <tkn/render.hpp>
#include <tkn/shader.hpp>
#include <tkn/camera.hpp>
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
#include <shaders/scatter.pp.frag.h>
#include <shaders/tkn.fullscreen.vert.h>

#include <shaders/tkn.shadow.vert.h>
#include <shaders/tkn.shadow-mv.vert.h>
#include <shaders/tkn.shadow.frag.h>
#include <shaders/tkn.shadowPoint.vert.h>
#include <shaders/tkn.shadowPoint-mv.vert.h>
#include <shaders/tkn.shadowPoint.frag.h>

#include <random>

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
	static constexpr float near = 0.1f;
	static constexpr float far = 100.f;
	static constexpr float fov = 0.5 * nytl::constants::pi;

	struct CamUbo {
		Mat4f vp;
		Vec3f camPos;
		float _;
		Mat4f vpInv;
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
			4, 5, 1,
			1, 2, 5,
			5, 6, 2,
			2, 3, 6,
			6, 7, 3,
			3, 0, 7,
			7, 4, 0,
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
		auto pass0 = {0u, 1u};
		auto rpi = renderPassInfo(
			{{vk::Format::r16g16b16a16Sfloat, depthFormat_}},
			{{pass0}});
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

		geom_.pipeLayout = {dev,
			{{geom_.dsLayout, shadowData_.dsLayout}}, {}};
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
			{{pass0}});
		scatter_.rp = {dev, rpi.info()};
		vpp::nameHandle(scatter_.rp, "scatter.rp");

		// layout
		auto bindings = {
			vpp::descriptorBinding( // depthTex
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment,
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

		// ds
		scatter_.ds = {dev.descriptorAllocator(), scatter_.dsLayout};
	}

	void initPP() {
		auto& dev = vkDevice();

		// rp
		auto pass0 = {0u};
		auto rpi = renderPassInfo(
			{{swapchainInfo().imageFormat}},
			{{pass0}});
		rpi.attachments[0].finalLayout = vk::ImageLayout::presentSrcKHR;
		pp_.rp = {dev, rpi.info()};
		vpp::nameHandle(pp_.rp, "pp.rp");

		// layout
		auto bindings = {
			vpp::descriptorBinding( // colorTex
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
			vpp::descriptorBinding( // scatterTex
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle())
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

		// offscreen targets
		auto cinfo = vpp::ViewableImageCreateInfo(
			vk::Format::r16g16b16a16Sfloat,
			vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::colorAttachment |
				vk::ImageUsageBits::sampled);
		vpp::Init<vpp::ViewableImage> initColor(dev.devMemAllocator(),
			cinfo.img, dev.deviceMemoryTypes());

		auto sinfo = vpp::ViewableImageCreateInfo(
			vk::Format::r16g16b16a16Sfloat,
			vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::colorAttachment |
				vk::ImageUsageBits::sampled);
		vpp::Init<vpp::ViewableImage> initScatter(dev.devMemAllocator(),
			sinfo.img, dev.deviceMemoryTypes());

		auto dinfo = vpp::ViewableImageCreateInfo(
			depthFormat_,
			vk::ImageAspectBits::depth, size,
			vk::ImageUsageBits::depthStencilAttachment |
				vk::ImageUsageBits::sampled);
		vpp::Init<vpp::ViewableImage> initDepth(dev.devMemAllocator(),
			dinfo.img, dev.deviceMemoryTypes());

		geom_.colorTarget = initColor.init(cinfo.view);
		geom_.depthTarget = initDepth.init(dinfo.view);
		scatter_.target = initScatter.init(sinfo.view);

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
			geom_.depthTarget.vkImageView()};
		fbi.renderPass = geom_.rp;
		fbi.attachmentCount = gatts.size();
		fbi.pAttachments = gatts.begin();
		geom_.fb = {dev, fbi};

		auto satts = {scatter_.target.vkImageView()};
		fbi.renderPass = scatter_.rp;
		fbi.attachmentCount = satts.size();
		fbi.pAttachments = satts.begin();
		scatter_.fb = {dev, fbi};

		// descriptors
		auto sdsu = vpp::DescriptorSetUpdate(scatter_.ds);
		sdsu.imageSampler({{{}, geom_.depthTarget.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});

		auto pdsu = vpp::DescriptorSetUpdate(pp_.ds);
		pdsu.imageSampler({{{}, geom_.colorTarget.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		pdsu.imageSampler({{{}, scatter_.target.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});

		vpp::apply({{sdsu, pdsu}});
	}

	void record(const RenderBuffer& rb) override {
		auto [width, height] = windowSize();
		auto& cb = rb.commandBuffer;
		auto& dev = vkDevice();

		vk::beginCommandBuffer(cb, {});

		// bind geometry
		vk::cmdBindVertexBuffers(cb, 0, {{geom_.vertices.buffer()}},
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
			std::array<vk::ClearValue, 2u> cv {};
			cv[0] = {0.f, 0.f, 0.f, 0.f}; // color, rgba16f
			cv[1].depthStencil = {1.f, 0u}; // depth

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

			std::array<vk::ClearValue, 1u> cv {};
			cv[0] = {0.f, 0.f, 0.f, 0.f}; // color
			vk::cmdBeginRenderPass(cb, {scatter_.rp, scatter_.fb,
				{0u, 0u, width, height},
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
		tkn::checkMovement(camera_, swaDisplay(), dt);
		App::scheduleRedraw();
	}

	nytl::Mat4f projectionMatrix() const {
		auto aspect = float(windowSize().x) / windowSize().y;
		return tkn::perspective3RH(fov, aspect, near, far);
	}

	nytl::Mat4f cameraVP() const {
		return projectionMatrix() * viewMatrix(camera_);
	}

	void updateDevice() override {
		App::updateDevice();

		if(camera_.update) {
			auto map = camUbo_.memoryMap();
			auto span = map.span();

			CamUbo ubo;
			ubo.vp = cameraVP();
			ubo.camPos = camera_.pos;
			ubo.vpInv = nytl::Mat4f(nytl::inverse(ubo.vp));
			tkn::write(span, ubo);
			map.flush();
		}
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

		constexpr auto btn = swa_mouse_button_left;
		constexpr auto fac = 0.005;
		if(swa_display_mouse_button_pressed(swaDisplay(), btn)) {
			tkn::rotateView(camera_, fac * ev.dx, fac * ev.dy);
		}
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
				l.data.position = camera_.pos;
				l.data.radius = rd(rnd);
				l.updateDevice();
				App::scheduleRerecord();
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
	tkn::Camera camera_;
	vpp::SubBuffer camUbo_;
	vpp::Sampler sampler_;

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
		vpp::SubBuffer ubo;
		vpp::RenderPass rp;
	} scatter_;

	// pass 3: post processing
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

