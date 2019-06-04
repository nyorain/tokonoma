#include <stage/scene/scene.hpp>
#include <stage/scene/primitive.hpp>
#include <stage/scene/material.hpp>
#include <stage/quaternion.hpp>
#include <stage/image.hpp>
#include <stage/bits.hpp>
#include <stage/transform.hpp>
#include <stage/texture.hpp>
#include <stage/util.hpp>
#include <vpp/vk.hpp>
#include <vpp/debug.hpp>
#include <vpp/image.hpp>
#include <dlg/dlg.hpp>

namespace doi {
namespace {

doi::Texture loadImage(Texture::InitData& data,
		const WorkBatcher& wb, const gltf::Image& tex,
		nytl::StringParam path, bool srgb) {
	auto name = tex.name.empty() ?  tex.name : "'" + tex.name + "'";
	dlg_info("  Loading image {}", name);
	auto params = TextureCreateParams {};
	params.srgb = srgb;
	params.format = srgb ?
		vk::Format::r8g8b8a8Srgb :
		vk::Format::r8g8b8a8Unorm;
	// full mipmap chain
	params.mipLevels = 0;
	params.fillMipmaps = true;

	// TODO: we could support additional formats like r8 or r8g8.
	// check tex.pixel_type. Also support other image parameters
	// TODO: we currently don't support hdr images. Check the
	// specified image format
	if(!tex.uri.empty()) {
		auto full = std::string(path);
		full += tex.uri;
		return {data, wb, read(full), params};
	}

	// TODO: simplifying assumptions that are usually met
	dlg_assert(tex.component == 4);
	dlg_assert(!tex.as_is);

	// TODO: we only have to copy the image data here in case
	// the gltf model is destroyed before the image finished initializtion...
	Image img;
	img.size.x = tex.width;
	img.size.y = tex.height;
	img.format = srgb ? vk::Format::r8g8b8a8Srgb : vk::Format::r8g8b8a8Unorm;

	auto dataSize = tex.width * tex.height * 4u;
	img.data = std::make_unique<std::byte[]>(dataSize);
	std::memcpy(img.data.get(), tex.image.data(), dataSize);

	return {data, wb, wrap(std::move(img)), params};
}

} // anon namespace

Scene::Scene(InitData& data, const WorkBatcher& wb, nytl::StringParam path,
		const tinygltf::Model& model, const tinygltf::Scene& scene,
		nytl::Mat4f matrix, const SceneRenderInfo& ri) {
	auto& dev = wb.dev;

	if(model.images.size() > imageCount) {
		throw std::runtime_error("Can't support that many images");
	}
	if(model.samplers.size() > samplerCount) {
		throw std::runtime_error("Can't support that many samplers");
	}

	// layout
	auto bindings = {
		// model ids
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
			vk::ShaderStageBits::vertex),
		// models
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
			vk::ShaderStageBits::vertex),
		// materials
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
			vk::ShaderStageBits::fragment),
		// textures[32]
		vpp::descriptorBinding(vk::DescriptorType::sampledImage,
			vk::ShaderStageBits::fragment, -1, 32),
		// samplers[8]
		vpp::descriptorBinding(vk::DescriptorType::sampler,
			vk::ShaderStageBits::fragment, -1, 8),
	};

	dsLayout_ = {dev, bindings};
	vpp::nameHandle(dsLayout_, "Scene:dsLayout");
	ds_ = {data.initDs, wb.alloc.ds, dsLayout_};

	// load samplers
	// TODO: optimization, low prio
	// check for duplicate samplers. But then also change how materials
	// access samplers, can't happen simply by id anymore.
	for(auto& sampler : model.samplers) {
		samplers_.emplace_back(dev, sampler, ri.samplerAnisotropy);
	}

	// init default sampler as specified in gltf
	vk::SamplerCreateInfo sci;
	sci.addressModeU = vk::SamplerAddressMode::repeat;
	sci.addressModeV = vk::SamplerAddressMode::repeat;
	sci.addressModeW = vk::SamplerAddressMode::repeat;
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	sci.mipmapMode = vk::SamplerMipmapMode::linear;
	sci.minLod = 0.0;
	sci.maxLod = 100.f; // use all mipmap levels
	sci.anisotropyEnable = ri.samplerAnisotropy != 1.f;
	sci.maxAnisotropy = ri.samplerAnisotropy;
	defaultSampler_ = {dev, sci};

	// load materials
	// we load images later on because we first need to know where
	// in materials they are used to know whether they contain srgb
	// or linear data
	images_.resize(model.images.size());

	data.materials.reserve(model.materials.size());
	dlg_info("Found {} materials", model.materials.size());
	for(auto& material : model.materials) {
		auto name = material.name.empty() ?
			material.name :
			"'" + material.name + "'";
		dlg_info("  Loading material {}", name);
		auto& d = data.materials.emplace_back();
		auto m = Material(d, model, material, ri.materialDsLayout,
			ri.dummyTex, *this);
		materials_.push_back(std::move(m));
	}

	// we need at least one material (see primitive creation)
	// if there is none, add dummy
	defaultMaterialID_ = materials_.size();
	materials_.emplace_back(ri.materialDsLayout, ri.dummyTex,
		defaultSampler_);

	// initialize images
	data.images.resize(model.images.size());
	for(auto i = 0u; i < model.images.size(); ++i) {
		auto& d = data.images[i];
		auto& img = images_[i];
		dlg_assertm(img.needed, "Model has unused image");
		images_[i].image = loadImage(d, wb, model.images[i], path, img.srgb);
	}

	auto inf = std::numeric_limits<float>::infinity();
	min_ = {inf, inf, inf};
	max_ = {-inf, -inf, -inf};

	// load nodes tree recursively
	for(auto& nodeid : scene.nodes) {
		dlg_assert(unsigned(nodeid) < model.nodes.size());
		auto& node = model.nodes[nodeid];
		loadNode(data, wb, model, node, ri, matrix);
	}
	auto blendCount = 0u;
	auto opaqueCount = 0u;
	for(auto& p : primitives_) {
		if(materials_[p.material()].blend()) {
			++blendCount;
		} else {
			++opaqueCount;
		}
	}

	// TODO: some of the below better on dev memory i guess?
	auto hostMem = dev.hostMemoryTypes();
	auto devMem = dev.deviceMemoryTypes();

	// models buffer
	auto pcount = blendCount + opaqueCount;
	auto size = pcount * sizeof(nytl::Mat4f) * 2;
	modelsBuf_ = {data.initModels, wb.alloc.bufHost, size,
		vk::BufferUsageBits::storageBuffer, hostMem};

	// materials buffer
	size = materials_.size() * sizeof(Material::Buffer);
	materialsBuf_ = {data.initModels, wb.alloc.bufDevice, size,
		vk::BufferUsageBits::storageBuffer |
		vk::BufferUsageBits::transferDst, devMem};
	data.materialsStage = {data.initMaterialsStage, wb.alloc.bufStage,
		size, vk::BufferUsageBits::transferSrc, hostMem};

	// cmds buffer
	size = opaqueCount * sizeof(vk::DrawIndexedIndirectCommand);
	cmds_ = {data.initCmds, wb.alloc.bufHost, size,
		vk::BufferUsageBits::indirectBuffer, hostMem};
	size = opaqueCount * sizeof(u32);
	modelIDs_ = {data.initModelIDs, wb.alloc.bufHost, size,
		vk::BufferUsageBits::storageTexelBuffer, hostMem};

	size = blendCount * sizeof(vk::DrawIndexedIndirectCommand);
	blendCmds_ = {data.initBlendCmds, wb.alloc.bufHost, size,
		vk::BufferUsageBits::indirectBuffer, hostMem};
	size = blendCount * sizeof(u32);
	blendModelIDs_ = {data.initBlendModelIDs, wb.alloc.bufHost, size,
		vk::BufferUsageBits::storageTexelBuffer, hostMem};
}

