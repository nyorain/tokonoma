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

// ArHoseSkyModel.h
struct ArHosekSkyModelState;

namespace tkn {

// Loads a static environment map (including irradiance and pre-convoluted
// levels for specular IBL).
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

	// Requires the camera ds to be bound as set 0 (holding the
	// position-invariant transform matrix).
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

// Dynamic sky environment based on an analytical sky model.
class SkyEnvironment {
public:
	struct Cache {

	};
};

} // namespace tkn
