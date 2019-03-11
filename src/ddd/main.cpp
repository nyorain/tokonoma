#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/bits.hpp>
#include <stage/gltf.hpp>
#include <stage/quaternion.hpp>

#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>
#include <ny/mouseButton.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/imageOps.hpp>

#include <dlg/dlg.hpp>
#include <nytl/mat.hpp>
#include <nytl/stringParam.hpp>

#include <tinygltf.hpp>

#include <shaders/model.vert.h>
#include <shaders/model.frag.h>
#include <shaders/shadowmap.vert.h>
#include <shaders/skybox.vert.h>
#include <shaders/skybox.frag.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma GCC diagnostic pop

// TODO: clean this mess up
// - seperate skybox file
// - seperate mesh file (make it a class)
// - seperate light file (make it a class, implement shadow there as well)
// - seperate camera file (make it a class)
// - don't create a pipeline for every object but instead e.g. have
//   a static method that initializes the pipeline

class Skybox {
public:
	void init(vpp::Device& dev, vk::RenderPass rp,
		vk::SampleCountBits samples);
	void updateDevice(const nytl::Mat4f& viewProj);
	void render(vk::CommandBuffer cb);

protected:
	vpp::Device* dev_;
	vpp::ViewableImage cubemap_;
	vpp::Sampler sampler_;
	vpp::SubBuffer indices_;
	vpp::SubBuffer ubo_;

	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

// TODO: allow loading panorama data
// https://stackoverflow.com/questions/29678510/convert-21-equirectangular-panorama-to-cube-map
// load cubemap

// XXX: the way vulkan handles cubemap samplers and image coorindates,
// top and bottom usually have to be rotated 180 degrees (or at least
// from the skybox set i tested with, see /assets/skyboxset1, those
// were rotated manually to fit)
void Skybox::init(vpp::Device& dev, vk::RenderPass rp,
		vk::SampleCountBits samples) {
	// ds layout
	auto bindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex),
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment),
	};

	dsLayout_ = {dev, bindings};
	pipeLayout_ = {dev, {dsLayout_}, {}};

	vpp::ShaderModule vertShader(dev, skybox_vert_data);
	vpp::ShaderModule fragShader(dev, skybox_frag_data);
	vpp::GraphicsPipelineInfo gpi {rp, pipeLayout_, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}, 0, samples};

	// depth test disabled by default
	gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
	// gpi.rasterization.cullMode = vk::CullModeBits::back;
	// gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(dev, {}, 1, gpi.info(), NULL, vkpipe);
	pipe_ = {dev, vkpipe};

	// indices
	indices_ = {dev.bufferAllocator(), 36u * sizeof(std::uint16_t),
		vk::BufferUsageBits::indexBuffer | vk::BufferUsageBits::transferDst,
		0, dev.deviceMemoryTypes()};
	std::array<std::uint16_t, 36> indices = {
		0, 1, 2,  2, 1, 3, // front
		1, 5, 3,  3, 5, 7, // right
		2, 3, 6,  6, 3, 7, // top
		4, 0, 6,  6, 0, 2, // left
		4, 5, 0,  0, 5, 1, // bottom
		5, 4, 7,  7, 4, 6, // back
	};
	vpp::writeStaging430(indices_, vpp::raw(*indices.data(), 36u));

	auto base = std::string("../assets/skyboxset1/SunSet/SunSet");
	// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
	const char* names[] = {
		"Right",
		"Left",
		"Up",
		"Down",
		"Back",
		"Front",
	};
	auto suffix = "2048.png";

	auto width = 0u;
	auto height = 0u;

	std::array<const std::byte*, 6> faceData {};

	for(auto i = 0u; i < 6u; ++i) {
		auto filename = base + names[i] + suffix;
		int iwidth, iheight, channels;
		unsigned char* data = stbi_load(filename.c_str(), &iwidth, &iheight,
			&channels, 4);
		if(!data) {
			dlg_warn("Failed to open texture file {}", filename);

			std::string err = "Could not load image from ";
			err += filename;
			err += ": ";
			err += stbi_failure_reason();
			throw std::runtime_error(err);
		}

		dlg_assert(iwidth > 0 && iheight > 0);
		if(width == 0 || height == 0) {
			width = iwidth;
			height = iheight;
			dlg_info("skybox: {} {}", width, height);
		} else {
			dlg_assert(int(width) == iwidth);
			dlg_assert(int(height) == iheight);
		}

		faceData[i] = reinterpret_cast<const std::byte*>(data);
		dlg_info("{}: {} ({})", i, (int*) faceData[i], (int) faceData[i][0]);
	}

	// free data
	nytl::ScopeGuard guard([&]{
		for(auto i = 0u; i < 6u; ++i) {
			std::free(const_cast<std::byte*>(faceData[i]));
		}
	});

	auto imgi = vpp::ViewableImageCreateInfo::color(
		dev, vk::Extent3D {width, height, 1u}).value();
	imgi.img.arrayLayers = 6u;
	imgi.img.flags = vk::ImageCreateBits::cubeCompatible;
	imgi.view.viewType = vk::ImageViewType::cube;
	imgi.view.subresourceRange.layerCount = 6u;
	cubemap_ = {dev, imgi};

	// buffer
	// XXX: this might get large
	auto totalSize = 6u * width * height * 4u;
	auto stage = vpp::SubBuffer(dev.bufferAllocator(), totalSize,
		vk::BufferUsageBits::transferSrc, 0u, dev.hostMemoryTypes());

	auto map = stage.memoryMap();
	auto span = map.span();
	auto offset = stage.offset();
	std::array<vk::BufferImageCopy, 6u> copies {};
	for(auto i = 0u; i < 6u; ++i) {
		copies[i] = {};
		copies[i].bufferOffset = offset;
		copies[i].imageExtent = {width, height, 1u};
		copies[i].imageOffset = {};
		copies[i].imageSubresource.aspectMask = vk::ImageAspectBits::color;
		copies[i].imageSubresource.baseArrayLayer = i;
		copies[i].imageSubresource.layerCount = 1u;
		copies[i].imageSubresource.mipLevel = 0u;
		offset += width * height * 4u;
		doi::write(span, faceData[i], width * height * 4u);
	}

	map = {};

	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());
	vk::beginCommandBuffer(cb, {});
	vpp::changeLayout(cb, cubemap_.image(), vk::ImageLayout::undefined,
		vk::PipelineStageBits::topOfPipe, {}, vk::ImageLayout::transferDstOptimal,
		vk::PipelineStageBits::transfer, vk::AccessBits::transferWrite,
		{vk::ImageAspectBits::color, 0, 1, 0, 6u});
	vk::cmdCopyBufferToImage(cb, stage.buffer(), cubemap_.image(),
		vk::ImageLayout::transferDstOptimal, copies);
	vpp::changeLayout(cb, cubemap_.image(), vk::ImageLayout::transferDstOptimal,
		vk::PipelineStageBits::transfer, vk::AccessBits::transferWrite,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::fragmentShader,
		vk::AccessBits::shaderRead,
		{vk::ImageAspectBits::color, 0, 1, 0, 6u});
	vk::endCommandBuffer(cb);

	vk::SubmitInfo si {};
	si.commandBufferCount = 1u;
	si.pCommandBuffers = &cb.vkHandle();
	qs.wait(qs.add(si));

	// ubo
	auto uboSize = sizeof(nytl::Mat4f);
	ubo_ = {dev.bufferAllocator(), uboSize, vk::BufferUsageBits::uniformBuffer,
		0u, dev.hostMemoryTypes()};

	// sampler
	vk::SamplerCreateInfo sci {};
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	sci.minLod = 0.0;
	sci.maxLod = 0.25;
	sci.mipmapMode = vk::SamplerMipmapMode::nearest;
	sampler_ = {dev, sci};

	// ds
	ds_ = {dev.descriptorAllocator(), dsLayout_};
	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
	dsu.imageSampler({{sampler_, cubemap_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.apply();
}

void Skybox::render(vk::CommandBuffer cb) {
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pipeLayout_, 0, {ds_}, {});
	vk::cmdBindIndexBuffer(cb, indices_.buffer(),
		indices_.offset(), vk::IndexType::uint16);
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
	vk::cmdDrawIndexed(cb, 36, 1, 0, 0, 0);
}

void Skybox::updateDevice(const nytl::Mat4f& viewProj) {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	doi::write(span, viewProj);
}

// Matches glsl struct
struct Light {
	enum class Type : std::uint32_t {
		point = 1u,
		dir = 2u,
	};

	nytl::Vec3f pd; // position/direction
	Type type;
	nytl::Vec3f color;
	std::uint32_t pcf {0};
};

struct Camera {
	bool update = true;
	nytl::Vec3f pos {0.f, 0.f, 3.f};
	nytl::Vec3f dir {0.f, 0.f, -1.f};
	nytl::Vec3f up {0.f, 1.f, 0.f};

	struct {
		float fov = 0.48 * nytl::constants::pi;
		float aspect = 1.f;
		float near = 0.01f;
		float far = 30.f;
	} perspective;
};

// camera matrix that does take movement into account
auto fixedMatrix(Camera& c) {
	auto& p = c.perspective;
	auto mat = doi::perspective3RH<float>(p.fov, p.aspect, p.near, p.far);
	return mat * doi::lookAtRH({}, c.dir, c.up);
}

auto matrix(Camera& c) {
	// auto mat = doi::ortho3Sym<float>(4.f, 4.f, 0.1f, 10.f);

	auto& p = c.perspective;
	auto mat = doi::perspective3RH<float>(p.fov, p.aspect, p.near, p.far);
	return mat * doi::lookAtRH(c.pos, c.pos + c.dir, c.up);
}

struct Mesh {
	static constexpr auto uboSize = 2 * sizeof(nytl::Mat4f);

	struct Vertex {
		nytl::Vec3f pos;
		nytl::Vec3f normal;
	};

	unsigned indexCount {};
	unsigned vertexCount {};
	vpp::SubBuffer vertices; // indices + vertices
	vpp::SubBuffer ubo; // different buffer since on mappable mem
	vpp::TrDs ds;
	nytl::Mat4f matrix = nytl::identity<4, float>();
};

std::optional<Mesh> loadMesh(const vpp::Device& dev,
		const tinygltf::Model& model, const tinygltf::Mesh& mesh,
		const vpp::TrDsLayout& dsLayout) {
	auto ret = Mesh {};

	if(mesh.primitives.empty()) {
		dlg_fatal("No primitive to render");
		return {};
	}

	auto& primitive = mesh.primitives[0];
	auto ip = primitive.attributes.find("POSITION");
	auto in = primitive.attributes.find("NORMAL");
	if(ip == primitive.attributes.end() || in == primitive.attributes.end()) {
		dlg_fatal("primitve doesn't have POSITION or NORMAL");
		return {};
	}

	auto& pa = model.accessors[ip->second];
	auto& na = model.accessors[in->second];
	auto& ia = model.accessors[primitive.indices];

	// compute total buffer size
	auto size = 0u;
	size += ia.count * sizeof(uint32_t); // indices
	size += na.count * sizeof(nytl::Vec3f); // normals
	size += pa.count * sizeof(nytl::Vec3f); // positions

	auto devMem = dev.deviceMemoryTypes();
	auto hostMem = dev.hostMemoryTypes();
	auto usage = vk::BufferUsageBits::vertexBuffer |
		vk::BufferUsageBits::indexBuffer |
		vk::BufferUsageBits::transferDst;

	dlg_assert(na.count == pa.count);
	ret.vertices = {dev.bufferAllocator(), size, usage, 0u, devMem};
	ret.indexCount = ia.count;
	ret.vertexCount = na.count;

	// fill it
	{
		auto stage = vpp::SubBuffer{dev.bufferAllocator(), size,
			vk::BufferUsageBits::transferSrc, 0u, hostMem};
		auto map = stage.memoryMap();
		auto span = map.span();

		// write indices
		for(auto idx : doi::range<1, std::uint32_t>(model, ia)) {
			doi::write(span, idx);
		}

		// write vertices and normals
		auto pr = doi::range<3, float>(model, pa);
		auto nr = doi::range<3, float>(model, na);
		for(auto pit = pr.begin(), nit = nr.begin();
				pit != pr.end() && nit != nr.end();
				++nit, ++pit) {
			doi::write(span, *pit);
			doi::write(span, *nit);
		}

		// upload
		auto& qs = dev.queueSubmitter();
		auto cb = qs.device().commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});
		vk::BufferCopy region;
		region.dstOffset = ret.vertices.offset();
		region.srcOffset = stage.offset();
		region.size = size;
		vk::cmdCopyBuffer(cb, stage.buffer(), ret.vertices.buffer(), {region});
		vk::endCommandBuffer(cb);

		// execute
		// TODO: could be batched with other work; we wait here
		vk::SubmitInfo submission;
		submission.commandBufferCount = 1;
		submission.pCommandBuffers = &cb.vkHandle();
		qs.wait(qs.add(submission));
	}

	// ubo
	size = Mesh::uboSize; // ubo
	usage = vk::BufferUsageBits::uniformBuffer;
	ret.ubo = {dev.bufferAllocator(), size, usage, 0u, hostMem};

	{
		auto map = ret.ubo.memoryMap();
		auto span = map.span();

		// write ubo
		doi::write(span, nytl::identity<4, float>()); // model matrix
		doi::write(span, nytl::identity<4, float>()); // normal matrix
	}

	// descriptor
	ret.ds = {dev.descriptorAllocator(), dsLayout};
	auto& mubo = ret.ubo;
	vpp::DescriptorSetUpdate odsu(ret.ds);
	odsu.uniform({{mubo.buffer(), mubo.offset(), mubo.size()}});
	vpp::apply({odsu});

	return ret;
}