void Scene::loadNode(InitData& data, const WorkBatcher& wb,
		const tinygltf::Model& model, const tinygltf::Node& node,
		const SceneRenderInfo& ri, nytl::Mat4f matrix) {
	if(!node.matrix.empty()) {
		nytl::Mat4f mat;
		for(auto r = 0u; r < 4; ++r) {
			for(auto c = 0u; c < 4; ++c) {
				// node.matrix is column major
				mat[r][c] = node.matrix[c * 4 + r];
			}
		}

		matrix = matrix * mat;
	} else if(!node.scale.empty() ||
			!node.translation.empty() ||
			!node.rotation.empty()) {
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

		matrix = matrix * mat;
	}

	for(auto nodeid : node.children) {
		auto& child = model.nodes[nodeid];
		loadNode(data, wb, model, child, ri, matrix);
	}

	if(node.mesh != -1) {
		auto& mesh = model.meshes[node.mesh];
		auto name = mesh.name.empty() ?  mesh.name : "'" + mesh.name + "'";
		dlg_info("  Loading mesh {}", name);
		for(auto& primitive : mesh.primitives) {
			auto mat = primitive.material;
			if(mat < 0) {
				// use default material
				mat = defaultMaterialID_;
			}

			auto id = primitives_.size() + 1;
			auto& d = data.primitives.emplace_back();
			auto p = Primitive(d, wb, model, primitive,
				ri.primitiveDsLayout, mat, matrix, id);
			min_ = nytl::vec::cw::min(min_, multPos(matrix, p.min()));
			max_ = nytl::vec::cw::max(max_, multPos(matrix, p.max()));

			primitives_.emplace_back(std::move(p));
		}
	}
}

