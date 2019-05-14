#include <stage/scene/primitive.hpp>
#include <stage/scene/material.hpp>
#include <stage/scene/shape.hpp>
#include <stage/bits.hpp>
#include <stage/gltf.hpp>

#include <vpp/vk.hpp>
#include <vpp/submit.hpp>
#include <vpp/bufferOps.hpp>

#include <dlg/dlg.hpp>

namespace doi {

// NOTE: currently some code duplication between constructors
// hard to factor out into one function though
// TODO: add deferred version of this constructor as well?
Primitive::Primitive(const WorkBatcher& wb, const Shape& shape,
		const vpp::TrDsLayout& dsLayout, unsigned material,
		const nytl::Mat4f& mat, unsigned id) : matrix(mat), id_(id),
			material_(material) {

	auto& dev = dsLayout.device();
	dlg_assert(shape.normals.size() == shape.positions.size());

	auto size = shape.indices.size() * sizeof(std::uint32_t);
	size += shape.positions.size() * sizeof(nytl::Vec3f);
	size += shape.normals.size() * sizeof(nytl::Vec3f);

	auto devMem = dev.deviceMemoryTypes();
	auto hostMem = dev.hostMemoryTypes();
	auto usage = vk::BufferUsageBits::vertexBuffer |
		vk::BufferUsageBits::indexBuffer |
		vk::BufferUsageBits::transferDst;
	vertices_ = {wb.alloc.bufDevice, size, usage, devMem, 8u};
	indexCount_ = shape.indices.size();
	vertexCount_ = shape.positions.size();

	// fill it
	{
		auto stage = vpp::SubBuffer{wb.alloc.bufStage, size,
			vk::BufferUsageBits::transferSrc, hostMem};
		auto map = stage.memoryMap();
		auto span = map.span();

		// write indices
		for(auto idx : shape.indices) {
			doi::write(span, idx);
		}

		// write vertices and normals
		auto& pr = shape.positions;
		auto& nr = shape.normals;
		for(auto pit = pr.begin(), nit = nr.begin();
				pit != pr.end() && nit != nr.end();
				++nit, ++pit) {
			doi::write(span, *pit);
			doi::write(span, *nit);
		}

		// upload
		vk::BufferCopy region;
		region.dstOffset = vertices_.offset();
		region.srcOffset = stage.offset();
		region.size = size;
		vk::cmdCopyBuffer(wb.cb, stage.buffer(), vertices_.buffer(), {{region}});
	}

	// ubo
	size = Primitive::uboSize; // ubo
	usage = vk::BufferUsageBits::uniformBuffer;
	ubo_ = {wb.alloc.bufHost, size, usage, hostMem};
	updateDevice();

	// descriptor
	ds_ = {dev.descriptorAllocator(), dsLayout};
	auto& mubo = ubo_;
	vpp::DescriptorSetUpdate odsu(ds_);
	odsu.uniform({{mubo.buffer(), mubo.offset(), mubo.size()}});
}

Primitive::Primitive(InitData& data, const WorkBatcher& wb,
		const tinygltf::Model& model, const tinygltf::Primitive& primitive,
		const vpp::TrDsLayout& dsLayout, unsigned material,
		const nytl::Mat4f& mat, unsigned id) :
			matrix(mat), id_(id), material_(material) {

	auto& dev = dsLayout.device();
	auto ip = primitive.attributes.find("POSITION");
	auto in = primitive.attributes.find("NORMAL");
	auto iuv = primitive.attributes.find("TEXCOORD_0"); // TODO: support more coords
	if(ip == primitive.attributes.end() || in == primitive.attributes.end()) {
		throw std::runtime_error("primitve doesn't have POSITION or NORMAL");
	}

	auto& pa = model.accessors[ip->second];
	auto& na = model.accessors[in->second];

	// TODO: fix that by inserting simple indices or set local flag
	// to just use vkcmddraw
	if(primitive.indices < 0) {
		throw std::runtime_error("Only models with indices supported");
	}
	auto& ia = model.accessors[primitive.indices];

	auto* uva = iuv == primitive.attributes.end() ?
		nullptr : &model.accessors[iuv->second];

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
	vertices_ = {data.initVert, wb.alloc.bufDevice, size, usage, devMem, 8u};
	indexCount_ = ia.count;
	vertexCount_ = na.count;

	// write indices
	data.vertData.resize(size);
	auto span = nytl::Span<std::byte>(data.vertData);
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

	auto stageSize = size;
	if(uva) {
		auto uvSize = uva->count * sizeof(nytl::Vec2f); // uv coords
		auto uvUsage = vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::transferDst;
		uv_ = {data.initUv, wb.alloc.bufDevice, uvSize, uvUsage, devMem, 8u};
		stageSize += uvSize;

		data.uvData.resize(uvSize);
		auto span = nytl::Span<std::byte>(data.uvData);
		auto uvr = doi::range<2, float>(model, *uva);
		for(auto uv : uvr) {
			doi::write(span, uv);
		}
	}

	data.stage = vpp::SubBuffer{data.initStage, wb.alloc.bufStage,
		stageSize, vk::BufferUsageBits::transferSrc, hostMem};

	// ubo
	usage = vk::BufferUsageBits::uniformBuffer;
	ubo_ = {data.initUbo, wb.alloc.bufHost, Primitive::uboSize, usage,
		hostMem, 16u};

	// descriptor
	ds_ = {data.initDs, wb.alloc.ds, dsLayout};
}

void Primitive::init(InitData& data, vk::CommandBuffer cb) {
	vertices_.init(data.initVert);
	ubo_.init(data.initUbo);
	data.stage.init(data.initStage);

	auto map = data.stage.memoryMap();
	auto span = map.span();
	doi::write(span, data.vertData.data(), data.vertData.size());

	vk::BufferCopy region;
	region.dstOffset = vertices_.offset();
	region.srcOffset = data.stage.offset();
	region.size = data.vertData.size();
	vk::cmdCopyBuffer(cb, data.stage.buffer(), vertices_.buffer(), {{region}});

	if(!data.uvData.empty()) {
		uv_.init(data.initUv);
		auto offset = span.data() - map.ptr();
		doi::write(span, data.uvData.data(), data.uvData.size());

		vk::BufferCopy region;
		region.dstOffset = uv_.offset();
		region.srcOffset = data.stage.offset() + offset;
		region.size = data.uvData.size();
		vk::cmdCopyBuffer(cb, data.stage.buffer(), uv_.buffer(), {{region}});
	}

	updateDevice();

	// descriptor
	ds_.init(data.initDs);
	vpp::DescriptorSetUpdate odsu(ds_);
	odsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
}

void Primitive::render(vk::CommandBuffer cb,
		vk::PipelineLayout pipeLayout) const {
	auto iOffset = vertices_.offset();
	auto vOffset = iOffset + indexCount_ * sizeof(std::uint32_t);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pipeLayout, 2, {{ds_.vkHandle()}}, {});

	vk::cmdBindVertexBuffers(cb, 0, 1, {vertices_.buffer()}, {vOffset});

	if(uv_.size() > 0) {
		vk::cmdBindVertexBuffers(cb, 1, 1, {uv_.buffer()}, {uv_.offset()});
	} else {
		// in this case we bind the vertex (pos + normal) buffer as
		// uv buffer. This is obviously utter garbage but allows us
		// to use just one pipeline. If we land here, the material of
		// this primitive uses no textures so uv coords are irrevelant.
		// The size of position+normals will always be larger than of uv coords.
		vk::cmdBindVertexBuffers(cb, 1, 1, {vertices_.buffer()}, {vOffset});
	}

	vk::cmdBindIndexBuffer(cb, vertices_.buffer(), iOffset, vk::IndexType::uint32);
	vk::cmdDrawIndexed(cb, indexCount_, 1, 0, 0, 0);
}

void Primitive::updateDevice() {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	doi::write(span, matrix);
	auto normalMatrix = nytl::Mat4f(transpose(inverse(matrix)));
	doi::write(span, normalMatrix);
	doi::write(span, std::uint32_t(id_));
}

} // namespace gltf
