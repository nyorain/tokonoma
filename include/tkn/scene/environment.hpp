#pragma once

#include <tkn/texture.hpp>
#include <tkn/types.hpp>
#include <tkn/render.hpp>
#include <vpp/fwd.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/mat.hpp>
#include <nytl/vec.hpp>
#include <nytl/stringParam.hpp>

namespace tkn {

/// Indices to create a box that has it's front faces on the inside,
/// (as needed for a skybox) with the skybox.vert shader that generates
/// the positions.
constexpr std::array<u16, 36> boxInsideIndices = {
	0, 1, 2,  2, 1, 3, // front
	1, 5, 3,  3, 5, 7, // right
	2, 3, 6,  6, 3, 7, // top
	4, 0, 6,  6, 0, 2, // left
	4, 5, 0,  0, 5, 1, // bottom
	5, 4, 7,  7, 4, 6, // back
};

/// Loads environment
/// Not movable due to ds layout.
class Environment {
public:
	struct InitData {
		tkn::Texture::InitData initEnvMap;
		tkn::Texture::InitData initIrradiance;
		vpp::TrDs::InitData initDs;
	};

public:
	Environment() = default;
	Environment(Environment&&) = delete;
	Environment& operator=(Environment&&) = delete;

	void create(InitData&, const WorkBatcher& wb, nytl::StringParam envMapPath,
		nytl::StringParam irradiancePath, vk::Sampler linear);
	void create(InitData&, const WorkBatcher& wb,
		std::unique_ptr<ImageProvider> envMap,
		std::unique_ptr<ImageProvider> irradiance,
		vk::Sampler linear);

	void init(InitData&, const WorkBatcher&);
	void createPipe(const vpp::Device&, vk::DescriptorSetLayout camDsLayout,
		vk::RenderPass rp, unsigned subpass, vk::SampleCountBits samples,
		nytl::Span<const vk::PipelineColorBlendAttachmentState>
			battachments = {&defaultBlendAttachment(), 1});

	// Requires caller to bind cube index buffer with boxInsideIndices.
	// Also requires the camera ds to be bound as set 0.
	void render(vk::CommandBuffer cb) const;

	auto& pipeLayout() const { return pipeLayout_; }
	auto& envMap() const { return envMap_; } // contains filtered mipmaps
	auto& irradiance() const { return irradiance_; }
	auto convolutionMipmaps() const { return convolutionMipmaps_; }

protected:
	tkn::Texture envMap_; // contains specular ibl mip maps
	tkn::Texture irradiance_;

	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	unsigned convolutionMipmaps_;
};

} // namespace tkn