void draw(vk::CommandBuffer cb, vk::PipelineLayout pipeLayout, const Mesh& mesh) {
	auto iOffset = mesh.vertices.offset();
	auto vOffset = iOffset + mesh.indexCount * sizeof(std::uint32_t);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pipeLayout, 1, {mesh.ds}, {});
	vk::cmdBindVertexBuffers(cb, 0, 1, {mesh.vertices.buffer()}, {vOffset});
	vk::cmdBindIndexBuffer(cb, mesh.vertices.buffer(), iOffset, vk::IndexType::uint32);
	vk::cmdDrawIndexed(cb, mesh.indexCount, 1, 0, 0, 0);
}

void updateDevice(Mesh& mesh) {
	auto map = mesh.ubo.memoryMap();
	auto span = map.span();
	doi::write(span, mesh.matrix);
	auto normalMatrix = nytl::Mat4f(transpose(inverse(mesh.matrix)));
	doi::write(span, normalMatrix);
}

class ViewApp : public doi::App {
public:
	static constexpr auto maxLightSize = 8u;
	using Vertex = Mesh::Vertex;

public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		// TODO: getting position right is hard
		// == example light ==
		lights_.emplace_back();
		lights_.back().color = {1.f, 1.f, 1.f};
		lights_.back().pd = {5.8f, 4.0f, 4.f};
		lights_.back().type = Light::Type::point;


