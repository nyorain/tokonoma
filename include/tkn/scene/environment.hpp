#pragma once

#include <tkn/texture.hpp>
#include <tkn/types.hpp>
#include <tkn/render.hpp>
#include <tkn/sh.hpp>
#include <vpp/fwd.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/mat.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/stringParam.hpp>

// TODO: use independent SkyboxRenderer for Environment as well.

// ArHoseSkyModel.h
struct ArHosekSkyModelState;

namespace tkn {

class f16;

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

// Renders any skybox, given by a cubemap.
class SkyboxRenderer {
public:
	struct PipeInfo {
		vk::Sampler linear;
		vk::DescriptorSetLayout camDsLayout;
		vk::RenderPass renderPass;

		vk::SampleCountBits samples {vk::SampleCountBits::e1};
		bool reverseDepth {true};
		unsigned subpass {0u};
	};

public:
	SkyboxRenderer() = default;
	SkyboxRenderer(SkyboxRenderer&&) = delete;
	SkyboxRenderer& operator=(SkyboxRenderer&&) = delete;

	void create(const vpp::Device& dev, const PipeInfo&,
		nytl::Span<const vk::PipelineColorBlendAttachmentState>
			battachments = {&defaultBlendAttachment(), 1});
	void render(vk::CommandBuffer cb, vk::DescriptorSet ds);

	const auto& pipeLayout() const { return pipeLayout_; }
	const auto& dsLayout() const { return dsLayout_; }

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

// Dynamic sky environment based on an analytical sky model.
// Produces a cubemap in rgba16fSfloat format.
class Sky {
public:
	static constexpr auto sunSize = nytl::radians(0.27f);
	static constexpr auto up = Vec3f{0.f, 1.f, 0.f};
	static constexpr auto cubemapFormat = vk::Format::r16g16b16a16Sfloat;

	static constexpr auto faceWidth = 64u;
	static constexpr auto faceHeight = 64u;

	struct Baked {
		using Pixel = nytl::Vec4<tkn::f16>; // must match cubemapFormat
		std::array<std::vector<Pixel>, 6u> faces;
		SH9<Vec4f> skyRadiance {};
		nytl::Vec3f avgSunRadiance {};
		nytl::Vec3f sunIrradiance {};
	};

	static Baked bake(Vec3f sunDir, Vec3f groundAlbeo, float turbidity);

public:
	Sky() = default;

	// When no dslayout is given, will not create ds.
	Sky(const vpp::Device& dev, const vpp::TrDsLayout*,
		Vec3f sunDir, Vec3f groundAlbedo, float turbidity);

	const auto& cubemap() const { return cubemap_; }
	const auto& ds() const { return ds_; }
	const auto& skyRadiance() const { return skyRadiance_; }

	const auto& avgSunRadiance() const { return avgSunRadiance_; }
	const auto& sunIrradiance() const { return sunIrradiance_; }

private:
	Texture cubemap_;
	vpp::TrDs ds_;
	SH9<Vec4f> skyRadiance_ {};

	nytl::Vec3f avgSunRadiance_ {};
	nytl::Vec3f sunIrradiance_ {};
};

} // namespace tkn