void Scene::init(InitData& data, const WorkBatcher& wb, vk::ImageView dummyView) {
	dlg_assert(images_.size() == data.images.size());
	dlg_assert(materials_.size() == data.materials.size() + 1); // + default mat
	dlg_assert(primitives_.size() == data.primitives.size());

	data.materialsStage.init(data.initMaterialsStage);
	materialsBuf_.init(data.initMaterials);
	modelsBuf_.init(data.initModels);
	cmds_.init(data.initCmds);
	modelIDs_.init(data.initModelIDs);
	blendCmds_.init(data.initBlendCmds);
	blendModelIDs_.init(data.initBlendModelIDs);

	ds_.init(data.initDs);

	std::array<vk::ImageView, imageCount> dsImages;
	for(auto i = 0u; i < data.images.size(); ++i) {
		if(i < data.images.size()) {
			images_[i].image.init(data.images[i], wb);
			dsImages[i] = images_[i].image.vkImageView();
		} else {
			dsImages[i] = dummyView;
		}
	}

	auto matMap = data.materialsStage.memoryMap();
	auto matSpan = matMap.span();
	for(auto i = 0u; i < data.materials.size(); ++i) {
		materials_[i].init(data.materials[i], *this);
		doi::write(matSpan, materials_[i].buf());
	}
	matMap.flush();
	matMap = {};

	vk::BufferCopy copy;
	copy.srcOffset = data.materialsStage.offset();
	copy.dstOffset = materialsBuf_.offset();
	copy.size = materialsBuf_.size();
	vk::cmdCopyBuffer(wb.cb, data.materialsStage.buffer(),
		materialsBuf_.buffer(), {{copy}});

	for(auto i = 0u; i < data.primitives.size(); ++i) {
		auto& p = primitives_[i];
		auto& material = materials_[p.material()];
		p.init(data.primitives[i], wb.cb);
		if(!p.hasTexCoords0() && material.needsTexCoords0()) {
			throw std::runtime_error("material uses texCoords0 but primitive "
				"doesn't provide them");
		}
		if(!p.hasTexCoords1() && material.needsTexCoords1()) {
			throw std::runtime_error("material uses texCoords1 but primitive "
				"doesn't provide them");
		}
	}

	// TODO: should be done in updateDevice/at runtime; support culling
	auto idmap = modelIDs_.memoryMap();
	auto idspan = idmap.span();
	auto cmdmap = cmds_.memoryMap();
	auto cmdspan = cmdmap.span();
	for(auto i = 0u; i < primitives_.size(); ++i) {
		auto& p = primitives_[i];
		if(!materials_[p.material()].blend()) {
			doi::write(idspan, u32(opaqueCount_));
			++opaqueCount_;

			vk::DrawIndexedIndirectCommand cmd;
			cmd.indexCount = p.indexCount();
			cmd.instanceCount = 1;
		}
	}
	idmap.flush();
}

