#include <stage/scene/scene.hpp>
#include <stage/quaternion.hpp>
#include <stage/image.hpp>
#include <stage/gltf.hpp>
#include <stage/bits.hpp>
#include <stage/transform.hpp>
#include <stage/render.hpp>
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

void Scene::create(InitData& data, const WorkBatcher& wb, nytl::StringParam path,
		const tinygltf::Model& model, const tinygltf::Scene& scene,
		nytl::Mat4f matrix, const SceneRenderInfo& ri) {
	auto& dev = wb.dev;
	multiDrawIndirect_ = ri.multiDrawIndirect;

	// TODO: can implement fallback, see gbuf.vert
	dlg_assert(ri.multiDrawIndirect);

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
	for(auto& p : primitives_) {
		dlg_assert(p.material < materials_.size());
		if(materials_[p.material].blend()) {
			dlg_trace("blend primitive {} {}", p.id, p.material);
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
	auto size = pcount * sizeof(nytl::Mat4f) * 2;
	stageSize += size;
	modelsBuf_ = {data.initModels, wb.alloc.bufHost, size,
		vk::BufferUsageBits::storageBuffer |
		vk::BufferUsageBits::transferDst,
		devMem};

	// primitive buffers
	size = data.indexCount * sizeof(Index);
	stageSize += size;
	indices_ = {data.initIndices, wb.alloc.bufDevice, size,
		vk::BufferUsageBits::indexBuffer |
		vk::BufferUsageBits::transferDst, devMem};

	size = data.tc1Count * sizeof(nytl::Vec2f);
	tc0Offset_ = size;
	size += data.tc0Count * sizeof(nytl::Vec2f);
	vertexOffset_ = size;
	size += data.vertexCount * sizeof(Primitive::Vertex);
	stageSize += size;
	vertices_ = {data.initVertices, wb.alloc.bufDevice, size,
		vk::BufferUsageBits::vertexBuffer |
		vk::BufferUsageBits::transferDst, devMem};

	// materials buffer
	size = materials_.size() * sizeof(Material);
	stageSize += size;
	materialsBuf_ = {data.initMaterials, wb.alloc.bufDevice, size,
		vk::BufferUsageBits::storageBuffer |
		vk::BufferUsageBits::transferDst, devMem};

	// cmds buffer
	// TODO: i guess opaque cmds and model ids are pretty static,
	// use devMem for that?
	size = std::max<u32>(opaqueCount_ * sizeof(vk::DrawIndexedIndirectCommand), 4u);
	cmds_ = {data.initCmds, wb.alloc.bufHost, size,
		vk::BufferUsageBits::indirectBuffer, hostMem};
	size = std::max<u32>(opaqueCount_ * sizeof(ModelID), 4u);
	modelIDs_ = {data.initModelIDs, wb.alloc.bufHost, size,
		vk::BufferUsageBits::storageTexelBuffer, hostMem};

	size = std::max<u32>(blendCount_ * sizeof(vk::DrawIndexedIndirectCommand), 4u);
	blendCmds_ = {data.initBlendCmds, wb.alloc.bufHost, size,
		vk::BufferUsageBits::indirectBuffer, hostMem};
	size = std::max<u32>(blendCount_ * sizeof(ModelID), 4u);
	blendModelIDs_ = {data.initBlendModelIDs, wb.alloc.bufHost, size,
		vk::BufferUsageBits::storageTexelBuffer, hostMem};

	// stage
	data.stage = {data.initStage, wb.alloc.bufHost, stageSize,
		vk::BufferUsageBits::transferSrc, hostMem};
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
			dlg_trace("BLEND alpha mode");
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

	p.material = mat;
	p.id = primitives_.size();
	p.matrix = matrix;

	auto ip = primitive.attributes.find("POSITION");
	auto in = primitive.attributes.find("NORMAL");
	auto itc0 = primitive.attributes.find("TEXCOORD_0");
	auto itc1 = primitive.attributes.find("TEXCOORD_1");

	if(ip == primitive.attributes.end()) {
		throw std::runtime_error("primitve doesn't have POSITION");
	}

	// TODO: we could manually compute normals, flat normals are good
	// enough in this case. But we could also an algorith smooth normals
	if(in == primitive.attributes.end()) {
		throw std::runtime_error("primitve doesn't have NORMAL");
	}

	auto& pa = model.accessors[ip->second];
	auto& na = model.accessors[in->second];

	// TODO: could fix that by inserting simple dummy indices
	if(primitive.indices < 0) {
		throw std::runtime_error("Only models with indices supported");
	}
	auto& ia = model.accessors[primitive.indices];

	auto* tc0a = itc0 == primitive.attributes.end() ?
		nullptr : &model.accessors[itc0->second];
	auto* tc1a = itc1 == primitive.attributes.end() ?
		nullptr : &model.accessors[itc1->second];

	// compute total buffer size
	auto size = 0u;
	size += ia.count * sizeof(uint32_t); // indices
	size += na.count * sizeof(nytl::Vec3f); // normals
	size += pa.count * sizeof(nytl::Vec3f); // positions
	dlg_assert(na.count == pa.count);

	p.indexCount = ia.count;
	p.vertexCount = na.count;

	data.indexCount += p.indexCount;
	data.vertexCount += p.vertexCount;

	// write indices
	// TODO: in optimal cases we might be able to directly memcpy
	p.indices.resize(ia.count);
	auto iit = p.indices.begin();
	for(auto idx : doi::range<1, std::uint32_t>(model, ia)) {
		dlg_assert(idx < p.vertexCount);
		*(iit++) = idx;
	}

	// write vertices and normals
	// TODO: we could use the gltf-supplied min-max values
	auto inf = std::numeric_limits<float>::infinity();
	p.min = nytl::Vec3f{inf, inf, inf};
	p.max = nytl::Vec3f{-inf, -inf, -inf};

	p.vertices.resize(pa.count);
	auto vit = p.vertices.begin();
	auto pr = doi::range<3, float>(model, pa);
	auto nr = doi::range<3, float>(model, na);
	for(auto pit = pr.begin(), nit = nr.begin();
			pit != pr.end() && nit != nr.end();
			++nit, ++pit) {
		p.min = nytl::vec::cw::min(p.min, *pit);
		p.max = nytl::vec::cw::max(p.max, *pit);
		*(vit++) = {*pit, *nit};
	}

	if(tc0a) {
		dlg_assert(tc0a->count == pa.count);

		p.texCoords0.resize(tc0a->count);
		auto tc0it = p.texCoords0.begin();
		auto uvr = doi::range<2, float>(model, *tc0a);
		for(auto uv : uvr) {
			*(tc0it++) = uv;
		}
		data.tc0Count += tc0a->count;
	} else if(materials_[p.material].needsTexCoord0()) {
		throw std::runtime_error("material uses texCoords0 but primitive "
			"doesn't provide them");
	}

	if(tc1a) {
		dlg_assert(tc1a->count == pa.count);

		p.texCoords1.resize(tc1a->count);
		auto tc1it = p.texCoords1.begin();
		auto uvr = doi::range<2, float>(model, *tc1a);
		for(auto uv : uvr) {
			*(tc1it++) = uv;
		}
		data.tc1Count += tc1a->count;
	} else if(materials_[p.material].needsTexCoord1()) {
		throw std::runtime_error("material uses texCoords1 but primitive "
			"doesn't provide them");
	}

	min_ = nytl::vec::cw::min(min_, multPos(matrix, p.min));
	max_ = nytl::vec::cw::max(max_, multPos(matrix, p.max));
}

void Scene::init(InitData& data, const WorkBatcher& wb, vk::ImageView dummyView) {
	dlg_assert(images_.size() == data.images.size());

	data.stage.init(data.initStage);
	materialsBuf_.init(data.initMaterials);
	modelsBuf_.init(data.initModels);
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
		doi::write(span, mat);
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
	skip(tc1Span, sizeof(Index) * data.indexCount);
	auto tc1Off = tc1Span.data() - stageMap.ptr();

	auto tc0Span = tc1Span;
	skip(tc0Span, sizeof(nytl::Vec2f) * data.tc1Count);
	auto vertexSpan = tc0Span;;
	skip(vertexSpan, sizeof(nytl::Vec2f) * data.tc0Count);

	auto vertexCount = 0u;
	auto indexCount = 0u;
	auto uploadPrimitive = [&](Primitive& primitive) {
		doi::write(tc0Span, nytl::as_bytes(nytl::span(primitive.texCoords0)));
		doi::write(tc1Span, nytl::as_bytes(nytl::span(primitive.texCoords1)));
		doi::write(vertexSpan, nytl::as_bytes(nytl::span(primitive.vertices)));
		doi::write(indexSpan, nytl::as_bytes(nytl::span(primitive.indices)));

		primitive.firstIndex = indexCount;
		primitive.vertexOffset = vertexCount;
		indexCount += primitive.indexCount;
		vertexCount += primitive.vertexCount;
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

	copy.srcOffset = data.stage.offset() + indexOff;
	copy.dstOffset = indices_.offset();
	copy.size = indices_.size();
	vk::cmdCopyBuffer(wb.cb, data.stage.buffer(),
		indices_.buffer(), {{copy}});

	copy.srcOffset = data.stage.offset() + tc1Off;
	copy.dstOffset = vertices_.offset();
	copy.size = vertices_.size();
	vk::cmdCopyBuffer(wb.cb, data.stage.buffer(),
		vertices_.buffer(), {{copy}});

	// upload matrices for all primitives to modelsBuf
	span = vertexSpan; // last offset
	copy.srcOffset = data.stage.offset() + vertexSpan.data() - stageMap.ptr();
	for(auto& primitive : primitives_) {
		write(span, primitive.matrix);
		auto normalMatrix = nytl::Mat4f(transpose(inverse(primitive.matrix)));
		normalMatrix[3][0] = primitive.material;
		normalMatrix[3][1] = primitive.id;
		write(span, normalMatrix);
	}

	copy.dstOffset = modelsBuf_.offset();
	copy.size = modelsBuf_.size();
	vk::cmdCopyBuffer(wb.cb, data.stage.buffer(),
		modelsBuf_.buffer(), {{copy}});

	stageMap.flush();

	// upload opaque draw commands
	// TODO: should be done in updateDevice/at runtime; support culling
	auto idmap = modelIDs_.memoryMap();
	auto idspan = idmap.span();
	auto cmdmap = cmds_.memoryMap();
	auto cmdspan = cmdmap.span();
	for(auto i = 0u; i < primitives_.size(); ++i) {
		auto& p = primitives_[i];
		if(materials_[p.material].blend()) { // not opaque
			continue;
		}

		doi::write(idspan, u32(i));

		vk::DrawIndexedIndirectCommand cmd;
		cmd.indexCount = p.indexCount;
		cmd.vertexOffset = p.vertexOffset;
		cmd.instanceCount = 1;
		cmd.firstInstance = 0;
		cmd.firstIndex = p.firstIndex;
		doi::write(cmdspan, cmd);
	}

	idmap.flush();
	cmdmap.flush();

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
	dsu.storage({{{modelsBuf_}}});
	dsu.storage({{{materialsBuf_}}});
	dsu.image(images);
	dsu.sampler(samplers);

	vpp::DescriptorSetUpdate bdsu(blendDs_);
	bdsu.storage({{{blendModelIDs_}}});
	bdsu.storage({{{modelsBuf_}}});
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

void Scene::updateDevice(nytl::Mat4f proj) {
	if(!blendCount_) {
		return;
	}

	// TODO: whole method rather inefficient atm
	std::vector<const Primitive*> blendPrims;
	for(auto& p : primitives_) {
		if(materials_[p.material].blend()) {
			blendPrims.push_back(&p);
		}
	}
	dlg_assert(blendPrims.size() == blendCount_);

	// sort in z-descending order
	auto cmp = [&](const Primitive* p1, const Primitive* p2) {
		auto c1 = multPos(p1->matrix, 0.5f * (p1->min + p1->max));
		auto c2 = multPos(p2->matrix, 0.5f * (p2->min + p2->max));
		return multPos(proj, c1).z > multPos(proj, c2).z;
	};
	std::sort(blendPrims.begin(), blendPrims.end(), cmp);

	auto idmap = blendModelIDs_.memoryMap();
	auto idspan = idmap.span();
	auto cmdmap = blendCmds_.memoryMap();
	auto cmdspan = cmdmap.span();
	for(auto* pp : blendPrims) {
		auto& p = *pp;
		// TODO: using id - 1 is a more of a hack i guess...
		doi::write(idspan, u32(p.id - 1));

		vk::DrawIndexedIndirectCommand cmd;
		cmd.indexCount = p.indexCount;
		cmd.vertexOffset = p.vertexOffset;
		cmd.instanceCount = 1;
		cmd.firstInstance = 0;
		cmd.firstIndex = p.firstIndex;
		doi::write(cmdspan, cmd);
	}

	idmap.flush();
	cmdmap.flush();
}

void Scene::render(vk::CommandBuffer cb, vk::PipelineLayout pl, bool blend) const {
	if(blend && !blendCount_) {
		return;
	}

	cmdBindGraphicsDescriptors(cb, pl, 1, {blend ? blendDs_ : ds_});
	vk::cmdBindIndexBuffer(cb, indices_.buffer(), indices_.offset(),
		vk::IndexType::uint32);

	auto bufs = {
		vertices_.buffer().vkHandle(), // pos+normal
		vertices_.buffer().vkHandle(), // tc0
		vertices_.buffer().vkHandle(), // tc1
	};
	auto offsets = {
		vertices_.offset() + vertexOffset_, // pos+normal
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

const vk::PipelineVertexInputStateCreateInfo& Scene::vertexInfo() {
	static constexpr auto stride = sizeof(Primitive::Vertex);
	static constexpr vk::VertexInputBindingDescription bindings[3] = {
		{0, stride, vk::VertexInputRate::vertex},
		{1, sizeof(float) * 2, vk::VertexInputRate::vertex}, // texCoords0
		{2, sizeof(float) * 2, vk::VertexInputRate::vertex}, // texCoords1
	};

	static constexpr vk::VertexInputAttributeDescription attributes[4] = {
		{0, 0, vk::Format::r32g32b32a32Sfloat, 0}, // pos
		{1, 0, vk::Format::r32g32b32a32Sfloat, sizeof(float) * 3}, // normal
		{2, 1, vk::Format::r32g32Sfloat, 0}, // texCoord0
		{3, 2, vk::Format::r32g32Sfloat, 0}, // texCoord1
	};

	static const vk::PipelineVertexInputStateCreateInfo ret = {{},
		3, bindings,
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
