#include "sky.hpp"

#include <tkn/app.hpp>
#include <tkn/ccam.hpp>
#include <tkn/render.hpp>
#include <tkn/bits.hpp>
#include <tkn/util.hpp>
#include <tkn/texture.hpp>
#include <tkn/formats.hpp>
#include <tkn/scene/environment.hpp>
#include <tkn/scene/shape.hpp>
#include <tkn/passes/highlight.hpp>
#include <tkn/passes/bloom.hpp>
#include <tkn/passes/luminance.hpp>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/submit.hpp>
#include <vpp/queue.hpp>
#include <vpp/debug.hpp>
#include <vpp/init.hpp>
#include <vpp/vk.hpp>

#include <dlg/dlg.hpp>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/planet.model.vert.h>
#include <shaders/planet.model.frag.h>
#include <shaders/planet.update.comp.h>
#include <shaders/planet.dispatch.comp.h>
#include <shaders/planet.pp.frag.h>
#include <shaders/planet.sky.comp.h>
#include <shaders/planet.apply.comp.h>
#include <shaders/planet.gend.comp.h>

using namespace tkn::types;

#define nameHandle(x) vpp::nameHandle(x, #x)

class PlanetApp : public tkn::App {
public:
	static constexpr auto colorFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto atmosGroupDimSize = 8u;
	static constexpr auto applyGroupDimSize = 8u;
	static constexpr auto genGroupDimSize = 8u;
	static constexpr auto bloomLevels = 6u;

	static constexpr auto nTilesPD = 128;
	static constexpr auto maxTileSize = 1024;
	static constexpr auto heightmapSize = 3 * maxTileSize;
	static constexpr auto nLods = 10u;

	struct TerrainUboData {
		nytl::Mat4f vp;
		nytl::Vec3f eye;
		u32 enable;
		nytl::Vec3u32 centerTile;
		u32 _0;
		Atmosphere atmosphere;
	};

	struct AtmosphereUboData {
		Atmosphere atmosphere;
		vec3 toSun;
		float camAspect;
		vec3 viewPos;
		float camNear;
		vec3 camDir;
		float camFar;
		vec3 camUp;
		float camFov;
	};

	struct PostParams {
		float exposure;
	};

	struct ApplyUboData {
		u32 nBloomLevels;
		float strength;
		float gain;
	};

	static nytl::Vec3u32 tilePos(vec3 dir) {
		auto [face, suv] = tkn::cubemap::face(dir);
		auto uv = 0.5f + 0.5f * suv; // range [0, 1]
		Vec2f tid;
		std::modf(uv.x * nTilesPD, &tid.x);
		std::modf(uv.y * nTilesPD, &tid.y);
		return {u32(tid.x), u32(tid.y), u32(face)};
	}

public:
	using Base = tkn::App;
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		cam_.useSpaceshipControl();
		cam_.near(-1.f);
		cam_.far(-100000.f);
		auto& c = **cam_.spaceshipControl();
		c.controls.move.mult = 500.f;
		c.controls.move.fastMult = 20.f;
		c.controls.move.slowMult = 0.01f;

		auto& dev = vkDevice();
		depthFormat_ = tkn::findDepthFormat(dev);
		if(depthFormat_ == vk::Format::undefined) {
			dlg_error("No depth format supported");
			return false;
		}

		auto si = tkn::linearSamplerInfo();
		sampler_ = vpp::Sampler(dev, si);

		si.mipmapMode = vk::SamplerMipmapMode::nearest;
		si.minFilter = vk::Filter::nearest;
		si.magFilter = vk::Filter::nearest;
		nearestSampler_ = vpp::Sampler(dev, si);