void Scene::createImage(unsigned id, bool srgb) {
	dlg_assert(id < images_.size());
	if(images_[id].needed) {
		dlg_assert(images_[id].srgb == srgb);
	}

	images_[id].srgb = srgb;
	images_[id].needed = true;
}

void Scene::updateDevice(nytl::Vec3f viewPos) {
	// TODO: whole method rather inefficient atm
	std::vector<const Primitive*> blendPrims;
	for(auto& p : primitives_) {
		if(materials_[p.material()].blend()) {
			blendPrims.push_back(&p);
		}
	}

	auto cmp = [&](const Primitive* p1, const Primitive* p2) {
		auto c1 = 0.5f * (p1->min() + p1->max());
		auto c2 = 0.5f * (p2->min() + p2->max());
		return length(c1 - viewPos) < length(c2 - viewPos);
	};
}

void Scene::render(vk::CommandBuffer cb, vk::PipelineLayout pl) const {
	for(auto& p : primitives_) {
		auto& material = materials_[p.material()];
		material.bind(cb, pl);
		p.render(cb, pl);
	}
}

void Scene::renderOpaque(vk::CommandBuffer cb, vk::PipelineLayout pl) const {
	for(auto& p : primitives_) {
		auto& material = materials_[p.material()];
		// don't render blend-transparent primitives (materials)
		if(material.blend()) {
			continue;
		}

		material.bind(cb, pl);
		p.render(cb, pl);
	}
}

void Scene::renderBlend(vk::CommandBuffer cb, vk::PipelineLayout pl) const {
	for(auto& p : primitives_) {
		auto& material = materials_[p.material()];
		// only render blend-transparent primitives (materials)
		if(!material.blend()) {
			continue;
		}

		material.bind(cb, pl);
		p.render(cb, pl);
	}
}

// util
std::tuple<std::optional<gltf::Model>, std::string> loadGltf(nytl::StringParam at) {

	// Load Model
	// fallback
	std::string path = "../assets/gltf/";
	std::string file = "test3.gltf";
	bool binary = false;
	if(!at.empty()) {
		if(has_suffix(at, ".gltf")) {
			auto i = at.find_last_of('/');
			if(i == std::string::npos) {
				path = {};
				file = at;
			} else {
				path = at.substr(0, i + 1);
				file = at.substr(i + 1);
			}
		} else if(has_suffix(at, ".gltb") || has_suffix(at, ".glb")) {
			binary = true;
			auto i = at.find_last_of('/');
			if(i == std::string::npos) {
				path = {};
				file = at;
			} else {
				path = at.substr(0, i + 1);
				file = at.substr(i + 1);
			}
		} else {
			path = at;
			if(path.back() != '/') {
				path.push_back('/');
			}
			if(tinygltf::FileExists(path + "scene.gltf", nullptr)) {
				file = "scene.gltf";
			} else if(tinygltf::FileExists(path + "scene.gltb", nullptr)) {
				binary = true;
				file = "scene.gltb";
			} else {
				dlg_fatal("Given folder doesn't have scene.gltf/gltb");
				return {};
			}
		}
	}

	dlg_info(">> Parsing gltf model...");

	gltf::TinyGLTF loader;
	gltf::Model model;
	std::string err;
	std::string warn;

	auto full = std::string(path);
	full += file;
	bool res;
	if(binary) {
		res = loader.LoadBinaryFromFile(&model, &err, &warn, full.c_str());
	} else {
		res = loader.LoadASCIIFromFile(&model, &err, &warn, full.c_str());
	}

	// error, warnings
	auto pos = 0u;
	auto end = warn.npos;
	while((end = warn.find_first_of('\n', pos)) != warn.npos) {
		auto d = warn.data() + pos;
		dlg_warn("  {}", std::string_view{d, end - pos});
		pos = end + 1;
	}

	pos = 0u;
	while((end = err.find_first_of('\n', pos)) != err.npos) {
		auto d = err.data() + pos;
		dlg_error("  {}", std::string_view{d, end - pos});
		pos = end + 1;
	}

	if(!res) {
		dlg_fatal(">> Failed to parse model");
		return {};
	}

	dlg_info(">> Parsing Succesful...");

	// traverse nodes
	if(model.scenes.empty()) {
		dlg_fatal(">> Model has no scenes");
		return {};
	}

	return {model, path};
}

