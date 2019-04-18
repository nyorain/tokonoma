#include <stage/scene/primitive.hpp>
#include <stage/scene/material.hpp>
#include <stage/bits.hpp>
#include <stage/gltf.hpp>

#include <vpp/vk.hpp>
#include <vpp/bufferOps.hpp>

#include <dlg/dlg.hpp>

namespace doi {

Primitive::Primitive(const vpp::Device& dev,
		const tinygltf::Model& model, const tinygltf::Primitive& primitive,
		const vpp::TrDsLayout& dsLayout, const Material& material,
		const nytl::Mat4f& mat) {
	this->matrix = mat;
	this->material_ = &material;

	auto ip = primitive.attributes.find("POSITION");
	auto in = primitive.attributes.find("NORMAL");
	auto iuv = primitive.attributes.find("TEXCOORD_0"); // TODO: more coords
	if(ip == primitive.attributes.end() || in == primitive.attributes.end()) {
		throw std::runtime_error("primitve doesn't have POSITION or NORMAL");
	}

	auto& pa = model.accessors[ip->second];
	auto& na = model.accessors[in->second];
	auto& ia = model.accessors[primitive.indices];

	auto* uva = iuv == primitive.attributes.end() ?
		nullptr : &model.accessors[iuv->second];
	if(!uva && material.hasTexture()) {
		throw std::runtime_error("primitive uses texture but has no uv coords");
	}

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
	vertices_ = {dev.bufferAllocator(), size, usage, 0u, devMem};
	indexCount_ = ia.count;
	vertexCount_ = na.count;

	auto stageSize = size;
	if(uva) {
		auto uvSize = uva->count * sizeof(nytl::Vec2f); // uv coords
		auto uvUsage = vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::transferDst;
		uv_ = {dev.bufferAllocator(), uvSize, uvUsage, 0u, devMem};
		stageSize += uvSize;
	}

	// fill it
	{
		auto stage = vpp::SubBuffer{dev.bufferAllocator(), stageSize,
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
		region.dstOffset = vertices_.offset();
		region.srcOffset = stage.offset();
		region.size = size;
		vk::cmdCopyBuffer(cb, stage.buffer(), vertices_.buffer(), {region});

		if(uva) {
			auto offset = span.data() - map.ptr();
			auto uvr = doi::range<2, float>(model, *uva);
			for(auto uv : uvr) {
				doi::write(span, uv);
			}
			dlg_assert(vk::DeviceSize(span.data() - map.ptr() - offset)
				== uv_.size());

			vk::BufferCopy region;
			region.dstOffset = uv_.offset();
			region.srcOffset = stage.offset() + offset;
			region.size = uv_.size();
			vk::cmdCopyBuffer(cb, stage.buffer(), uv_.buffer(), {region});
		}

		vk::endCommandBuffer(cb);

		// execute
		// TODO: could be batched with other work; we wait here
		vk::SubmitInfo submission;
		submission.commandBufferCount = 1;
		submission.pCommandBuffers = &cb.vkHandle();
		qs.wait(qs.add(submission));
	}

	// ubo
	size = Primitive::uboSize; // ubo
	usage = vk::BufferUsageBits::uniformBuffer;
	ubo_ = {dev.bufferAllocator(), size, usage, 0u, hostMem};
	updateDevice();

	// descriptor
	ds_ = {dev.descriptorAllocator(), dsLayout};
	auto& mubo = ubo_;
	vpp::DescriptorSetUpdate odsu(ds_);
	odsu.uniform({{mubo.buffer(), mubo.offset(), mubo.size()}});
	vpp::apply({odsu});
}

void Primitive::render(vk::CommandBuffer cb, vk::PipelineLayout pipeLayout,
		bool shadow) {
	auto iOffset = vertices_.offset();
	auto vOffset = iOffset + indexCount_ * sizeof(std::uint32_t);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		// pipeLayout, 2 - shadow, {ds_}, {});
		pipeLayout, 2, {ds_}, {});
	vk::cmdBindVertexBuffers(cb, 0, 1, {vertices_.buffer()}, {vOffset});

	if(!shadow) {
		material().bind(cb, pipeLayout);
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
}

} // namespace gltf
