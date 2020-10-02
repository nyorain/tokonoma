#pragma once

#include <vpp/fwd.hpp>
#include <vpp/handles.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/image.hpp>

namespace rvg2 {

class Context;

// Potentially supported device features/extensions.
enum class DeviceFeature {
	// This feature is required.
	// It's only in this list (and must be passed when creating a context)
	// to make this explicit.
	uniformDynamicArrayIndexing = (1u << 0),

	// Optional, allows to batch draw calls into one indirect call which
	// can slightly improve performance.
	multidrawIndirect = (1u << 1),

	// Optional, allows more updates (e.g. pool buffer reallocation) without
	// requiring a command buffer re-record. Also allows a lot more textures
	// per paint pool, potentially minimizing the number of state changes
	// when many textures are needed for drawing.
	descriptorIndexing = (1u << 2),

	// Optional, makes plane clipping more efficient.
	// The number of supported clip planes will be queried and used
	// automatically in the vertex shader instead of performing this
	// custom clipping in the fragment shader.
	clipDistance = (1u << 3),
};

/// Control various aspects of a context.
/// You have to set those members that have no default value to valid
/// values.
struct ContextSettings {
	/// The renderpass and subpass in which it will be used.
	/// Must be specified for pipeline creation.
	vk::RenderPass renderPass;
	unsigned subpass;
	unsigned uploadQueueFamily;

	/// Which device features are supported.
	/// See DeviceFeatures for more information.
	nytl::Flags<DeviceFeature> deviceFeatures;

	/// The multisample bits to use for the pipelines.
	vk::SampleCountBits samples {};

	/// The pipeline cache to use.
	/// Will use no cache if left empty.
	vk::PipelineCache pipelineCache {};
};

class Context {
public:
	/// Creates a new context for given device and settings.
	/// The device must remain valid for the lifetime of this context.
	/// You should generally avoid to create multiple contexts for one
	/// device since a context creates and manages expensive resources
	/// like pipelines.
	Context(vpp::Device&, const ContextSettings&);
	~Context() = default;

	Context(Context&&) = delete;
	Context& operator=(Context&&) = delete;

	vk::Semaphore endFrameSubmit(vk::SubmitInfo&);
	const vk::CommandBuffer* endFrameWork();

	vk::CommandBuffer recordableUploadCmdBuf();
	void keepAlive(vpp::SubBuffer);
	void keepAlive(vpp::ViewableImage);

	vpp::BufferAllocator& bufferAllocator() const;
	vpp::DescriptorAllocator& dsAllocator() const;
	vpp::DeviceMemoryAllocator& devMemAllocator() const;

	const vpp::Device& device() const { return dev_; }
	const vpp::TrDsLayout& dsLayout() const { return dsLayout_; }
	vk::Sampler sampler() const { return sampler_; }
	vk::PipelineLayout pipeLayout() const { return pipeLayout_; }
	vk::Pipeline pipe() const { return pipe_; }
	vk::ImageView dummyImageView() const { return dummyImage_.vkImageView(); }
	vpp::BufferSpan dummyBuffer() const { return dummyBuffer_; }

	const ContextSettings& settings() const { return settings_; }
	unsigned numBindableTextures() const { return numBindableTextures_; }
	bool multidrawIndirect() const {
		return settings().deviceFeatures & DeviceFeature::multidrawIndirect;
	}
	bool descriptorIndexing() const {
		return settings().deviceFeatures & DeviceFeature::descriptorIndexing;
	}

private:
	const vpp::Device& dev_;

	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::Sampler sampler_;

	vpp::ViewableImage dummyImage_;
	vpp::SubBuffer dummyBuffer_;

	nytl::Flags<DeviceFeature> features_;

	vpp::Semaphore uploadSemaphore_;
	vpp::CommandBuffer uploadCb_;
	bool uploadWork_ {};

	struct KeepAlive {
		std::vector<vpp::SubBuffer> bufs;
		std::vector<vpp::ViewableImage> imgs;
	};

	KeepAlive keepAlive_;
	KeepAlive keepAliveLast_;

	ContextSettings settings_;
	unsigned numBindableTextures_;
};

NYTL_FLAG_OPS(DeviceFeature)

} // namespace rvg2