		auto devMem = dev.deviceMemoryTypes();
		ubo_ = {dev.bufferAllocator(), sizeof(TerrainUboData),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		// indirect dispatch buffer
		comp_.dispatch = {dev.bufferAllocator(),
			sizeof(vk::DrawIndirectCommand) + sizeof(vk::DispatchIndirectCommand),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::indirectBuffer |
				vk::BufferUsageBits::transferDst, devMem};

		// vertices/indices
		// auto shape = tkn::generateIco(0u);
		auto shape = tkn::generateUV(tkn::Sphere{}, 8u, 8u);
		std::vector<nytl::Vec4f> vverts;
		for(auto& v : shape.positions) {
			vverts.emplace_back(v);
		}

		auto verts = tkn::bytes(vverts);
		auto inds = tkn::bytes(shape.indices);

		vertices_ = {dev.bufferAllocator(), verts.size(),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::transferDst, devMem};
		indices_ = {dev.bufferAllocator(), inds.size(),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::transferDst, devMem};

		// key buffers
		// TODO: allow buffers to grow dynamically
		// condition can simply be checked. Modify compute shaders
		// to check for out-of-bounds
		auto triCount = 10 * 1024 * 1024;
		auto bufSize = triCount * sizeof(nytl::Vec2u32);
		auto usage = vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::transferSrc |
			vk::BufferUsageBits::transferDst;

		keys0_ = {dev.bufferAllocator(), bufSize + 8, usage, devMem};
		keys1_ = {dev.bufferAllocator(), bufSize + 8, usage, devMem};

		// upload work
		// writing initial data to buffers
		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		// write initial data to buffers
		tkn::WriteBuffer data0;

		dlg_assert(shape.indices.size() % 3 == 0);
		auto numTris = shape.indices.size() / 3;
		write(data0, u32(numTris)); // counter
		write(data0, 0.f); // padding

		for(auto i = 0u; i < numTris; ++i) {
			write(data0, nytl::Vec2u32 {1, i});
		}

		struct {
			vk::DrawIndirectCommand draw;
			vk::DispatchIndirectCommand dispatch;
		} cmds {};

		cmds.draw.firstInstance = 0;
		cmds.draw.firstVertex = 0;
		cmds.draw.instanceCount = numTris;
		cmds.draw.vertexCount = 3; // triangle (list)
		cmds.dispatch.x = numTris;
		cmds.dispatch.y = 1;
		cmds.dispatch.z = 1;

		auto stage0 = vpp::fillStaging(cb, keys0_, data0.buffer);
		auto stage1 = vpp::fillStaging(cb, comp_.dispatch, tkn::bytes(cmds));
		auto stage3 = vpp::fillStaging(cb, vertices_, verts);
		auto stage4 = vpp::fillStaging(cb, indices_, inds);

		// heightmap
		auto hsize = vk::Extent2D{heightmapSize, heightmapSize};
		auto husage = vk::ImageUsageBits::transferSrc |
			vk::ImageUsageBits::transferDst |
			vk::ImageUsageBits::storage |
			vk::ImageUsageBits::sampled;
		auto info = vpp::ViewableImageCreateInfo(vk::Format::r16g16b16a16Sfloat,
			vk::ImageAspectBits::color, hsize, husage);
		info.img.arrayLayers = nLods;
		info.view.viewType = vk::ImageViewType::e2dArray;
		info.view.subresourceRange.layerCount = nLods;
		heightmap_ = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};
		nameHandle(heightmap_);

		auto wb = tkn::WorkBatcher(dev);
		wb.cb = cb;

		// auto img = tkn::loadImage("heightmap.ktx");
		// tkn::TextureCreateParams params;
		// // create full mip chain
		// params.mipLevels = 0;
		// params.fillMipmaps = true;
		// auto initHeightmap = tkn::createTexture(wb, std::move(img), params);
		// heightmap_ = tkn::initTexture(initHeightmap, wb);
		// nameHandle(heightmap_);

		initGenComp();
		initUpdateComp();
		initIndirectComp();
		initAtmosphere();
		initRenderTerrain();
		initRenderApply();
		initRenderPP();

		// starmap & skybox renderer
		auto data = initStars(wb);

		tkn::HighLightPass::InitData initHighlight;
		highlight_.create(initHighlight, wb, sampler_);

		tkn::LuminancePass::InitData initLum;
		lum_.create(initLum, wb, nearestSampler_, {}); // TODO: add fullscreen vert mod
		// lum_.maxLogLuminance = 100.f; // TODO
		// lum_.minLogLuminance = 2.f; // TODO

