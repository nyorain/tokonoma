#pragma once

#include <tkn/defer.hpp>
#include <tkn/bits.hpp>
#include <tkn/texture.hpp>
#include <tkn/types.hpp>
#include <tkn/render.hpp>
#include <tkn/render.hpp>
#include <tkn/types.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/handles.hpp>
#include <vpp/image.hpp>

namespace tkn {

// Pass that performs a downscale and bias/scaling.
// Usually this output is needed for lens-based post-processing
// effects such as lens flare or bloom.
class HighLightPass {
public:
	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initUbo;
	};

	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

public:
	static constexpr auto groupDimSize = 8; // 2D; 64 WorkGroup invocations

	/// How many levels the created image should have.
	/// In any case, will only fill the first level.
	/// Changing this requires buffer recreation.
	unsigned numLevels = 1u;

	struct {
		float bias = -0.4;
		float scale = 1.5;
	} params;

public:
	HighLightPass() = default;
	void create(InitData&, WorkBatcher& wb, vk::Sampler linear);
	void init(InitData&);

	void createBuffers(InitBufferData& data, WorkBatcher&, vk::Extent2D size);
	void initBuffers(InitBufferData& data, vk::ImageView lightInput,
		vk::ImageView emissionInput);

	void record(vk::CommandBuffer cb, vk::Extent2D);
	void updateDevice();

	SyncScope dstScopeEmission() const;
	SyncScope dstScopeLight() const;
	SyncScope srcScopeTarget() const; // only for the first level

	// The target will have 1/2 the size of the main buffers
	const auto& target() const { return target_; }

protected:
	vpp::SubBuffer ubo_;
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::TrDs ds_;

	vpp::ViewableImage target_;
};

} // namespace tkn

