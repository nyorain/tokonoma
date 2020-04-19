#pragma once

// Contains utility for writing render and compute passes.

#include <tkn/types.hpp>
#include <vpp/fwd.hpp>
#include <vkpp/structs.hpp>
#include <vkpp/enums.hpp>

#include <initializer_list>
#include <array>
#include <numeric>

namespace tkn {

/// Shortcuts for vk::cmdBindDescriptorSets
void cmdBindGraphicsDescriptors(vk::CommandBuffer, vk::PipelineLayout,
	unsigned first, std::initializer_list<vk::DescriptorSet>,
	std::initializer_list<uint32_t> = {});
void cmdBindComputeDescriptors(vk::CommandBuffer, vk::PipelineLayout,
	unsigned first, std::initializer_list<vk::DescriptorSet>,
	std::initializer_list<uint32_t> = {});

void cmdCopyBuffer(vk::CommandBuffer, vpp::BufferSpan src, vpp::BufferSpan dst);

/// Returns a PipelineColorBlendAttachmentState that disables blending
/// but allows all components to be written.
const vk::PipelineColorBlendAttachmentState& noBlendAttachment();

/// Returns a PipelineColorBlendAttachmentState with default oneMinusSrcAlpha
/// blending (and replacing alpha blending) and full write mask.
const vk::PipelineColorBlendAttachmentState& defaultBlendAttachment();

/// Returns a blend attachment that has blending disabled and the color
/// write mask set to empty, i.e. it disables all writing to this
/// attachment
const vk::PipelineColorBlendAttachmentState& disableBlendAttachment();

/// Returns the size of mipmap level 'i' for an image that has size 'full'
vk::Extent2D mipmapSize(vk::Extent2D full, unsigned i);

// See downscale command.
struct DownscaleTarget {
	vk::Image image {};
	vk::Format format {};
	vk::ImageLayout layout {};
	unsigned width {};
	unsigned height {};
	unsigned layerCount {1};

	// source scope of writing mipmap level 0
	vk::AccessFlags srcAccess {};
	vk::PipelineStageFlags srcStages {};
};

/// Pass that dynamically generates mipmap levels of a color image.
/// Doesn't work for image with depth format since those don't allow
/// linear filtered blitting in vulkan.
/// The given target must have at least genLevels + 1 mipLevels.
/// Undefined for genLevels == 0 (doesn't make sense).
/// Will overwrite the current contents of the first 'genLevels' levels.
/// Afterwards, the first 'genLevels' levels will be transferSrcOptimal
/// layout, with AccessBits::transferRead access in transfer stage.
/// Not optimal for non-power-of-two textures, see source code
/// for more details/example.
void downscale(vk::CommandBuffer cb, const DownscaleTarget& target,
		unsigned genLevels);
void downscale(const vpp::Device& dev, vk::CommandBuffer cb,
		const DownscaleTarget& target, unsigned genLevels);

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

bool operator==(SyncScope a, SyncScope b);
bool operator!=(SyncScope a, SyncScope b);

// Only possible when both have same layout (or one has undefined layout).
SyncScope& operator|=(SyncScope& a, SyncScope b);
SyncScope operator|(SyncScope a, SyncScope b);

struct ImageBarrier {
	vk::Image image {};
	SyncScope src {};
	SyncScope dst {};
	vk::ImageSubresourceRange subres = {
		vk::ImageAspectBits::color, 0, 1, 0, 1
	};
};

void barrier(vk::CommandBuffer cb, nytl::Span<const ImageBarrier> barriers);
void barrier(vk::CommandBuffer cb, vk::Image image, SyncScope src,
		SyncScope dst, vk::ImageSubresourceRange subres =
			{vk::ImageAspectBits::color, 0, 1, 0, 1});

// Pipeline specialization info the compute group size.
// Use it like this: `auto cgss = ComputeGroupSizeSpec<2>({16, 16}, {0, 1})`,
// (local_size_x = 16, local_size_y = 16), declared with constant
// ids 0, 1. Then use `cgss.spec` in the creation of your pipeline.
// When IDs are subsequently numbered (default to starting at 0),
// you can use the shortcut `auto cgss = ComputeGroupSizeSpec(16, 16)`.
template<u32 Size>
struct ComputeGroupSizeSpec {
	ComputeGroupSizeSpec(const std::array<u32, Size>& values,
			const std::array<u32, Size>& constantIDs) {
		for(auto i = 0u; i < Size; ++i) {
			entries[i] = {constantIDs[i], 4 * i, 4u};
			memcpy(data.data() + 4 * i, &values[i], 4u);
		}

		spec.dataSize = sizeof(data);
		spec.pData = data.data();
		spec.mapEntryCount = entries.size();
		spec.pMapEntries = entries.data();
	}

	ComputeGroupSizeSpec(const std::array<u32, Size>& vs, u32 firstID = 0)
		: ComputeGroupSizeSpec(vs, iotaArray(firstID)) {}

	// has science gone too far?
	template<typename... Args, typename = std::enable_if_t<
		std::is_convertible_v<std::common_type_t<Args...>, u32>>>
	ComputeGroupSizeSpec(Args&&... args)
		: ComputeGroupSizeSpec({u32(args)...}, iotaArray(0)) {}

	vk::SpecializationInfo spec;

	// private
	static std::array<u32, Size> iotaArray(u32 firstID) {
		std::array<u32, Size> ret;
		std::iota(ret.begin(), ret.end(), firstID);
		return ret;
	}

	std::array<std::byte, 4 * Size> data;
	std::array<vk::SpecializationMapEntry, Size> entries;
};

template<typename... Args> ComputeGroupSizeSpec(Args&&...)
	-> ComputeGroupSizeSpec<sizeof...(Args)>;

} // namespace tkn
