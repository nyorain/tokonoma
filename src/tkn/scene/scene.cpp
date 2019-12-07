#define VPP_NO_DEBUG_MARKER
#include <tkn/scene/scene.hpp>
#include <tkn/scene/shape.hpp>
#include <tkn/quaternion.hpp>
#include <tkn/types.hpp>
#include <tkn/image.hpp>
#include <tkn/gltf.hpp>
#include <tkn/bits.hpp>
#include <tkn/transform.hpp>
#include <tkn/render.hpp>
#include <tkn/texture.hpp>
#include <tkn/util.hpp>
#include <vpp/vk.hpp>
#include <vpp/debug.hpp>
#include <vpp/image.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <dlg/dlg.hpp>
#include <numeric>

// NOTE: we can't use instanced rendering since multiple instance might
// have completely different transform matrices and we therefore couldn't
// order them correctly. Could use it for different transparency
// approaches (order independent; stoachastic)
// might bring performance improvement, could e.g. be detected from
// gltf structure

// TODO: move this small utility somewhere more general (tkn/render?)
// it proves quite useful here for complex buffers with many
// subparts
struct BufferCopier {
public:
	vpp::BufferSpan src;
	vpp::BufferSpan dst;
	vk::DeviceSize srcOff {};
	vk::DeviceSize dstOff {};
	std::vector<vk::BufferCopy> copies {};

public:
	void advanceSrc(vk::DeviceSize size) {
		dlg_assert(src.size() >= srcOff + size);
		srcOff += size;
	}

	void advanceDst(vk::DeviceSize size) {
		dlg_assert(dst.size() >= dstOff + size);
		dstOff += size;
	}

	void copy(vk::DeviceSize size) {
		dlg_assert(src.size() >= srcOff + size);
		dlg_assert(dst.size() >= dstOff + size);

		copies.push_back({src.offset() + srcOff, dst.offset() + dstOff, size});
		srcOff += size;
		dstOff += size;
	}

	vpp::BufferSpan remainingSrc() const {
		return {src.buffer(), src.size() - srcOff, srcOff};
	}

	vpp::BufferSpan remainingDst() const {
		return {dst.buffer(), dst.size() - dstOff, dstOff};
	}
};

