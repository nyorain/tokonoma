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

/*
const vk::PipelineVertexInputStateCreateInfo& Primitive::vertexInfo() {
	static constexpr auto stride = sizeof(doi::Primitive::Vertex);
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

	auto inf = std::numeric_limits<float>::infinity();
	min_ = {inf, inf, inf};
	max_ = {-inf, -inf, -inf};

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
			min_ = nytl::vec::cw::min(min_, *pit);
			max_ = nytl::vec::cw::max(max_, *pit);

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
	auto itc0 = primitive.attributes.find("TEXCOORD_0");
	auto itc1 = primitive.attributes.find("TEXCOORD_1");

	// TODO: we could manually compute normals, flat normals are good
	// enough in this case. But we could also an algorith smooth normals
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

	auto* tc0a = itc0 == primitive.attributes.end() ?
		nullptr : &model.accessors[itc0->second];
	auto* tc1a = itc1 == primitive.attributes.end() ?
		nullptr : &model.accessors[itc1->second];

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
	auto inf = std::numeric_limits<float>::infinity();
	min_ = {inf, inf, inf};
	max_ = {-inf, -inf, -inf};

	auto pr = doi::range<3, float>(model, pa);
	auto nr = doi::range<3, float>(model, na);
	for(auto pit = pr.begin(), nit = nr.begin();
			pit != pr.end() && nit != nr.end();
			++nit, ++pit) {
		min_ = nytl::vec::cw::min(min_, *pit);
		max_ = nytl::vec::cw::max(max_, *pit);
		doi::write(span, *pit);
		doi::write(span, *nit);
	}

	auto stageSize = size;
	auto texCoordsUsage = vk::BufferUsageBits::vertexBuffer |
		vk::BufferUsageBits::transferDst;
	if(tc0a) {
		dlg_assert(tc0a->count == pa.count);
		auto size = tc0a->count * sizeof(nytl::Vec2f); // uv coords
		texCoords0_ = {data.initTexCoords0, wb.alloc.bufDevice, size,
			texCoordsUsage, devMem, 8u};
		stageSize += size;

		data.texCoord0Data.resize(size);
		auto span = nytl::Span<std::byte>(data.texCoord0Data);
		auto uvr = doi::range<2, float>(model, *tc0a);
		for(auto uv : uvr) {
			doi::write(span, uv);
		}
	}
	if(tc1a) {
		dlg_assert(tc1a->count == pa.count);
		auto size = tc1a->count * sizeof(nytl::Vec2f); // uv coords
		texCoords1_ = {data.initTexCoords1, wb.alloc.bufDevice, size,
			texCoordsUsage, devMem, 8u};
		stageSize += size;

		data.texCoord1Data.resize(size);
		auto span = nytl::Span<std::byte>(data.texCoord1Data);
		auto uvr = doi::range<2, float>(model, *tc1a);
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

	if(!data.texCoord0Data.empty()) {
		texCoords0_.init(data.initTexCoords0);
		auto offset = span.data() - map.ptr();
		doi::write(span, data.texCoord0Data.data(), data.texCoord0Data.size());

		vk::BufferCopy region;
		region.dstOffset = texCoords0_.offset();
		region.srcOffset = data.stage.offset() + offset;
		region.size = data.texCoord0Data.size();
		vk::cmdCopyBuffer(cb, data.stage.buffer(),
			texCoords0_.buffer(), {{region}});
	}
	if(!data.texCoord1Data.empty()) {
		texCoords1_.init(data.initTexCoords1);
		auto offset = span.data() - map.ptr();
		doi::write(span, data.texCoord1Data.data(), data.texCoord1Data.size());

		vk::BufferCopy region;
		region.dstOffset = texCoords1_.offset();
		region.srcOffset = data.stage.offset() + offset;
		region.size = data.texCoord1Data.size();
		vk::cmdCopyBuffer(cb, data.stage.buffer(),
			texCoords1_.buffer(), {{region}});
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

	// for each texture coordinate that is not provided, we simply
	// bind the vertices_ buffer as dummy buffer (vulkan needs something),
	// but we won't use that data in the shaders.
	// The size of position+normals will always be larger than of uv coords.
	auto vb = vertices_.buffer().vkHandle();
	std::array<vk::Buffer, 3> bufs = {vb, vb, vb};
	std::array<vk::DeviceSize, 3> offsets = {vOffset, vOffset, vOffset};

	if(texCoords0_.size() > 0) {
		bufs[1] = texCoords0_.buffer();
		offsets[1] = texCoords0_.offset();
	}
	if(texCoords1_.size() > 0) {
		bufs[2] = texCoords1_.buffer();
		offsets[2] = texCoords1_.offset();
	}

	vk::cmdBindVertexBuffers(cb, 0, bufs, offsets);
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
*/

} // namespace gltf