// Sampler
SamplerInfo::SamplerInfo(const gltf::Sampler& sampler) {
	// minFilter
	mipmapMode = vk::SamplerMipmapMode::linear;
	switch(sampler.minFilter) {
		case TINYGLTF_TEXTURE_FILTER_LINEAR:
			minFilter = vk::Filter::linear;
			break;
		case TINYGLTF_TEXTURE_FILTER_NEAREST:
			minFilter = vk::Filter::linear;
			break;
		case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR:
			minFilter = vk::Filter::linear;
			mipmapMode = vk::SamplerMipmapMode::linear;
			break;
		case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST:
			minFilter = vk::Filter::nearest;
			mipmapMode = vk::SamplerMipmapMode::linear;
			break;
		case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR:
			minFilter = vk::Filter::linear;
			mipmapMode = vk::SamplerMipmapMode::nearest;
			break;
		default:
			minFilter = vk::Filter::linear;
			mipmapMode = vk::SamplerMipmapMode::linear;
			dlg_warn("Unknown gltf sampler.minFilter {}", sampler.minFilter);
			break;
	}

	// magFilter
	if(sampler.magFilter == TINYGLTF_TEXTURE_FILTER_LINEAR) {
		magFilter = vk::Filter::linear;
	} else if(sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST) {
		magFilter = vk::Filter::nearest;
	} else {
		magFilter = vk::Filter::linear;
		dlg_warn("Unknown gltf sampler.magFilter {}", sampler.magFilter);
	}

	auto translateAddressMode = [&](auto mode) {
		if(mode == TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT) {
			return vk::SamplerAddressMode::mirroredRepeat;
		} else if(mode == TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE) {
			return vk::SamplerAddressMode::clampToEdge;
		} else if(mode == TINYGLTF_TEXTURE_WRAP_REPEAT) {
			return vk::SamplerAddressMode::repeat;
		} else {
			dlg_warn("Unknown gltf sampler address mode {}", mode);
			return vk::SamplerAddressMode::repeat;
		}
	};

	addressModeU = translateAddressMode(sampler.wrapS);
	addressModeV = translateAddressMode(sampler.wrapT);
}

bool operator==(const SamplerInfo& a, const SamplerInfo& b) {
	return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool operator!=(const SamplerInfo& a, const SamplerInfo& b) {
	return std::memcmp(&a, &b, sizeof(a)) != 0;
}

Sampler::Sampler(const vpp::Device& dev, const gltf::Sampler& sampler,
		float maxAnisotropy) {
	info = {sampler};

	vk::SamplerCreateInfo sci;
	sci.addressModeU = info.addressModeU;
	sci.addressModeV = info.addressModeV;
	sci.minFilter = info.minFilter;
	sci.magFilter = info.magFilter;
	sci.mipmapMode = info.mipmapMode;
	sci.minLod = 0.f;
	sci.maxLod = 100.f; // all levels
	sci.anisotropyEnable = maxAnisotropy != 1.f;
	sci.maxAnisotropy = maxAnisotropy;
	sci.mipLodBias = 0.f;
	sci.compareEnable = false;
	sci.borderColor = vk::BorderColor::floatOpaqueWhite;
	this->sampler = {dev, sci};
}

} // namespace doi