		// === Init pipeline ===
		auto& dev = vulkanDevice();
		auto hostMem = dev.hostMemoryTypes();
		skybox_.init(dev, renderer().renderPass(), renderer().samples());

		// shadow sampler
		vk::SamplerCreateInfo sci {};
		sci.addressModeU = vk::SamplerAddressMode::clampToBorder;
		sci.addressModeV = vk::SamplerAddressMode::clampToBorder;
		sci.addressModeW = vk::SamplerAddressMode::clampToBorder;
		sci.borderColor = vk::BorderColor::floatOpaqueWhite;
		sci.magFilter = vk::Filter::linear;
		sci.minFilter = vk::Filter::linear;
		sci.minLod = 0.0;
		sci.maxLod = 0.25;
		sci.mipmapMode = vk::SamplerMipmapMode::nearest;
		sci.compareEnable = true;
		sci.compareOp = vk::CompareOp::lessOrEqual;
		shadow_.sampler = vpp::Sampler(dev, sci);

		// per scense; view + projection matrix, lights
		auto sceneBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment),
		};

		sceneDsLayout_ = {dev, sceneBindings};

		// per object; model matrix and material stuff
		auto objectBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		objectDsLayout_ = {dev, objectBindings};

		pipeLayout_ = {dev, {sceneDsLayout_, objectDsLayout_}, {}};

		vk::SpecializationMapEntry maxLightsEntry;
		maxLightsEntry.size = sizeof(std::uint32_t);

		vk::SpecializationInfo fragSpec;
		fragSpec.dataSize = sizeof(std::uint32_t);
		fragSpec.pData = &maxLightSize;
		fragSpec.mapEntryCount = 1;
		fragSpec.pMapEntries = &maxLightsEntry;

		vpp::ShaderModule vertShader(dev, model_vert_data);
		vpp::ShaderModule fragShader(dev, model_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderer().renderPass(), pipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment, &fragSpec},
		}}, 0, renderer().samples()};

		constexpr auto stride = sizeof(Vertex);
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::vertex
		};

		vk::VertexInputAttributeDescription attributes[2];
		attributes[0].format = vk::Format::r32g32b32Sfloat; // pos

		attributes[1].format = vk::Format::r32g32b32Sfloat; // normal
		attributes[1].offset = sizeof(float) * 3; // pos
		attributes[1].location = 1;

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 2u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		gpi.rasterization.cullMode = vk::CullModeBits::back;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		pipe_ = {dev, vkpipe};

		// Load Model
		const auto filename = "../assets/gltf/test3.gltf";
		if (!loadModel(filename)) {
			return false;
		}

		// == ubo and stuff ==
		auto sceneUboSize = 2 * sizeof(nytl::Mat4f) // light; proj matrix
			+ maxLightSize * sizeof(Light) // lights
			+ sizeof(nytl::Vec3f) // viewPos
			+ sizeof(std::uint32_t); // numLights
		sceneDs_ = {dev.descriptorAllocator(), sceneDsLayout_};
		sceneUbo_ = {dev.bufferAllocator(), sceneUboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		initShadowPipe();

		// descriptors
		vpp::DescriptorSetUpdate sdsu(sceneDs_);
		sdsu.uniform({{sceneUbo_.buffer(), sceneUbo_.offset(), sceneUbo_.size()}});
		sdsu.imageSampler({{shadow_.sampler, shadow_.target.vkImageView(),
			vk::ImageLayout::depthStencilReadOnlyOptimal}});
		vpp::apply({sdsu});

		return true;
	}

	bool loadModel(nytl::StringParam filename) {
		dlg_info(">> Loading model...");
		namespace gltf = tinygltf;

		gltf::TinyGLTF loader;
		gltf::Model model;
		std::string err;
		std::string warn;
		auto res = loader.LoadASCIIFromFile(&model, &err, &warn, filename.c_str());

		// error, warnings
		auto pos = 0u;
		auto end = warn.npos;
		while((end = warn.find_first_of('\n', pos)) != warn.npos) {
			auto d = warn.data() + pos;
			dlg_warn("  {}", std::string_view{d, end - pos});
		}

		pos = 0u;
		while((end = err.find_first_of('\n', pos + 1)) != err.npos) {
			auto d = err.data() + pos;
			dlg_error("  {}", std::string_view{d, end - pos});
		}

		if(!res) {
			dlg_fatal(">> Failed to load model");
			return false;
		}

		dlg_info(">> Loading Succesful...");

		// if(model.meshes.empty()) {
		// 	dlg_fatal("No mesh to render");
		// 	return false;
		// }
		//
		// dlg_info("Found {} meshes", model.meshes.size());
		// simple mesh loading/rendering
		// for(auto& mesh : model.meshes) {
		// 	auto m = loadMesh(vulkanDevice(), model, mesh, objectDsLayout_);
		// 	if(!m) {
		// 		return false;
		// 	}
		// 	meshes_.push_back(std::move(*m));
		// }

		// traverse nodes
		dlg_info("Found {} scenes", model.scenes.size());
		auto& scene = model.scenes[model.defaultScene];

		auto mat = nytl::identity<4, float>();
		for(auto nodeid : scene.nodes) {
			auto& node = model.nodes[nodeid];
			loadNode(model, node, mat);
		}

		return true;
	}

	void loadNode(const tinygltf::Model& model,
			const tinygltf::Node& node,
			nytl::Mat4f matrix) {
		if(!node.matrix.empty()) {
			nytl::Mat4f mat;
			for(auto r = 0u; r < 4; ++r) {
				for(auto c = 0u; c < 4; ++c) {
					// they are column major
					mat[r][c] = node.matrix[c * 4 + r];
				}
			}

			dlg_info(mat);
			matrix = matrix * mat;
		} else if(!node.scale.empty() || !node.translation.empty() || !node.rotation.empty()) {
			// gltf: matrix computed as T * R * S
			auto mat = nytl::identity<4, float>();
			if(!node.scale.empty()) {
				mat[0][0] = node.scale[0];
				mat[1][1] = node.scale[1];
				mat[2][2] = node.scale[2];
			}

			if(!node.rotation.empty()) {
				doi::Quaternion q;
				q.x = node.rotation[0];
				q.y = node.rotation[1];
				q.z = node.rotation[2];
				q.w = node.rotation[3];

				mat = doi::toMat<4>(q) * mat;
			}

			if(!node.translation.empty()) {
				nytl::Vec3f t;
				t.x = node.translation[0];
				t.y = node.translation[1];
				t.z = node.translation[2];
				mat = doi::translateMat(t) * mat;
			}

			dlg_info(mat);
			matrix = matrix * mat;
		}

		for(auto nodeid : node.children) {
			auto& child = model.nodes[nodeid];
			loadNode(model, child, matrix);
		}

		if(node.mesh != -1) {
			auto& mesh = model.meshes[node.mesh];
			auto m = loadMesh(vulkanDevice(), model, mesh, objectDsLayout_);
			if(!m) {
				dlg_error("could not load mesh");
				return;
			}

			meshes_.push_back(std::move(*m));
			meshes_.back().matrix = matrix;
		}
	}

	void initShadowPipe() {
		auto& dev = vulkanDevice();
		auto hostMem = dev.hostMemoryTypes();

		// renderpass
		vk::AttachmentDescription depth {};
		depth.initialLayout = vk::ImageLayout::undefined;
		depth.finalLayout = vk::ImageLayout::depthStencilReadOnlyOptimal;
		depth.format = shadow_.format;
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

		shadow_.rp = {dev, rpi};

		// target
		auto targetUsage = vk::ImageUsageBits::depthStencilAttachment |
			vk::ImageUsageBits::sampled;
		auto targetInfo = vpp::ViewableImageCreateInfo::general(dev,
			shadow_.extent, targetUsage, {shadow_.format},
			vk::ImageAspectBits::depth);
		shadow_.target = {dev, *targetInfo};

		// framebuffer
		vk::FramebufferCreateInfo fbi {};
		fbi.attachmentCount = 1;
		fbi.width = shadow_.extent.width;
		fbi.height = shadow_.extent.height;
		fbi.layers = 1u;
		fbi.pAttachments = &shadow_.target.vkImageView();
		fbi.renderPass = shadow_.rp;
		shadow_.fb = {dev, fbi};

		// layouts
		// XXX: using other layouts for now
		/*
		auto bindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex), // view/projection
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex) // model
		};

		shadow_.dsLayout = {dev, bindings};
		shadow_.pipeLayout = {dev, {shadow_.dsLayout}, {}};
		*/

		vpp::ShaderModule vertShader(dev, shadowmap_vert_data);
		vpp::GraphicsPipelineInfo gpi {shadow_.rp, pipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
		}}, 0, vk::SampleCountBits::e1};

		constexpr auto stride = sizeof(Vertex);
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::vertex
		};

		vk::VertexInputAttributeDescription attributes[1];
		attributes[0].format = vk::Format::r32g32b32Sfloat; // pos

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 1u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;

		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
		gpi.rasterization.cullMode = vk::CullModeBits::back;
		gpi.rasterization.depthBiasEnable = true;

		auto dynamicStates = {
			vk::DynamicState::depthBias,
			vk::DynamicState::viewport,
			vk::DynamicState::scissor
		};
		gpi.dynamic.pDynamicStates = dynamicStates.begin();
		gpi.dynamic.dynamicStateCount = 3;

		gpi.blend.attachmentCount = 0;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		shadow_.pipeline = {dev, vkpipe};

		// setup light ds and ubo
		auto lightUboSize = sizeof(nytl::Mat4f); // projection, view
		shadow_.ds = {dev.descriptorAllocator(), sceneDsLayout_};
		shadow_.ubo = {dev.bufferAllocator(), lightUboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		auto& lubo = shadow_.ubo;
		vpp::DescriptorSetUpdate ldsu(shadow_.ds);
		ldsu.uniform({{lubo.buffer(), lubo.offset(), lubo.size()}});
		vpp::apply({ldsu});

		// fill ubo once
		{
			auto map = shadow_.ubo.memoryMap();
			auto span = map.span();
			doi::write(span, lightMatrix());
		}
	}

	nytl::Mat4f lightMatrix() {
		auto& light = lights_[0];
		auto mat = doi::ortho3Sym(20.f, 20.f, 0.5f, 20.f);
		// auto mat = doi::perspective3RH<float>(0.25 * nytl::constants::pi, 1.f, 0.1, 5.f);
		mat = mat * doi::lookAtRH(light.pd,
			{0.f, 0.f, 0.f}, // always looks at center
			{0.f, 1.f, 0.f});
		return mat;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);

		auto width = shadow_.extent.width;
		auto height = shadow_.extent.height;
		vk::ClearValue clearValue {};
		clearValue.depthStencil = {1.f, 0u};

		// draw shadow map!
		vk::RenderPassBeginInfo rpb; // TODO
		vk::cmdBeginRenderPass(cb, {
			shadow_.rp,
			shadow_.fb,
			{0u, 0u, width, height},
			1,
			&clearValue
		}, {});

		vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});
		vk::cmdSetDepthBias(cb, 1.25, 0.f, 1.75);

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, shadow_.pipeline);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {shadow_.ds}, {});

		for(auto& mesh : meshes_) {
			draw(cb, pipeLayout_, mesh);
		}

		vk::cmdEndRenderPass(cb);
	}

	void render(vk::CommandBuffer cb) override {
		skybox_.render(cb);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {sceneDs_}, {});
		for(auto& mesh : meshes_) {
			draw(cb, pipeLayout_, mesh);
		}
	}

	void update(double dt) override {
		App::update(dt);
		App::redraw();
		time_ += dt;

		// movement
		auto kc = appContext().keyboardContext();
		auto fac = dt;

		auto yUp = nytl::Vec3f {0.f, 1.f, 0.f};
		auto right = nytl::normalized(nytl::cross(camera_.dir, yUp));
		auto up = nytl::normalized(nytl::cross(camera_.dir, right));
		if(kc->pressed(ny::Keycode::d)) { // right
			camera_.pos += fac * right;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::a)) { // left
			camera_.pos += -fac * right;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::w)) {
			camera_.pos += fac * camera_.dir;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::s)) {
			camera_.pos += -fac * camera_.dir;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::q)) { // up
			camera_.pos += -fac * up;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::e)) { // down
			camera_.pos += fac * up;
			camera_.update = true;
		}

		if(moveLight_) {
			lights_[0].pd.x = 10.0 * std::cos(0.2 * time_);
			lights_[0].pd.z = 10.0 * std::sin(0.2 * time_);
			updateLights_ = true;
		}
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		switch(ev.keycode) {
			case ny::Keycode::l:
				moveLight_ ^= true;
				return true;
			case ny::Keycode::p:
				lights_[0].pcf = 1 - lights_[0].pcf;
				updateLights_ = true;
				return true;
			default:
				break;
		}

		return false;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			using nytl::constants::pi;
			yaw_ += 0.005 * ev.delta.x;
			pitch_ += 0.005 * ev.delta.y;
			pitch_ = std::clamp<float>(pitch_, -pi / 2 + 0.1, pi / 2 - 0.1);

			camera_.dir.x = std::sin(yaw_) * std::cos(pitch_);
			camera_.dir.y = -std::sin(pitch_);
			camera_.dir.z = -std::cos(yaw_) * std::cos(pitch_);
			nytl::normalize(camera_.dir);
			camera_.update = true;
		}
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button == ny::MouseButton::left) {
			rotateView_ = ev.pressed;
			return true;
		}

		return false;
	}

	void updateDevice() override {
		// update scene ubo
		if(camera_.update || updateLights_) {
			camera_.update = false;
			// updateLights_ set to false below

			auto map = sceneUbo_.memoryMap();
			auto span = map.span();

			doi::write(span, matrix(camera_));
			doi::write(span, lightMatrix());

			{ // lights
				auto lspan = span;
				for(auto& l : lights_) {
					doi::write(lspan, l);
				}
			}

			doi::skip(span, sizeof(Light) * maxLightSize);
			doi::write(span, camera_.pos);
			doi::write(span, std::uint32_t(lights_.size()));

			skybox_.updateDevice(fixedMatrix(camera_));
		}

		if(updateLights_) {
			updateLights_ = false;
			auto map = shadow_.ubo.memoryMap();
			auto span = map.span();
			doi::write(span, lightMatrix());
		}

		// update model matrix
		for(auto& m : meshes_) {
			::updateDevice(m);
		}
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	bool needsDepth() const override {
		return true;
	}

protected:
	vpp::SubBuffer sceneUbo_;
	vpp::TrDsLayout sceneDsLayout_;
	vpp::TrDsLayout objectDsLayout_;
	vpp::TrDs sceneDs_;

	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;

	bool moveLight_ {false};

	float time_ {};
	bool rotateView_ {false}; // mouseLeft down

	std::vector<Mesh> meshes_;
	std::vector<Light> lights_;
	bool updateLights_ {true};

	Camera camera_ {};
	// XXX: integrate into camera class
	float yaw_ {0.f};
	float pitch_ {0.f}; // rotation around x (left/right) axis

	// shadow
	struct {
		// static
		vpp::RenderPass rp;
		vpp::Sampler sampler; // with compareOp (?) glsl: sampler2DShadow
		// vpp::TrDsLayout sceneDsLayout;
		// vpp::TrDsLayout objectDsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipeline;
		vk::Format format = vk::Format::d32Sfloat; // TODO

		vk::Extent3D extent {2048u, 2048u, 1u};

		// per light
		vpp::ViewableImage target;
		vpp::Framebuffer fb;
		vpp::TrDs ds;
		vpp::SubBuffer ubo; // holding the light view matrix
	} shadow_;

	Skybox skybox_;
};

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({"3D View", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