		highlight_.init(initHighlight);
		blur_.init(dev, sampler_);
		lum_.init(initLum);

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		return true;
	}

	[[nodiscard]] tkn::TextureInitData initStars(tkn::WorkBatcher& wb) {
		auto& dev = vkDevice();
		auto bindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex),
		};

		stars_.ubo = {dev.bufferAllocator(), sizeof(nytl::Mat4f),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
		stars_.dsLayout.init(dev, bindings);
		stars_.dsCam = {dev.descriptorAllocator(), stars_.dsLayout};

		vpp::DescriptorSetUpdate dsuCam(stars_.dsCam);
		dsuCam.uniform({{{stars_.ubo}}});
		dsuCam.apply();

		tkn::SkyboxRenderer::PipeInfo skyboxInfo;
		skyboxInfo.sampler = sampler_;
		skyboxInfo.camDsLayout = stars_.dsLayout;
		skyboxInfo.renderPass = terrain_.rp;
		stars_.renderer.create(dev, skyboxInfo);

		// auto starmapProvider = tkn::loadImage("galaxy1.ktx");
		auto starmapProvider = tkn::loadImage("galaxy2.ktx");
		tkn::TextureCreateParams params;
		params.cubemap = true;
		params.usage = vk::ImageUsageBits::sampled;
		params.mipLevels = 0u; // generate full chain
		params.fillMipmaps = true;
		params.format = vk::Format::r16g16b16a16Sfloat;
		auto initTex = tkn::createTexture(wb, std::move(starmapProvider), params);
		stars_.map = tkn::initTexture(initTex, wb);

		stars_.dsMap = {dev.descriptorAllocator(), stars_.renderer.dsLayout()};
		vpp::DescriptorSetUpdate dsuMap(stars_.dsMap);
		dsuMap.imageSampler({{{}, stars_.map.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuMap.apply();

		return initTex;
	}

	void initUpdateComp() {
		auto& dev = vkDevice();
		auto bindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // read, old keys
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // write, new keys
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute), // camera
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // verts
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // inds
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_.vkHandle()), // heightmap
		};

		comp_.update.dsLayout.init(dev, bindings);
		nameHandle(comp_.update.dsLayout);
		comp_.update.pipeLayout = {dev, {{comp_.update.dsLayout.vkHandle()}}, {}};
		nameHandle(comp_.update.pipeLayout);

		comp_.update.ds = {dev.descriptorAllocator(), comp_.update.dsLayout};
		nameHandle(comp_.update.ds);

		vpp::DescriptorSetUpdate dsu(comp_.update.ds);
		dsu.storage({{{keys0_}}});
		dsu.storage({{{keys1_}}});
		dsu.uniform({{{ubo_}}});
		dsu.storage({{{vertices_}}});
		dsu.storage({{{indices_}}});
		dsu.imageSampler({{{{}, heightmap_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}}});

		auto mod = vpp::ShaderModule(dev, planet_update_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = comp_.update.pipeLayout;
		cpi.stage.module = mod;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;
		comp_.update.pipe = {dev, cpi};
	}

	void initIndirectComp() {
		auto& dev = vkDevice();
		auto bindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // dispatch buf
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // counter new keys
		};

		comp_.indirect.dsLayout.init(dev, bindings);
		nameHandle(comp_.indirect.dsLayout);
		comp_.indirect.pipeLayout = {dev, {{comp_.indirect.dsLayout.vkHandle()}}, {}};
		nameHandle(comp_.indirect.pipeLayout);

		comp_.indirect.ds = {dev.descriptorAllocator(), comp_.indirect.dsLayout};
		nameHandle(comp_.indirect.ds);

		vpp::DescriptorSetUpdate dsu(comp_.indirect.ds);
		dsu.storage({{{comp_.dispatch}}});
		dsu.storage({{{keys1_}}});

		auto mod = vpp::ShaderModule(dev, planet_dispatch_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = comp_.indirect.pipeLayout;
		cpi.stage.module = mod;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;
		comp_.indirect.pipe = {dev, cpi};
	}

	void initGenComp() {
		auto& dev = vkDevice();
		auto bindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute), // generated heightmap
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute), // centerTile
		};

		gen_.dsLayout.init(dev, bindings);
		nameHandle(gen_.dsLayout);
		gen_.pipeLayout = {dev, {{gen_.dsLayout.vkHandle()}}, {}};
		nameHandle(gen_.pipeLayout);

		gen_.ds = {dev.descriptorAllocator(), gen_.dsLayout};
		nameHandle(gen_.ds);

		vpp::DescriptorSetUpdate dsu(gen_.ds);
		dsu.storage({{{}, heightmap_.imageView(), vk::ImageLayout::general}});
		dsu.uniform({{{ubo_}}});
		dsu.apply();

		auto spec = tkn::ComputeGroupSizeSpec(genGroupDimSize, genGroupDimSize, 1u);
		auto mod = vpp::ShaderModule(dev, planet_gend_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = gen_.pipeLayout;
		cpi.stage.module = mod;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;
		cpi.stage.pSpecializationInfo = &spec.spec;
		gen_.pipe = {dev, cpi};
		nameHandle(gen_.pipe);

		// gen cb
		gen_.sem = vpp::Semaphore(dev);
		gen_.cb = dev.commandAllocator().get(dev.queueSubmitter().queue().family());
		nameHandle(gen_.cb);

		auto cb = gen_.cb.vkHandle();
		vk::beginCommandBuffer(cb, {});

		vk::ImageMemoryBarrier b;
		b.image = heightmap_.image();
		b.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, nLods};
		b.oldLayout = vk::ImageLayout::undefined;
		b.srcAccessMask = {};
		b.newLayout = vk::ImageLayout::general;
		b.dstAccessMask = vk::AccessBits::shaderWrite;

		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::computeShader, {}, {}, {}, {{b}});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, gen_.pipe);
		tkn::cmdBindComputeDescriptors(cb, gen_.pipeLayout, 0, {{gen_.ds}});

		auto gxy = tkn::ceilDivide(heightmapSize, genGroupDimSize);
		vk::cmdDispatch(cb, gxy, gxy, nLods);

		b.oldLayout = vk::ImageLayout::general;
		b.srcAccessMask = vk::AccessBits::shaderWrite;
		b.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		b.dstAccessMask = vk::AccessBits::shaderRead;
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::allCommands, {}, {}, {}, {{b}});

		vk::endCommandBuffer(cb);
	}

	void initAtmosphere() {
		auto& dev = vkDevice();

		auto& sky = atmosphere_.sky;
		sky.configFile = TKN_BASE_DIR "/assets/planet.atmos.qwe";
		atmosphere_.sky.init(dev, sampler_);

		auto bindings = std::array{
			// atmosphere parameters
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// tranmission
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_.vkHandle()),
			// scat rayleigh
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_.vkHandle()),
			// scat mie
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_.vkHandle()),
			// combined scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_.vkHandle()),
			// in depth
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_.vkHandle()),
			// in,out color
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
		};

		atmosphere_.dsLayout.init(dev, bindings);
		nameHandle(atmosphere_.dsLayout);

		vk::PushConstantRange pcr;
		pcr.size = sizeof(u32);
		pcr.stageFlags = vk::ShaderStageBits::compute;
		atmosphere_.pipeLayout = {dev, {{atmosphere_.dsLayout.vkHandle()}}, {{pcr}}};
		nameHandle(atmosphere_.pipeLayout);

		atmosphere_.ubo = {dev.bufferAllocator(), sizeof(AtmosphereUboData),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		atmosphere_.ds = {dev.descriptorAllocator(), atmosphere_.dsLayout};
		nameHandle(atmosphere_.ds);

		// pipe
		auto mod = vpp::ShaderModule(dev, planet_sky_comp_data);
		auto cgs = tkn::ComputeGroupSizeSpec(atmosGroupDimSize, atmosGroupDimSize);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = atmosphere_.pipeLayout;
		cpi.stage.module = mod;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;
		cpi.stage.pSpecializationInfo = &cgs.spec;
		atmosphere_.pipe = {dev, cpi};
	}

	void updateAtmosphereDs() {
		auto& sky = atmosphere_.sky;
		vpp::DescriptorSetUpdate dsuAtmos(atmosphere_.ds);
		dsuAtmos.uniform({{{atmosphere_.ubo}}});
		dsuAtmos.imageSampler({{{}, sky.transTable().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuAtmos.imageSampler({{{}, sky.scatTableRayleigh().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuAtmos.imageSampler({{{}, sky.scatTableMie().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuAtmos.imageSampler({{{}, sky.scatTableCombined().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});

		// window-size-dependent buffers
		dsuAtmos.imageSampler({{{}, terrain_.depth.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuAtmos.storage({{{}, terrain_.color.imageView(),
			vk::ImageLayout::general}});

		// terrain ds
		vpp::DescriptorSetUpdate dsuTerrain(terrain_.ds);
		dsuTerrain.skip(4);
		dsuTerrain.imageSampler({{{{}, sky.transTable().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}}});
		dsuTerrain.imageSampler({{{{}, sky.irradianceTable().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}}});

	}

	void initRenderTerrain() {
		auto& dev = vkDevice();
		auto pass0 = {0u, 1u};
		auto rpi = tkn::renderPassInfo({{colorFormat, depthFormat_}}, {{{pass0}}});
		rpi.attachments[0].finalLayout = vk::ImageLayout::general;

		vk::SubpassDependency dep;
		dep.srcSubpass = 0u;
		dep.srcAccessMask = vk::AccessBits::colorAttachmentWrite |
			vk::AccessBits::depthStencilAttachmentWrite;
		dep.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput |
			vk::PipelineStageBits::earlyFragmentTests;
		dep.dstSubpass = vk::subpassExternal;
		dep.dstAccessMask = vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite;
		dep.dstStageMask = vk::PipelineStageBits::computeShader;
		rpi.dependencies.push_back(dep);

		terrain_.rp = {dev, rpi.info()};
		nameHandle(terrain_.rp);

		auto bindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment), // scene data
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::vertex), // vertices
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::vertex), // indices
			// heightmap
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment,
				&sampler_.vkHandle()),
			// atmosphere transmittance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_.vkHandle()),
			// atmosphere irradiance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_.vkHandle()),
		};

		terrain_.dsLayout.init(dev, bindings);
		nameHandle(terrain_.dsLayout);
		terrain_.pipeLayout = {dev, {{terrain_.dsLayout.vkHandle()}}, {}};
		nameHandle(terrain_.pipeLayout);

		terrain_.ds = {dev.descriptorAllocator(), terrain_.dsLayout};
		nameHandle(terrain_.ds);

		vpp::DescriptorSetUpdate dsu(terrain_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.storage({{{vertices_}}});
		dsu.storage({{{indices_}}});
		dsu.imageSampler({{{{}, heightmap_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}}});

		auto& sky = atmosphere_.sky;
		dsu.imageSampler({{{{}, sky.transTable().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}}});
		dsu.imageSampler({{{{}, sky.irradianceTable().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}}});

		auto vert = vpp::ShaderModule(dev, planet_model_vert_data);
		auto frag = vpp::ShaderModule(dev, planet_model_frag_data);
		auto gpi = vpp::GraphicsPipelineInfo(terrain_.rp, terrain_.pipeLayout, {{{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment},
		}}});

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
		gpi.rasterization.cullMode = vk::CullModeBits::back;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
		gpi.rasterization.polygonMode = vk::PolygonMode::fill;

		constexpr auto stride = sizeof(nytl::Vec2u32); // uvec2
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::instance};

		vk::VertexInputAttributeDescription attributes[1];
		attributes[0].format = vk::Format::r32g32Uint;

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 1u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;

		terrain_.pipe = vpp::Pipeline(vkDevice(), gpi.info());
	}

	void initRenderApply() {
		auto& dev = vkDevice();
		auto bindings = std::array {
			// ubo
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute),
			// bloom
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_.vkHandle()),
			// i/o color
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute)
		};

		applyPass_.dsLayout.init(dev, bindings);
		nameHandle(applyPass_.dsLayout);
		applyPass_.pipeLayout = {dev, {{applyPass_.dsLayout.vkHandle()}}, {}};
		nameHandle(applyPass_.pipeLayout);
		applyPass_.ds = {dev.descriptorAllocator(), applyPass_.dsLayout};
		nameHandle(applyPass_.ds);

		applyPass_.ubo = {dev.bufferAllocator(), sizeof(ApplyUboData),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		auto mod = vpp::ShaderModule(dev, planet_apply_comp_data);
		auto cgs = tkn::ComputeGroupSizeSpec(applyGroupDimSize, applyGroupDimSize);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = applyPass_.pipeLayout;
		cpi.stage.module = mod;
		cpi.stage.pSpecializationInfo = &cgs.spec;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;
		applyPass_.pipe = vpp::Pipeline(dev, cpi);
		nameHandle(applyPass_.pipe);
	}

	void initRenderPP() {
		auto& dev = vkDevice();
		auto pass0 = {0u};
		auto rpi = tkn::renderPassInfo({{swapchainInfo().imageFormat}}, {{{pass0}}});
		rpi.attachments[0].finalLayout = vk::ImageLayout::presentSrcKHR;
		pp_.rp = {dev, rpi.info()};
		nameHandle(pp_.rp);

		auto bindings = std::array {
			// color
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_.vkHandle()),
			// ubo
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
		};

		pp_.dsLayout.init(dev, bindings);
		nameHandle(pp_.dsLayout);
		pp_.pipeLayout = {dev, {{pp_.dsLayout.vkHandle()}}, {}};
		nameHandle(pp_.pipeLayout);
		pp_.ds = {dev.descriptorAllocator(), pp_.dsLayout};
		nameHandle(pp_.ds);

		pp_.ubo = {dev.bufferAllocator(), sizeof(PostParams),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		auto vert = vpp::ShaderModule(dev, tkn_fullscreen_vert_data);
		auto frag = vpp::ShaderModule(dev, planet_pp_frag_data);
		auto gpi = vpp::GraphicsPipelineInfo(pp_.rp, pp_.pipeLayout, {{{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment},
		}}});

		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		pp_.pipe = vpp::Pipeline(dev, gpi.info());
	}

	void compute(vk::CommandBuffer cb) {
		// reset counter in dst buffer to 0
		vk::cmdFillBuffer(cb, keys1_.buffer(), keys1_.offset(), 4, 0);

		// make sure the reset is visible
		vk::BufferMemoryBarrier barrier1;
		barrier1.buffer = keys1_.buffer();
		barrier1.offset = keys1_.offset();
		barrier1.size = 4u;
		barrier1.srcAccessMask = vk::AccessBits::transferWrite;
		barrier1.dstAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;

		// run update pipeline
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader, {}, {}, {{barrier1}}, {});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, comp_.update.pipe);
		tkn::cmdBindComputeDescriptors(cb, comp_.update.pipeLayout, 0, {comp_.update.ds});
		vk::cmdDispatchIndirect(cb, comp_.dispatch.buffer(),
			comp_.dispatch.offset() + sizeof(vk::DrawIndirectCommand));

		// make sure updates in keys1_ is visible
		vk::BufferMemoryBarrier barrier0;
		barrier0.buffer = keys0_.buffer();
		barrier0.offset = keys0_.offset();
		barrier0.size = keys0_.size();
		barrier0.srcAccessMask = vk::AccessBits::shaderRead;
		barrier0.dstAccessMask = vk::AccessBits::transferWrite;

		barrier1.size = keys1_.size();
		barrier1.srcAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		barrier1.dstAccessMask = vk::AccessBits::transferRead |
			vk::AccessBits::shaderRead;

		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::transfer |
				vk::PipelineStageBits::vertexInput |
				vk::PipelineStageBits::computeShader,
			{}, {}, {{barrier0, barrier1}}, {});

		// run indirect pipe to build commands
		// We can't do this with a simple copy since for the dispatch size
		// we have to divide by the compute gropu size. And running
		// a compute shader is likely to be faster then reading the
		// counter value to cpu, computing the division, and writing
		// it back to the gpu.
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, comp_.indirect.pipe);
		tkn::cmdBindComputeDescriptors(cb, comp_.indirect.pipeLayout, 0,
			{comp_.indirect.ds});
		vk::cmdDispatch(cb, 1, 1, 1);

		// copy from keys1_ (the new triangles) to keys0_ (which are
		// used for drawing and in the next update step).
		// we could alternatively use ping-ponging and do 2 steps per
		// frame or just use 2 completely independent command buffers.
		// May be more efficient but harder to sync.
		auto keys1 = vpp::BufferSpan(keys1_.buffer(), keys1_.size(),
			keys1_.offset());
		tkn::cmdCopyBuffer(cb, keys1, keys0_);

		barrier0.srcAccessMask = vk::AccessBits::transferWrite;
		barrier0.dstAccessMask = vk::AccessBits::shaderRead;

		barrier1.srcAccessMask = vk::AccessBits::transferRead;
		barrier1.dstAccessMask = vk::AccessBits::shaderWrite;

		vk::BufferMemoryBarrier barrierIndirect;
		barrierIndirect.buffer = comp_.dispatch.buffer();
		barrierIndirect.offset = comp_.dispatch.offset();
		barrierIndirect.size = comp_.dispatch.size();
		barrierIndirect.srcAccessMask = vk::AccessBits::shaderWrite;
		barrierIndirect.dstAccessMask = vk::AccessBits::indirectCommandRead;

		// make sure the copy is visible for drawing (and the next step)
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer |
				vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::vertexShader |
				vk::PipelineStageBits::drawIndirect |
				vk::PipelineStageBits::computeShader,
			{}, {}, {{barrier0, barrier1, barrierIndirect}}, {});
	}

	void renderTerrain(vk::CommandBuffer cb) {
		tkn::cmdBindGraphicsDescriptors(cb, terrain_.pipeLayout, 0, {terrain_.ds});
		vk::cmdBindVertexBuffers(cb, 0, {{keys0_.buffer().vkHandle()}},
			{{keys0_.offset() + 8}}); // skip counter and padding

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, terrain_.pipe);
		vk::cmdDrawIndirect(cb, comp_.dispatch.buffer(), comp_.dispatch.offset(), 1, 0);

		// stars are also rendered in this pass
		auto& sbr = stars_.renderer;
		tkn::cmdBindGraphicsDescriptors(cb, sbr.pipeLayout(), 0, {stars_.dsCam});
		sbr.render(cb, stars_.dsMap);
	}

	void record(const RenderBuffer& rb) override {
		const auto extent = swapchainInfo().imageExtent;
		const auto [width, height] = extent;
		auto cb = rb.commandBuffer;
		vk::beginCommandBuffer(cb, {});

		compute(cb);

		vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		// render terrain
		auto cvT = std::array {
			vk::ClearValue{0.f, 0.f, 0.f, 1.f}, // color
			vk::ClearValue{1.f, 0u}, // depth
		};
		vk::cmdBeginRenderPass(cb, {
			terrain_.rp, terrain_.fb,
			{0u, 0u, width, height},
			u32(cvT.size()), cvT.data()
		}, {});
		renderTerrain(cb);
		vk::cmdEndRenderPass(cb);

		// atmosphere
		u32 scatOrder = atmosphere_.sky.config_.maxScatOrder;
		vk::cmdPushConstants(cb, atmosphere_.pipeLayout,
			vk::ShaderStageBits::compute, 0u, sizeof(u32), &scatOrder);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, atmosphere_.pipe);
		tkn::cmdBindComputeDescriptors(cb, atmosphere_.pipeLayout, 0, {atmosphere_.ds});

		auto cx = tkn::ceilDivide(width, atmosGroupDimSize);
		auto cy = tkn::ceilDivide(height, atmosGroupDimSize);
		vk::cmdDispatch(cb, cx, cy, 1u);

		auto srcScopeLight = tkn::SyncScope::computeReadWrite(); // from atmos
		auto dstScopeLight = highlight_.dstScopeLight();
		tkn::barrier(cb, terrain_.color.image(), srcScopeLight, dstScopeLight);

		// vk::ImageMemoryBarrier barrier;
		// barrier.image = terrain_.color.image();
		// barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		// barrier.oldLayout = vk::ImageLayout::general;
		// barrier.srcAccessMask = vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite;
		// barrier.newLayout = lum_.dstScopeLight().layout;
		// barrier.dstAccessMask = lum_.dstScopeLight().access;
		// vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
		// 	lum_.dstScopeLight().stages, {}, {}, {}, {{barrier}});

		// barrier.oldLayout = barrier.newLayout;
		// barrier.srcAccessMask = barrier.dstAccessMask;
		// barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		// barrier.dstAccessMask = vk::AccessBits::shaderRead;
		// vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
		// 	vk::PipelineStageBits::fragmentShader | vk::PipelineStageBits::computeShader,
		// 	{}, {}, {}, {{barrier}});

		// compute bloom
		highlight_.record(cb, extent);
		tkn::barrier(cb, highlight_.target().image(),
			highlight_.srcScopeTarget(), bloom_.dstScopeHighlight());
		bloom_.record(cb, highlight_.target().image(), extent, blur_);

		// apply bloom to main image
		{
			auto applyBloomScope = tkn::SyncScope::computeSampled();
			auto applyLightScope = tkn::SyncScope::computeReadWrite();
			tkn::barrier(cb, highlight_.target().image(),
				bloom_.srcScopeHighlight(), applyBloomScope);
			tkn::barrier(cb, terrain_.color.image(),
				dstScopeLight, applyLightScope);

			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, applyPass_.pipe);
			tkn::cmdBindComputeDescriptors(cb, applyPass_.pipeLayout, 0u, {applyPass_.ds});
			auto cx = tkn::ceilDivide(width, applyGroupDimSize);
			auto cy = tkn::ceilDivide(height, applyGroupDimSize);
			vk::cmdDispatch(cb, cx, cy, 1u);

			// make sure it's readable for lum and post-process
			auto postScopeLight = tkn::SyncScope::fragmentSampled();
			postScopeLight.stages |= vk::PipelineStageBits::computeShader;
			tkn::barrier(cb, terrain_.color.image(), applyLightScope, postScopeLight);
		}

		// compute log-avg luminance
		lum_.record(cb, extent);

		// post process
		auto cvpp = std::array {
			vk::ClearValue{0.f, 0.f, 0.f, 1.f}, // color
		};
		vk::cmdBeginRenderPass(cb, {
			pp_.rp, rb.framebuffer,
			{0u, 0u, width, height},
			u32(cvpp.size()), cvpp.data()
		}, {});
		tkn::cmdBindGraphicsDescriptors(cb, pp_.pipeLayout, 0, {pp_.ds});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen quad
		vk::cmdEndRenderPass(cb);

		vk::endCommandBuffer(rb.commandBuffer);
	}

	void initBuffers(const vk::Extent2D& size, nytl::Span<RenderBuffer> buffers) override {
		auto& dev = vkDevice();
		auto devMem = dev.deviceMemoryTypes();

		// depth
		auto usage = vk::ImageUsageBits::depthStencilAttachment |
			vk::ImageUsageBits::sampled;
		auto info = vpp::ViewableImageCreateInfo(depthFormat_,
			vk::ImageAspectBits::depth, size, usage);
		terrain_.depth = {dev.devMemAllocator(), info, devMem};

		// color
		usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::sampled |
			vk::ImageUsageBits::storage;
		info = vpp::ViewableImageCreateInfo(colorFormat,
			vk::ImageAspectBits::color, size, usage);
		terrain_.color = {dev.devMemAllocator(), info, devMem};

		// fb
		auto atts0 = {terrain_.color.vkImageView(), terrain_.depth.vkImageView()};
		vk::FramebufferCreateInfo fbi;
		fbi.renderPass = terrain_.rp;
		fbi.width = size.width;
		fbi.height = size.height;
		fbi.layers = 1;
		fbi.attachmentCount = atts0.size();
		fbi.pAttachments = atts0.begin();
		terrain_.fb = {dev, fbi};

		auto wb = tkn::WorkBatcher(dev);
		highlight_.numLevels = bloomLevels;
		tkn::HighLightPass::InitBufferData initHighlight;
		highlight_.createBuffers(initHighlight, wb, size);

		tkn::BloomPass::InitBufferData initBloom;
		bloom_.createBuffers(initBloom, wb, bloomLevels, size, blur_);

		tkn::LuminancePass::InitBufferData initLum;
		lum_.createBuffers(initLum, wb, size);

		highlight_.initBuffers(initHighlight, terrain_.color.imageView());
		bloom_.initBuffers(initBloom, highlight_.target().image(),
			highlight_.target().imageView(), blur_);
		lum_.initBuffers(initLum, terrain_.color.imageView(), size);

		// update descriptors
		updateAtmosphereDs();

		vpp::DescriptorSetUpdate dsuApply(applyPass_.ds);
		dsuApply.uniform({{{applyPass_.ubo}}});
		dsuApply.imageSampler({{{}, highlight_.target().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuApply.storage({{{}, terrain_.color.imageView(),
			vk::ImageLayout::general}});

		vpp::DescriptorSetUpdate dsuPP(pp_.ds);
		dsuPP.imageSampler({{{}, terrain_.color.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuPP.uniform({{{pp_.ubo}}});

		vpp::apply({{{dsuPP}, {dsuApply}}});

		// render buffers for pp
		fbi.renderPass = pp_.rp;
		for(auto& buf : buffers) {
			auto attachments = {buf.imageView.vkHandle()};
			fbi.attachmentCount = attachments.size();
			fbi.pAttachments = attachments.begin();
			buf.framebuffer = {dev, fbi};
		}
	}

	void update(double dt) override {
		Base::update(dt);
		Base::scheduleRedraw();
		cam_.update(swaDisplay(), dt);

		/*
		float o;
		if(dstExposure_ > exposure_) {
			o = std::log2(0.001 / exposure_);
		} else {
			o = 10 * std::log2(exposure_ / 0.001);
		}

		float ispeed = 0.9 / (1 + std::abs(o));
		exposure_ += (1 - std::pow(ispeed, dt)) * (dstExposure_ - exposure_);
		dlg_info("exposure: {}", exposure_);
		*/
		highlight_.params.bias = -0.5 * (1 / exposure_);
		highlight_.params.scale = 1.5f;

		// check center tile
		if(cam_.needsUpdate) {
			auto nc = tilePos(cam_.position());
			if(nc != centerTile_ && !frozen_) {
				centerTile_ = nc;

				auto& qs = vkDevice().queueSubmitter();

				// TODO: this relies on ubo_ being updated in updateDevice
				// before the cb starts execution.
				// Probably not good (although it should always be the case atm)
				vk::SubmitInfo si;
				si.commandBufferCount = 1u;
				si.pCommandBuffers = &gen_.cb.vkHandle();
				si.signalSemaphoreCount = 1u;
				si.pSignalSemaphores = &gen_.sem.vkHandle();
				qs.add(si);

				Base::addSemaphore(gen_.sem, vk::PipelineStageBits::allCommands);
				dlg_info("update centerTile: {}", centerTile_);
			}
		}
	}

	void updateDevice() override {
		Base::updateDevice();

		if(cam_.needsUpdate) {
			cam_.needsUpdate = false;

			auto map = ubo_.memoryMap();
			auto span = map.span();
			auto vp = cam_.viewProjectionMatrix();

			TerrainUboData data;
			data.vp = vp;
			data.eye = cam_.position();
			data.enable = !frozen_;
			data.centerTile = centerTile_;
			data.atmosphere = atmosphere_.sky.config_.atmos;
			tkn::write(span, data);
			map.flush();

			// stars ubo
			map = stars_.ubo.memoryMap();
			span = map.span();
			tkn::write(span, cam_.fixedViewProjectionMatrix());
			map.flush();

			// atmosphere ubo
			AtmosphereUboData aud;
			aud.atmosphere = atmosphere_.sky.config_.atmos;
			aud.camAspect = cam_.aspect();
			aud.camDir = cam_.dir();
			aud.camNear = -cam_.near();
			aud.camFar = -cam_.far();
			aud.camFov = *cam_.perspectiveFov();
			aud.camUp = cam_.up();
			aud.toSun = toSun_;
			aud.viewPos = cam_.position();

			map = atmosphere_.ubo.memoryMap();
			span = map.span();
			tkn::write(span, aud);
			map.flush();
		}

		// update pp ubo
		{
			auto map = pp_.ubo.memoryMap();
			auto span = map.span();
			PostParams ppp;
			ppp.exposure = exposure_;
			tkn::write(span, ppp);
			map.flush();
		}

		// update apply ubo
		{
			auto map = applyPass_.ubo.memoryMap();
			auto span = map.span();
			ApplyUboData data;
			data.gain = 0.5f;
			data.strength = 1.f;
			data.nBloomLevels = bloomLevels;
			tkn::write(span, data);
			map.flush();
		}

		highlight_.updateDevice();
		auto gavgLum = lum_.updateDevice();

		// dlg_info("avgLum: {}", gavgLum);
		// solution of (tonemap(dstExposure * gavgLum) = 0.2)
		// dstExposure_ = std::clamp<float>(-std::log(0.8f) / gavgLum, 0.000001, 0.1f);
		// dlg_info("dstExposure: {}", dstExposure_);

		// TODO: more explicit regeneration...
		if(atmosphere_.sky.updateDevice()) {
			updateAtmosphereDs();
			Base::scheduleRerecord();
		}

		if(atmosphere_.sky.waitSemaphore) {
			Base::addSemaphore(atmosphere_.sky.waitSemaphore,
				vk::PipelineStageBits::allGraphics);
			atmosphere_.sky.waitSemaphore = {};
		}
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		cam_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		cam_.aspect({w, h});
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == swa_key_f) {
			cam_.needsUpdate = true; // TODO: hacky
			frozen_ = !frozen_;
			return true;
		} else if(atmosphere_.sky.key(ev.keycode)) {
			return true;
		}

		return false;
	}

	const char* name() const override { return "planet"; }

private:
	tkn::ControlledCamera cam_;
	vpp::ViewableImage heightmap_;
	vpp::Sampler sampler_;
	vpp::Sampler nearestSampler_;
	vk::Format depthFormat_;

	vpp::SubBuffer vertices_;
	vpp::SubBuffer indices_;

	vpp::SubBuffer ubo_;
	vpp::SubBuffer keys0_;
	vpp::SubBuffer keys1_; // temporary buffer we write updates to

	float dstExposure_ {0.0003f};
	float exposure_ {0.0002f};
	bool frozen_ {false};
	nytl::Vec3f toSun_ {0.f, 1.f, 0.f};

	// render pass: render terrain
	struct {
		vpp::RenderPass rp;
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;

		vpp::ViewableImage depth;
		vpp::ViewableImage color;
		vpp::Framebuffer fb;
	} terrain_;

	struct {
		vpp::SubBuffer ubo;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs dsCam;
		tkn::SkyboxRenderer renderer;
		vpp::TrDs dsMap;
		vpp::ViewableImage map;
	} stars_;

	// render pass: atmosphere
	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::SubBuffer ubo;
		BrunetonSky sky;
	} atmosphere_;

	// pass: apply bloom
	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::SubBuffer ubo;
	} applyPass_;

	// render pass: post-process/tonemap
	struct {
		vpp::RenderPass rp;
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
		vpp::SubBuffer ubo;
	} pp_;

	struct {
		struct {
			vpp::PipelineLayout pipeLayout;
			vpp::Pipeline pipe;
			vpp::TrDsLayout dsLayout;
			vpp::TrDs ds;
		} update;

		struct {
			vpp::TrDsLayout dsLayout;
			vpp::PipelineLayout pipeLayout;
			vpp::Pipeline pipe;
			vpp::TrDs ds;
		} indirect;

		vpp::SubBuffer dispatch; // indirect dispatch command
	} comp_;

	struct {
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::CommandBuffer cb;
		vpp::Semaphore sem;
	} gen_;

	tkn::GaussianBlur blur_;
	tkn::HighLightPass highlight_;
	tkn::BloomPass bloom_;
	tkn::LuminancePass lum_;

	nytl::Vec3u32 centerTile_ {10, 0, 0};
};

int main(int argc, const char** argv) {
	return tkn::appMain<PlanetApp>(argc, argv);
}

