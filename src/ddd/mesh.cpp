#include "mesh.hpp"
#include <stage/bits.hpp>
#include <stage/gltf.hpp>

#include <vpp/vk.hpp>
#include <vpp/bufferOps.hpp>

#include <dlg/dlg.hpp>

Mesh::Mesh(const vpp::Device& dev,
		const tinygltf::Model& model, const tinygltf::Mesh& mesh,
		const vpp::TrDsLayout& dsLayout) {
	if(mesh.primitives.empty()) {
		throw std::runtime_error("No primitive to render");
	}

	auto& primitive = mesh.primitives[0];
	auto ip = primitive.attributes.find("POSITION");
	auto in = primitive.attributes.find("NORMAL");
	if(ip == primitive.attributes.end() || in == primitive.attributes.end()) {
		throw std::runtime_error("primitve doesn't have POSITION or NORMAL");
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
	vertices = {dev.bufferAllocator(), size, usage, 0u, devMem};
	indexCount = ia.count;
	vertexCount = na.count;

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
		region.dstOffset = vertices.offset();
		region.srcOffset = stage.offset();
		region.size = size;
		vk::cmdCopyBuffer(cb, stage.buffer(), vertices.buffer(), {region});
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
	ubo = {dev.bufferAllocator(), size, usage, 0u, hostMem};

	{
		auto map = ubo.memoryMap();
		auto span = map.span();

		// write ubo
		doi::write(span, nytl::identity<4, float>()); // model matrix
		doi::write(span, nytl::identity<4, float>()); // normal matrix
	}

	// descriptor
	ds = {dev.descriptorAllocator(), dsLayout};
	auto& mubo = ubo;
	vpp::DescriptorSetUpdate odsu(ds);
	odsu.uniform({{mubo.buffer(), mubo.offset(), mubo.size()}});
	vpp::apply({odsu});
}

void Mesh::render(vk::CommandBuffer cb, vk::PipelineLayout pipeLayout) {
	auto iOffset = vertices.offset();
	auto vOffset = iOffset + indexCount * sizeof(std::uint32_t);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pipeLayout, 1, {ds}, {});
	vk::cmdBindVertexBuffers(cb, 0, 1, {vertices.buffer()}, {vOffset});
	vk::cmdBindIndexBuffer(cb, vertices.buffer(), iOffset, vk::IndexType::uint32);
	vk::cmdDrawIndexed(cb, indexCount, 1, 0, 0, 0);
}

void Mesh::updateDevice() {
	auto map = ubo.memoryMap();
	auto span = map.span();
	doi::write(span, matrix);
	auto normalMatrix = nytl::Mat4f(transpose(inverse(matrix)));
	doi::write(span, normalMatrix);
}
