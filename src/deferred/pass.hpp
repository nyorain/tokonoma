#pragma once

#include <stage/defer.hpp>
#include <stage/bits.hpp>
#include <stage/texture.hpp>
#include <stage/types.hpp>

#include <vpp/vk.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/handles.hpp>
#include <vpp/image.hpp>

// Utilities for modular render passes.

using namespace doi::types;

// TODO: move to stage/render.hpp
// Pipeline specialization info the compute group size.
struct ComputeGroupSizeSpec {
	inline ComputeGroupSizeSpec(u32 x, u32 y, u32 idx = 0, u32 idy = 1) {
		entries = {{
			{idx, 0, 4u},
			{idy, 4u, 4u},
		}};

		auto span = nytl::Span<std::byte>(data);
		doi::write(span, x);
		doi::write(span, y);

		spec.dataSize = sizeof(data);
		spec.pData = data.data();
		spec.mapEntryCount = entries.size();
		spec.pMapEntries = entries.data();
	}

	std::array<std::byte, 8> data;
	std::array<vk::SpecializationMapEntry, 2> entries;
	vk::SpecializationInfo spec;
};

struct PassCreateInfo {
	const doi::WorkBatcher& wb;
	vk::Format depthFormat;

	struct {
		const vpp::TrDsLayout& scene;
		const vpp::TrDsLayout& material;
		const vpp::TrDsLayout& primitive;
		const vpp::TrDsLayout& light;
	} dsLayouts;

	struct {
		vk::Sampler linear;
		vk::Sampler nearest;
	} samplers;

	vk::ShaderModule fullscreenVertShader;
};

struct SyncScope {
	vk::PipelineStageFlags stages {};
	vk::ImageLayout layout {vk::ImageLayout::undefined};
	vk::AccessFlags access {};

	static inline SyncScope fragmentSampled() {
		return {
			vk::PipelineStageBits::fragmentShader,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::AccessBits::shaderRead,
		};
	}
	static inline SyncScope computeSampled() {
		return {
			vk::PipelineStageBits::computeShader,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::AccessBits::shaderRead,
		};
	}
	static inline SyncScope computeReadWrite() {
		return {
			vk::PipelineStageBits::computeShader,
			vk::ImageLayout::general,
			vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite,
		};
	}
};

inline bool operator==(SyncScope a, SyncScope b) {
	return a.stages == b.stages &&
		a.layout == b.layout &&
		a.access == b.access;
}

inline bool operator!=(SyncScope a, SyncScope b) {
	return a.stages != b.stages ||
		a.layout != b.layout ||
		a.access != b.access;
}

inline SyncScope& operator|=(SyncScope& a, SyncScope b) {
	if(a.layout == vk::ImageLayout::undefined) {
		a.layout = b.layout;
	} else {
		dlg_assert(b.layout == vk::ImageLayout::undefined ||
			a.layout == b.layout);
	}

	a.stages |= b.stages;
	a.access |= b.access;
	return a;
}

inline SyncScope operator|(SyncScope a, SyncScope b) {
	return a |= b;
}

struct ImageBarrier {
	vk::Image image {};
	SyncScope src {};
	SyncScope dst {};
	vk::ImageSubresourceRange subres = {
		vk::ImageAspectBits::color, 0, 1, 0, 1
	};
};

inline void barrier(vk::CommandBuffer cb, nytl::Span<const ImageBarrier> barriers) {
	dlg_assert(!barriers.empty());

	std::vector<vk::ImageMemoryBarrier> vkBarriers;
	vkBarriers.reserve(barriers.size());
	vk::PipelineStageFlags srcStages = {};
	vk::PipelineStageFlags dstStages = {};
	for(auto& b : barriers) {
		if(!b.image) { // empty barriers allowed
			continue;
		}

		srcStages |= b.src.stages;
		dstStages |= b.dst.stages;

		auto& barrier = vkBarriers.emplace_back();
		barrier.image = b.image;
		barrier.srcAccessMask = b.src.access;
		barrier.oldLayout = b.src.layout;
		barrier.dstAccessMask = b.dst.access;
		barrier.newLayout = b.dst.layout;
		barrier.subresourceRange = b.subres;
	}

	if(vkBarriers.empty()) {
		return;
	}

	vk::cmdPipelineBarrier(cb, srcStages, dstStages, {}, {}, {}, vkBarriers);
}

inline void barrier(vk::CommandBuffer cb, vk::Image image, SyncScope src,
		SyncScope dst, vk::ImageSubresourceRange subres =
			{vk::ImageAspectBits::color, 0, 1, 0, 1}) {
	vk::ImageMemoryBarrier barrier;
	barrier.image = image;
	barrier.srcAccessMask = src.access;
	barrier.oldLayout = src.layout;
	barrier.dstAccessMask = dst.access;
	barrier.newLayout = dst.layout;
	barrier.subresourceRange = subres;
	vk::cmdPipelineBarrier(cb, src.stages, dst.stages, {}, {}, {}, {{barrier}});
}

struct QueryPoolTimestamp {
	vk::QueryPool pool;
	unsigned query;

	void write(vk::CommandBuffer cb, vk::PipelineStageBits stage =
			vk::PipelineStageBits::topOfPipe) {
		vk::cmdWriteTimestamp(cb, stage, pool, query);
	}
};