namespace tkn {
namespace {

tkn::Texture loadImage(Texture::InitData& data,
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
	if(tex.image.empty() && !tex.uri.empty()) {
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

void Scene::create(InitData& data, const WorkBatcher& wb, nytl::StringParam path,
		const tinygltf::Model& model, const tinygltf::Scene& scene,
		nytl::Mat4f matrix, const SceneRenderInfo& ri) {
	auto& dev = wb.dev;
	multiDrawIndirect_ = ri.multiDrawIndirect;
	dlg_assertm(multiDrawIndirect_, "Emulating multi draw indirect not yet "
		"implemented, see deferred/gbuf.vert");

	if(model.images.size() > imageCount) {
		auto msg = dlg::format("Model has {} images, only {} supported",
			model.images.size(), imageCount);
		throw std::runtime_error(msg);
	}
	if(model.samplers.size() > samplerCount) {
		auto msg = dlg::format("Model has {} samplers, only {} supported",
			model.samplers.size(), samplerCount);
		throw std::runtime_error(msg);
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
		// textures[imageCount]
		vpp::descriptorBinding(vk::DescriptorType::sampledImage,
			vk::ShaderStageBits::fragment, -1, imageCount),
		// samplers[samplerCount]
		vpp::descriptorBinding(vk::DescriptorType::sampler,
			vk::ShaderStageBits::fragment, -1, samplerCount),
	};

	dsLayout_ = {dev, bindings};
	vpp::nameHandle(dsLayout_, "Scene:dsLayout");
	ds_ = {data.initDs, wb.alloc.ds, dsLayout_};
	blendDs_ = {data.initBlendDs, wb.alloc.ds, dsLayout_};

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
	sci.mipLodBias = Scene::mipLodBias;
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

	dlg_info("Found {} materials", model.materials.size());
	for(auto& material : model.materials) {
		auto name = material.name.empty() ?
			material.name : "'" + material.name + "'";
		dlg_info("  Loading material {}", name);
		loadMaterial(data, wb, model, material, ri);
	}

	// we need at least one material (see primitive creation)
	// if there is none, add dummy
	defaultMaterialID_ = materials_.size();
	materials_.emplace_back(); // default material

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

	blendCount_ = 0u;
	opaqueCount_ = 0u;
	for(auto& ini : instances_) {
		dlg_assert(ini.materialID < materials_.size());
		if(materials_[ini.materialID].blend()) {
			++blendCount_;
		} else {
			++opaqueCount_;
		}
	}

	auto hostMem = dev.hostMemoryTypes();
	auto devMem = dev.deviceMemoryTypes();
	auto stageSize = 0u;

	// models buffer
	auto pcount = blendCount_ + opaqueCount_;
	auto size = pcount * sizeof(nytl::Mat4f) * 3;
	instanceBuf_ = {data.initModels, wb.alloc.bufHost, size,
		vk::BufferUsageBits::storageBuffer, hostMem};

	// primitive buffers
	size = indexCount_ * sizeof(Index);
	stageSize += size;
	indices_ = {data.initIndices, wb.alloc.bufDevice, size,
		vk::BufferUsageBits::indexBuffer |
		vk::BufferUsageBits::transferSrc |
		vk::BufferUsageBits::transferDst, devMem};

	size = data.tc1Count * sizeof(Vec2f);
	tc0Offset_ = size;
	size += data.tc0Count * sizeof(Vec2f);
	posOffset_ = size;
	size += vertexCount_ * sizeof(Vec3f);
	normalOffset_ = size;
	size += vertexCount_ * sizeof(Vec3f);

	stageSize += size;
	vertices_ = {data.initVertices, wb.alloc.bufDevice, size,
		vk::BufferUsageBits::vertexBuffer |
		vk::BufferUsageBits::transferSrc |
		vk::BufferUsageBits::transferDst, devMem};

	// materials buffer
	size = materials_.size() * sizeof(Material);
	stageSize += size;
	materialsBuf_ = {data.initMaterials, wb.alloc.bufDevice, size,
		vk::BufferUsageBits::storageBuffer |
		vk::BufferUsageBits::transferSrc |
		vk::BufferUsageBits::transferDst, devMem};

	// cmds buffer
	// TODO: i guess opaque model ids are pretty static, use devMem for that?
	// opaque cmds are not so static when we implement culling etc
	size = std::max<u32>(opaqueCount_ * sizeof(vk::DrawIndexedIndirectCommand), 4u);
	cmds_ = {data.initCmds, wb.alloc.bufHost, size,
		vk::BufferUsageBits::indirectBuffer, hostMem};
	size = std::max<u32>(opaqueCount_ * sizeof(ModelID), 4u);
	modelIDs_ = {data.initModelIDs, wb.alloc.bufHost, size,
		vk::BufferUsageBits::storageBuffer, hostMem};

	size = std::max<u32>(blendCount_ * sizeof(vk::DrawIndexedIndirectCommand), 4u);
	blendCmds_ = {data.initBlendCmds, wb.alloc.bufHost, size,
		vk::BufferUsageBits::indirectBuffer, hostMem};
	size = std::max<u32>(blendCount_ * sizeof(ModelID), 4u);
	blendModelIDs_ = {data.initBlendModelIDs, wb.alloc.bufHost, size,
		vk::BufferUsageBits::storageBuffer, hostMem};

	// stage
	data.stage = {data.initStage, wb.alloc.bufHost, stageSize,
		vk::BufferUsageBits::transferSrc, hostMem};

	auto qf = device().queueSubmitter().queue().family();
	uploadCb_ = device().commandAllocator().get(qf,
		vk::CommandPoolCreateBits::resetCommandBuffer);
	uploadSemaphore_ = {device()};
}

void Scene::loadMaterial(InitData&, const WorkBatcher&,
		const gltf::Model& model, const gltf::Material& material,
		const SceneRenderInfo&) {
	auto& m = materials_.emplace_back();
	auto& pbr = material.values;
	auto& add = material.additionalValues;
	if(auto color = pbr.find("baseColorFactor"); color != pbr.end()) {
		auto c = color->second.ColorFactor();
		m.albedoFac = nytl::Vec4f{
			float(c[0]),
			float(c[1]),
			float(c[2]),
			float(c[3])};
	}

	if(auto roughness = pbr.find("roughnessFactor"); roughness != pbr.end()) {
		dlg_assert(roughness->second.has_number_value);
		m.roughnessFac = roughness->second.Factor();
	}

	if(auto metal = pbr.find("metallicFactor"); metal != pbr.end()) {
		dlg_assert(metal->second.has_number_value);
		m.metalnessFac = metal->second.Factor();
	}

	if(auto em = add.find("emissiveFactor"); em != add.end()) {
		auto c = em->second.ColorFactor();
		m.emissionFac = {float(c[0]), float(c[1]), float(c[2])};
	}

	auto getTex = [&](const gltf::Parameter& p, bool srgb) {
		unsigned coord = p.TextureTexCoord();
		dlg_assert(coord <= 1);
		if(coord == 0) {
			m.flags |= Material::Bit::needsTexCoord0;
		} else if(coord == 1) {
			m.flags |= Material::Bit::needsTexCoord1;
		}

		auto texid = p.TextureIndex();
		dlg_assert(texid != -1);
		auto& tex = model.textures[texid];

		// sampler and texture id require +1 since id = 0 is
		// reserved for the default/dummy values
		auto samplerID = 0u;
		if(tex.sampler >= 0) {
			dlg_assert(unsigned(tex.sampler) < samplers_.size());
			samplerID = tex.sampler + 1;
		}

		dlg_assert(tex.source >= 0);
		auto id = unsigned(tex.source);

		dlg_assert(id < images_.size());
		if(images_[id].needed) {
			dlg_assert(images_[id].srgb == srgb);
		}

		images_[id].srgb = srgb;
		images_[id].needed = true;
		return Material::Tex{coord, id + 1, samplerID};
	};

	if(auto tex = pbr.find("baseColorTexture"); tex != pbr.end()) {
		m.albedo = getTex(tex->second, true);
	}
	if(auto tex = pbr.find("metallicRoughnessTexture"); tex != pbr.end()) {
		m.metalRough = getTex(tex->second, false);
	}

	if(auto tex = add.find("normalTexture"); tex != add.end()) {
		m.normals = getTex(tex->second, false);
		m.flags |= Material::Bit::normalMap;
	}
	if(auto tex = add.find("occlusionTexture"); tex != add.end()) {
		m.occlusion = getTex(tex->second, false);
	}
	if(auto tex = add.find("emissiveTexture"); tex != add.end()) {
		m.emission = getTex(tex->second, true);
	}

	// default is OPAQUE (alphaCutoff_ == -1.f);
	if(auto am = add.find("alphaMode"); am != add.end()) {
		auto& alphaMode = am->second.string_value;
		dlg_assert(!alphaMode.empty());
		if(alphaMode == "BLEND") {
			m.flags |= Material::Bit::blend;
			m.alphaCutoff = 0.0;
		} else if(alphaMode == "MASK") {
			m.alphaCutoff = 0.5; // default per gltf
			if(auto ac = add.find("alphaCutoff"); ac != add.end()) {
				dlg_assert(ac->second.has_number_value);
				m.alphaCutoff = ac->second.number_value;
			}
		}
	}

	// doubleSided, default is false
	if(auto d = add.find("doubleSided"); d != add.end()) {
		if(d->second.bool_value) {
			m.flags |= Material::Bit::doubleSided;
		}
	}
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
			tkn::Quaternion q;
			q.x = node.rotation[0];
			q.y = node.rotation[1];
			q.z = node.rotation[2];
			q.w = node.rotation[3];

			mat = tkn::toMat<4>(q) * mat;
		}

		if(!node.translation.empty()) {
			nytl::Vec3f t;
			t.x = node.translation[0];
			t.y = node.translation[1];
			t.z = node.translation[2];
			mat = tkn::translateMat(t) * mat;
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
			// TODO
			try {
				loadPrimitive(data, wb, model, primitive, matrix);
			} catch(const std::exception& err) {
				dlg_error("Omitting primitive: {}", err.what());
			}
		}
	}
}

void Scene::loadPrimitive(InitData& data, const WorkBatcher&,
		const gltf::Model& model, const gltf::Primitive& primitive,
		nytl::Mat4f matrix) {
	auto& p = primitives_.emplace_back();
	auto mat = primitive.material;
	if(mat < 0) {
		// use default material
		mat = defaultMaterialID_;
	}

	// supporting other modes requires custom pipelines, means that
	// we can't render the scene with a single pipe.
	// theoretically possible but a lot of complexity, really not
	// worth it for now
	if(primitive.mode != TINYGLTF_MODE_TRIANGLES) {
		dlg_error("Unsupported primitive.mode: {}", primitive.mode);
		throw std::runtime_error("Unsupported primitive.mode");
	}

	auto ip = primitive.attributes.find("POSITION");
	auto in = primitive.attributes.find("NORMAL");
	auto itc0 = primitive.attributes.find("TEXCOORD_0");
	auto itc1 = primitive.attributes.find("TEXCOORD_1");

	// positions
	// a model *must* have this property, we can't get that anywhere else/
	// just guess something
	if(ip == primitive.attributes.end()) {
		throw std::runtime_error("primitve doesn't have POSITION");
	}

	auto& pa = model.accessors[ip->second];

	// PERF: we could use the gltf-supplied min-max values
	auto inf = std::numeric_limits<float>::infinity();
	p.min = nytl::Vec3f{inf, inf, inf};
	p.max = nytl::Vec3f{-inf, -inf, -inf};

	p.positions.reserve(pa.count);
	for(const auto& pos : range<3, float>(model, pa)) {
		p.min = nytl::vec::cw::min(p.min, pos);
		p.max = nytl::vec::cw::max(p.max, pos);
		p.positions.push_back(pos);
	}

	// indices
	if(primitive.indices < 0) {
		dlg_info("Primitive has no indices, using simple iota indices");
		p.indices.resize(pa.count);
		std::iota(p.indices.begin(), p.indices.end(), u32(0u));
	} else {
		auto& ia = model.accessors[primitive.indices];
		p.indices.resize(ia.count);

		auto iit = p.indices.begin();
		for(auto idx : tkn::range<1, std::uint32_t>(model, ia)) {
			dlg_assertm(idx < p.positions.size(), "Index out of range");
			*(iit++) = idx;
		}
	}

	// normals
	if(in == primitive.attributes.end()) {
		dlg_info("Primitive has no normals, generating simple normals");
		dlg_assertm(p.indices.size() % 3 == 0,
			"Triangle mode primitive index count not multiple of 3");

		p.normals = areaSmoothNormals(p.positions, p.indices);
	} else {
		auto& na = model.accessors[in->second];
		dlg_assert(na.count == pa.count);

		p.normals.reserve(na.count);
		for(const auto& normal : range<3, float>(model, na)) {
			p.normals.push_back(normal);
		}
	}

	auto* tc0a = itc0 == primitive.attributes.end() ?
		nullptr : &model.accessors[itc0->second];
	auto* tc1a = itc1 == primitive.attributes.end() ?
		nullptr : &model.accessors[itc1->second];

	// compute total buffer size
	auto size = 0u;
	size += p.indices.size() * sizeof(u32); // indices
	size += p.positions.size() * sizeof(nytl::Vec3f); // normals
	size += p.normals.size() * sizeof(nytl::Vec3f); // positions

	indexCount_ += p.indices.size();
	vertexCount_ += p.positions.size();

	if(tc0a) {
		dlg_assert(tc0a->count == pa.count);

		p.texCoords0.resize(tc0a->count);
		auto tc0it = p.texCoords0.begin();
		auto uvr = tkn::range<2, float>(model, *tc0a);
		for(auto uv : uvr) {
			*(tc0it++) = uv;
		}
		data.tc0Count += tc0a->count;
	}

	if(tc1a) {
		dlg_assert(tc1a->count == pa.count);

		p.texCoords1.resize(tc1a->count);
		auto tc1it = p.texCoords1.begin();
		auto uvr = tkn::range<2, float>(model, *tc1a);
		for(auto uv : uvr) {
			*(tc1it++) = uv;
		}
		data.tc1Count += tc1a->count;
	}

	min_ = nytl::vec::cw::min(min_, multPos(matrix, p.min));
	max_ = nytl::vec::cw::max(max_, multPos(matrix, p.max));

	Instance ini;
	ini.matrix = matrix;
	ini.lastMatrix = matrix;
	ini.materialID = mat;
	ini.modelID = ++instanceID_;
	ini.primitiveID = primitives_.size() - 1;

	if(!tc0a && materials_[ini.materialID].needsTexCoord0()) {
		throw std::runtime_error("material uses texCoords0 but primitive "
			"doesn't provide them");
	}
	if(!tc1a && materials_[ini.materialID].needsTexCoord1()) {
		throw std::runtime_error("material uses texCoords1 but primitive "
			"doesn't provide them");
	}

	instances_.push_back(ini);
}

void Scene::init(InitData& data, const WorkBatcher& wb, vk::ImageView dummyView) {
	dlg_assert(images_.size() == data.images.size());

	data.stage.init(data.initStage);
	materialsBuf_.init(data.initMaterials);
	instanceBuf_.init(data.initModels);
	cmds_.init(data.initCmds);
	modelIDs_.init(data.initModelIDs);
	blendCmds_.init(data.initBlendCmds);
	blendModelIDs_.init(data.initBlendModelIDs);
	vertices_.init(data.initVertices);
	indices_.init(data.initIndices);

	ds_.init(data.initDs);
	blendDs_.init(data.initBlendDs);
	vpp::nameHandle(ds_, "Scene:ds");
	vpp::nameHandle(blendDs_, "Scene:blendDs");

	// init images
	for(auto i = 0u; i < data.images.size(); ++i) {
		images_[i].image.init(data.images[i], wb);
	}

	// upload buffer data
	auto stageMap = data.stage.memoryMap();
	auto span = stageMap.span();
	for(auto& mat : materials_) {
		tkn::write(span, mat);
	}

	vk::BufferCopy copy;
	copy.srcOffset = data.stage.offset();
	copy.dstOffset = materialsBuf_.offset();
	copy.size = materialsBuf_.size();
	vk::cmdCopyBuffer(wb.cb, data.stage.buffer(),
		materialsBuf_.buffer(), {{copy}});

	auto indexOff = span.data() - stageMap.ptr();
	auto indexSpan = span;

	auto tc1Span = span;
	skip(tc1Span, sizeof(Index) * indexCount_);
	auto tc1Off = tc1Span.data() - stageMap.ptr();
	auto tc0Span = tc1Span;
	skip(tc0Span, sizeof(nytl::Vec2f) * data.tc1Count);
	auto posSpan = tc0Span;
	skip(posSpan, sizeof(nytl::Vec2f) * data.tc0Count);
	auto normalSpan = posSpan;
	skip(normalSpan, sizeof(nytl::Vec3f) * vertexCount_);

	auto vertexCount = 0u;
	auto indexCount = 0u;
	auto uploadPrimitive = [&](Primitive& primitive) {
		tkn::write(tc0Span, bytes(primitive.texCoords0));
		tkn::write(tc1Span, bytes(primitive.texCoords1));
		tkn::write(posSpan, bytes(primitive.positions));
		tkn::write(normalSpan, bytes(primitive.normals));
		tkn::write(indexSpan, bytes(primitive.indices));

		primitive.firstIndex = indexCount;
		primitive.vertexOffset = vertexCount;
		indexCount += primitive.indices.size();
		vertexCount += primitive.positions.size();
	};

	// the ordering of primitives in the buffer like this allows us to
	// only allocate tc0 and tc1 buffer space for the models that
	// really need it the. The others will read garbage.
	// the buffer is always large enough since tc0,tc1,verts are always
	// allocated on the same buffer and the data in verts is large
	// than the (theoretical) need for tc0, tc1

	// first upload primitives that have both tex coords
	for(auto& primitive : primitives_) {
		if(!primitive.texCoords1.empty()) {
			uploadPrimitive(primitive);
		}
	}

	// then primitives with only one tex coord
	for(auto& primitive : primitives_) {
		if(primitive.texCoords1.empty() && !primitive.texCoords0.empty()) {
			uploadPrimitive(primitive);
		}
	}

	// then primitives without any texcoord
	for(auto& primitive : primitives_) {
		if(primitive.texCoords1.empty() && primitive.texCoords0.empty()) {
			uploadPrimitive(primitive);
		}
	}

	// indices
	copy.srcOffset = data.stage.offset() + indexOff;
	copy.dstOffset = indices_.offset();
	copy.size = indices_.size();
	vk::cmdCopyBuffer(wb.cb, data.stage.buffer(),
		indices_.buffer(), {{copy}});

	// all vertex data (pos, normal, tex coords)
	copy.srcOffset = data.stage.offset() + tc1Off;
	copy.dstOffset = vertices_.offset();
	copy.size = vertices_.size();
	vk::cmdCopyBuffer(wb.cb, data.stage.buffer(),
		vertices_.buffer(), {{copy}});

	// upload matrices for all primitives to modelsBuf
	span = normalSpan; // last offset
	stageMap.flush();

	// upload opaque draw commands
	// TODO: cmd/models should be done in updateDevice/at runtime;
	// support culling
	auto idmap = modelIDs_.memoryMap();
	auto idspan = idmap.span();
	auto cmdmap = cmds_.memoryMap();
	auto cmdspan = cmdmap.span();
	auto iniMap = instanceBuf_.memoryMap();
	auto iniSpan = iniMap.span();
	for(auto& ini : instances_) {
		if(!materials_[ini.materialID].blend()) { // not opaque
			writeInstance(ini, idspan, cmdspan);
		}

		write(iniSpan, ini.matrix);
		auto normalMatrix = nytl::Mat4f(transpose(inverse(ini.matrix)));
		normalMatrix[3][0] = ini.materialID;
		normalMatrix[3][1] = ini.modelID;
		write(iniSpan, normalMatrix);
		write(iniSpan, ini.lastMatrix);
	}

	idmap.flush();
	cmdmap.flush();
	iniMap.flush();

	// descriptors
	std::vector<vk::DescriptorImageInfo> images;
	images.reserve(imageCount);
	auto& dummyInfo = images.emplace_back();
	dummyInfo.imageLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	dummyInfo.imageView = dummyView;
	for(auto& img : images_) {
		auto& info = images.emplace_back();
		info.imageLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		info.imageView = img.image.imageView();
	}
	images.resize(imageCount, dummyInfo);

	std::vector<vk::DescriptorImageInfo> samplers;
	samplers.reserve(samplerCount);
	auto& defaultInfo = samplers.emplace_back();
	defaultInfo.sampler = defaultSampler_;
	for(auto& sampler : samplers_) {
		auto& info = samplers.emplace_back();
		info.sampler = sampler.sampler;
	}
	samplers.resize(samplerCount, defaultInfo);

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.storage({{{modelIDs_}}});
	dsu.storage({{{instanceBuf_}}});
	dsu.storage({{{materialsBuf_}}});
	dsu.image(images);
	dsu.sampler(samplers);

	vpp::DescriptorSetUpdate bdsu(blendDs_);
	bdsu.storage({{{blendModelIDs_}}});
	bdsu.storage({{{instanceBuf_}}});
	bdsu.storage({{{materialsBuf_}}});
	bdsu.image(images);
	bdsu.sampler(samplers);
}

void Scene::createImage(unsigned id, bool srgb) {
	dlg_assert(id < images_.size());
	if(images_[id].needed) {
		dlg_assert(images_[id].srgb == srgb);
	}

	images_[id].srgb = srgb;
	images_[id].needed = true;
}

void Scene::writeInstance(const Instance& ini, nytl::Span<std::byte>& ids,
		nytl::Span<std::byte>& cmds) {
	// HACK: not sure if logical ids used as rendering indirection
	// should be connected to picking-releated model ids
	tkn::write(ids, u32(ini.modelID - 1));

	auto& p = primitives_[ini.primitiveID];
	vk::DrawIndexedIndirectCommand cmd;
	cmd.indexCount = p.indices.size();
	cmd.vertexOffset = p.vertexOffset;
	cmd.instanceCount = 1;
	cmd.firstInstance = 0;
	cmd.firstIndex = p.firstIndex;
	tkn::write(cmds, cmd);
}

// TODO: when updateDs is set to true, a rerecord is needed
// not necessarily coupled to semaphore...
vk::Semaphore Scene::upload() {
	auto& dev = device();

	// reset from previoius upload
	upload_.indices = {};
	upload_.materials = {};
	upload_.vertices = {};

	auto updateDs = false;
	if(newInis_ || !updateInis_.empty()) {
		// recound blend/opaque
		auto nblendCount = 0u;
		auto nopaqueCount = 0u;
		for(auto& ini : instances_) {
			dlg_assert(ini.materialID < materials_.size());
			if(materials_[ini.materialID].blend()) {
				++nblendCount;
			} else {
				++nopaqueCount;
			}
		}

		// recreate buffers as needed
		u32 size;
		if(nblendCount != blendCount_) {
			blendCount_ = nblendCount;
			size = std::max<u32>(blendCount_ * sizeof(vk::DrawIndexedIndirectCommand), 4u);
			blendCmds_ = {dev.bufferAllocator(), size,
				vk::BufferUsageBits::indirectBuffer, dev.hostMemoryTypes()};
			size = std::max<u32>(blendCount_ * sizeof(ModelID), 4u);
			blendModelIDs_ = {dev.bufferAllocator(), size,
				vk::BufferUsageBits::storageBuffer, dev.hostMemoryTypes()};
			updateDs = true;
		}

		if(nopaqueCount != opaqueCount_) {
			opaqueCount_ = nopaqueCount;
			size = std::max<u32>(opaqueCount_ *
				sizeof(vk::DrawIndexedIndirectCommand), 4u);
			cmds_ = {dev.bufferAllocator(), size,
				vk::BufferUsageBits::indirectBuffer, dev.hostMemoryTypes()};
			size = std::max<u32>(opaqueCount_ * sizeof(ModelID), 4u);
			modelIDs_ = {dev.bufferAllocator(), size,
				vk::BufferUsageBits::storageBuffer, dev.hostMemoryTypes()};
			updateDs = true;
		}

		if(newInis_) {
			auto pcount = blendCount_ + opaqueCount_;
			auto size = pcount * sizeof(nytl::Mat4f) * 3;
			instanceBuf_ = {dev.bufferAllocator(), size,
				vk::BufferUsageBits::storageBuffer , dev.hostMemoryTypes()};
			updateDs = true;
		}

		// upload new ids, commands for opaque instances
		// for blend instances they are written after sort in updateDevice
		auto idmap = modelIDs_.memoryMap();
		auto idspan = idmap.span();
		auto cmdmap = cmds_.memoryMap();
		auto cmdspan = cmdmap.span();
		auto iniMap = instanceBuf_.memoryMap();
		auto iniSpan = iniMap.span();

		// TODO: don't write all, only changed ones
		for(auto& ini : instances_) {
			if(!materials_[ini.materialID].blend()) { // opaque
				writeInstance(ini, idspan, cmdspan);
			}

			write(iniSpan, ini.matrix);
			auto normalMatrix = nytl::Mat4f(transpose(inverse(ini.matrix)));
			normalMatrix[3][0] = ini.materialID;
			normalMatrix[3][1] = ini.modelID;
			write(iniSpan, normalMatrix);
			write(iniSpan, ini.lastMatrix);
		}

		idmap.flush();
		cmdmap.flush();
		iniMap.flush();

		newInis_ = {};
		updateInis_.clear();
	}

	if(!newMats_ && !newPrimitives_) {
		return {};
	}

	vk::CommandBuffer cb = uploadCb_;
	vk::beginCommandBuffer(uploadCb_, {});

	// utility
	auto subspan = [](vpp::BufferSpan span, vk::DeviceSize off,
			vk::DeviceSize size) {
		return vpp::BufferSpan(span.buffer(), size, span.offset() + off);
	};

	auto split = [](vpp::BufferSpan span, vk::DeviceSize off)
			-> std::array<vpp::BufferSpan, 2> {
		dlg_assert(off <= span.size());
		auto size1 = off;
		auto size2 = span.size() - off;
		auto a = vpp::BufferSpan(span.buffer(), size1, span.offset());
		auto b = vpp::BufferSpan(span.buffer(), size2, span.offset() + off);
		return {a, b};
	};

	auto stageSize = newMats_ * sizeof(Material);
	for(auto& p : nytl::span(primitives_).last(newPrimitives_)) {
		stageSize += bytes(p.positions).size();
		stageSize += bytes(p.normals).size();
		stageSize += bytes(p.texCoords0).size();
		stageSize += bytes(p.texCoords1).size();
		stageSize += bytes(p.indices).size();
	}

	// create stage buffer
	if(upload_.stage.size() < stageSize) {
		upload_.stage = {dev.bufferAllocator(), stageSize,
			vk::BufferUsageBits::transferSrc,
			dev.hostMemoryTypes()};
	}

	auto stageMap = upload_.stage.memoryMap();
	auto stageSpan = stageMap.span();

	auto devMem = device().deviceMemoryTypes();
	if(newMats_) {
		updateDs = true;
		upload_.materials = std::move(materialsBuf_);

		auto size = sizeof(Material) * materials_.size();
		materialsBuf_ = {device().bufferAllocator(), size,
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::transferSrc |
			vk::BufferUsageBits::transferDst, devMem};

		auto [oldm, newm] = split(materialsBuf_, upload_.materials.size());
		tkn::cmdCopyBuffer(cb, upload_.materials, oldm);

		auto newMats = nytl::span(materials_).last(newMats_);
		auto soff = stageSpan.data() - stageMap.ptr();
		auto ssize = write(stageSpan, nytl::as_bytes(newMats));
		auto srcMats = subspan(upload_.stage, soff, ssize);
		tkn::cmdCopyBuffer(cb, srcMats, newm);

		newMats_ = 0;
	}

	if(newPrimitives_) {
		updateDs = true;
		auto off0 = stageSpan.data() - stageMap.ptr();

		upload_.vertices = std::move(vertices_);
		upload_.indices = std::move(indices_);

		auto tc1Size = 0u;
		for(auto& p : nytl::span(primitives_).last(newPrimitives_)) {
			tc1Size += write(stageSpan, bytes(p.texCoords1));
		}

		auto tc0Size = 0u;
		for(auto& p : nytl::span(primitives_).last(newPrimitives_)) {
			tc0Size += write(stageSpan, bytes(p.texCoords0));
		}

		auto posSize = 0u;
		for(auto& p : nytl::span(primitives_).last(newPrimitives_)) {
			posSize += write(stageSpan, bytes(p.positions));
		}

		auto normalsSize = 0u;
		for(auto& p : nytl::span(primitives_).last(newPrimitives_)) {
			normalsSize += write(stageSpan, bytes(p.normals));
		}

		dlg_assert(posSize == normalsSize);
		auto addSize = tc1Size + tc0Size + posSize + normalsSize;
		auto newVSize = upload_.vertices.size() + addSize;
		vertices_ = {device().bufferAllocator(), newVSize,
			vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::transferSrc |
			vk::BufferUsageBits::transferDst, devMem};

		// copy vertices
		auto oldtc1 = tc0Offset_ - 0;
		auto oldtc0 = posOffset_ - tc0Offset_;
		auto oldpos = normalOffset_ - posOffset_;
		auto oldnormals = oldpos;
		dlg_assert(oldnormals == upload_.vertices.size() - normalOffset_);

		BufferCopier oldVCopies{upload_.vertices, vertices_};
		oldVCopies.copy(oldtc1);
		oldVCopies.advanceDst(tc1Size);
		oldVCopies.copy(oldtc0);
		oldVCopies.advanceDst(tc0Size);
		oldVCopies.copy(oldpos);
		oldVCopies.advanceDst(posSize);
		oldVCopies.copy(oldnormals);
		vk::cmdCopyBuffer(cb, upload_.vertices.buffer(), vertices_.buffer(),
			oldVCopies.copies);

		BufferCopier newVCopies{upload_.stage, vertices_};
		newVCopies.advanceSrc(off0);
		newVCopies.advanceDst(oldtc1);
		newVCopies.copy(tc1Size);
		newVCopies.advanceDst(oldtc0);
		newVCopies.copy(tc0Size);
		newVCopies.advanceDst(oldpos);
		newVCopies.copy(posSize);
		newVCopies.advanceDst(oldnormals);
		newVCopies.copy(normalsSize);
		vk::cmdCopyBuffer(cb, upload_.stage.buffer(), vertices_.buffer(),
			newVCopies.copies);

		tc0Offset_ += tc1Size;
		posOffset_ += tc0Size + tc1Size;
		normalOffset_ += tc0Size + tc1Size + posSize;

		// copy indices
		off0 = stageSpan.data() - stageMap.ptr();
		auto indicesSize = 0u;
		for(auto& p : nytl::span(primitives_).last(newPrimitives_)) {
			indicesSize += write(stageSpan, bytes(p.indices));
		}

		auto newISize = upload_.indices.size() + indicesSize;
		indices_ = {device().bufferAllocator(), newISize,
			vk::BufferUsageBits::indexBuffer |
			vk::BufferUsageBits::transferSrc |
			vk::BufferUsageBits::transferDst, devMem};

		auto [oldi, newi] = split(indices_, upload_.indices.size());
		tkn::cmdCopyBuffer(cb, upload_.indices, oldi);
		tkn::cmdCopyBuffer(cb, subspan(upload_.stage, off0, indicesSize), newi);

		newPrimitives_ = 0;
	}

	// pretty sure this assert is wrong, right?
	// because stageSpan can obviously be larger than what we neet
	// dlg_assertm(stageSpan.size() == 0, "{}", stageSpan.size()); // all used

	stageMap.flush();
	stageMap = {};

	vk::endCommandBuffer(uploadCb_);

	vk::SubmitInfo si;
	si.pCommandBuffers = &uploadCb_.vkHandle();
	si.commandBufferCount = 1;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &uploadSemaphore_.vkHandle();

	device().queueSubmitter().add(si);
	if(updateDs) {
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.storage({{{modelIDs_}}});
		dsu.storage({{{instanceBuf_}}});
		dsu.storage({{{materialsBuf_}}});

		vpp::DescriptorSetUpdate bdsu(blendDs_);
		bdsu.storage({{{blendModelIDs_}}});
		bdsu.storage({{{instanceBuf_}}});
		bdsu.storage({{{materialsBuf_}}});
	}

	return uploadSemaphore_;
}

vk::Semaphore Scene::updateDevice(nytl::Mat4f proj) {
	auto ret = upload();

	// sort blended primitives
	if(!blendCount_) {
		return ret;
	}

	struct Prim {
		const Instance* instance;
		float depth;
	};

	std::vector<Prim> blendPrims;
	for(auto& ini : instances_) {
		auto& p = primitives_[ini.primitiveID];
		if(materials_[ini.materialID].blend()) {
			auto c = multPos(ini.matrix, 0.5f * (p.min + p.max));
			auto z = multPos(proj, c).z;
			blendPrims.push_back({&ini, z});
		}
	}
	dlg_assert(blendPrims.size() == blendCount_);

	// sort in z-descending order
	auto cmp = [&](const Prim& p1, const Prim& p2) {
		return p1.depth > p2.depth;
	};
	std::sort(blendPrims.begin(), blendPrims.end(), cmp);

	auto idmap = blendModelIDs_.memoryMap();
	auto idspan = idmap.span();
	auto cmdmap = blendCmds_.memoryMap();
	auto cmdspan = cmdmap.span();
	for(auto& p : blendPrims) {
		writeInstance(*p.instance, idspan, cmdspan);
	}

	idmap.flush();
	cmdmap.flush();
	return ret;
}

void Scene::render(vk::CommandBuffer cb, vk::PipelineLayout pl, bool blend) const {
	if(blend && !blendCount_) {
		return;
	}

	cmdBindGraphicsDescriptors(cb, pl, 1, {blend ? blendDs_ : ds_});
	vk::cmdBindIndexBuffer(cb, indices_.buffer(), indices_.offset(),
		vk::IndexType::uint32);

	auto bufs = {
		vertices_.buffer().vkHandle(), // pos
		vertices_.buffer().vkHandle(), // normal
		vertices_.buffer().vkHandle(), // tc0
		vertices_.buffer().vkHandle(), // tc1
	};
	auto offsets = {
		vertices_.offset() + posOffset_, // pos
		vertices_.offset() + normalOffset_, // normal
		vertices_.offset() + tc0Offset_, // tc0
		vertices_.offset(), // tc1
	};

	auto& cmds = blend ? blendCmds_ : cmds_;
	auto count = blend ? blendCount_ : opaqueCount_;

	vk::cmdBindVertexBuffers(cb, 0, bufs, offsets);
	if(multiDrawIndirect_) {
		auto stride = sizeof(vk::DrawIndexedIndirectCommand);
		vk::cmdDrawIndexedIndirect(cb, cmds.buffer(), cmds.offset(),
			count, stride);
	} else {
		auto off = cmds.offset();
		for(auto i = 0u; i < count; ++i) {
			vk::cmdDrawIndexedIndirect(cb, cmds.buffer(), off, 1u, 0u);
			off += sizeof(vk::DrawIndexedIndirectCommand);
		}
	}
}

u32 Scene::addPrimitive(std::vector<nytl::Vec3f> positions,
		std::vector<nytl::Vec3f> normals,
		std::vector<u32> indices,
		std::vector<nytl::Vec2f> texCoords0,
		std::vector<nytl::Vec2f> texCoords1) {
	++newPrimitives_;
	dlg_assert(normals.size() == positions.size());
	for(auto& i : indices) {
		dlg_assert(i < positions.size());
	}

	auto& p = primitives_.emplace_back();
	p.positions = std::move(positions);
	p.normals = std::move(normals);
	p.indices = std::move(indices);
	p.texCoords0 = std::move(texCoords0);
	p.texCoords1 = std::move(texCoords1);
	p.firstIndex = indexCount_;
	p.vertexOffset = vertexCount_;
	indexCount_ += p.indices.size();
	vertexCount_ += p.positions.size();
	return primitives_.size() - 1;
}

u32 Scene::addMaterial(const Material& mat) {
	materials_.push_back(mat);
	++newMats_;
	return materials_.size() - 1;
}

u32 Scene::addInstance(u32 primitiveID, nytl::Mat4f matrix, u32 matID) {
	auto& ini = instances_.emplace_back();
	ini.materialID = matID;
	ini.matrix = matrix;
	ini.lastMatrix = matrix;
	ini.modelID = ++instanceID_;
	ini.primitiveID = primitiveID;
	++newInis_;
	return instances_.size() - 1;
}

u32 Scene::addInstance(const Primitive& prim, nytl::Mat4f matrix, u32 matID) {
	auto it = std::find_if(primitives_.begin(), primitives_.end(),
		[&](auto& p) { return &p == &prim; });
	dlg_assert(it != primitives_.end());
	return addInstance(it - primitives_.begin(), matrix, matID);
}

const vk::PipelineVertexInputStateCreateInfo& Scene::vertexInfo() {
	// Deinterleave all data (even positions and normals which are
	// always given) since e.g. for shadow maps normals are not
	// needed. Texture coordinates might not be given
	static constexpr vk::VertexInputBindingDescription bindings[] = {
		{0, sizeof(Vec3f), vk::VertexInputRate::vertex}, // positions
		{1, sizeof(Vec3f), vk::VertexInputRate::vertex}, // normals
		{2, sizeof(Vec2f), vk::VertexInputRate::vertex}, // texCoords0
		{3, sizeof(Vec2f), vk::VertexInputRate::vertex}, // texCoords1
	};

	static constexpr vk::VertexInputAttributeDescription attributes[] = {
		{0, 0, vk::Format::r32g32b32Sfloat, 0}, // pos
		{1, 1, vk::Format::r32g32b32Sfloat, 0}, // normal
		{2, 2, vk::Format::r32g32Sfloat, 0}, // texCoords0
		{3, 3, vk::Format::r32g32Sfloat, 0}, // texCoords1
	};

	static const vk::PipelineVertexInputStateCreateInfo ret = {{},
		4, bindings,
		4, attributes
	};

	return ret;
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
	bool res;

	auto full = std::string(path);
	full += file;
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

std::optional<gltf::Model> loadGltf(nytl::Span<const std::byte> buffer) {
	gltf::TinyGLTF loader;
	gltf::Model model;
	std::string err, warn;

	auto bytes = (const unsigned char*) buffer.data();
	auto res = loader.LoadBinaryFromMemory(&model, &err, &warn, bytes, buffer.size());

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

	return {model};
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
	sci.mipLodBias = Scene::mipLodBias;
	sci.maxLod = 100.f; // all levels
	sci.anisotropyEnable = maxAnisotropy != 1.f;
	sci.maxAnisotropy = maxAnisotropy;
	sci.compareEnable = false;
	sci.borderColor = vk::BorderColor::floatOpaqueWhite;
	this->sampler = {dev, sci};
}

} // namespace tkn
