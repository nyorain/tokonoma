#include <tkn/scene/environment.hpp>
#include <tkn/texture.hpp>
#include <tkn/bits.hpp>
#include <tkn/image.hpp>
#include <tkn/f16.hpp>
#include <tkn/render.hpp>

#include <vpp/bufferOps.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/submit.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/debug.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/handles.hpp>
#include <vpp/formats.hpp>
#include <vpp/vk.hpp>

#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>

extern "C" {
#include <ArHosekSkyModel.h>
}

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/tkn.skybox.vert.h>
#include <shaders/tkn.skybox.frag.h>

namespace tkn {

// Environment
void Environment::create(InitData& data, const WorkBatcher& wb,
		nytl::StringParam envMapPath, nytl::StringParam irradiancePath,
		vk::Sampler linear) {
	auto envMap = tkn::read(envMapPath, true);
	auto irradiance = tkn::read(irradiancePath, true);
	create(data, wb, std::move(envMap), std::move(irradiance), linear);
}

void Environment::create(InitData& data, const WorkBatcher& wb,
		std::unique_ptr<ImageProvider> envMap,
		std::unique_ptr<ImageProvider> irradiance,
		vk::Sampler linear) {
	auto& dev = wb.dev;

	// textures
	tkn::TextureCreateParams params;
	params.cubemap = true;
	params.format = vk::Format::r16g16b16a16Sfloat;
	convolutionMipmaps_ = envMap->mipLevels();
	envMap_ = {data.initEnvMap, wb, std::move(envMap), params};
	irradiance_ = {data.initIrradiance, wb, std::move(irradiance), params};

	// pipe
	// ds layout
	auto bindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &linear),
	};

	dsLayout_ = {dev, bindings};

	// ubo
	ds_ = {data.initDs, dev.descriptorAllocator(), dsLayout_};
}

void Environment::init(InitData& data, const WorkBatcher& wb) {
	envMap_.init(data.initEnvMap, wb);
	irradiance_.init(data.initIrradiance, wb);
	ds_.init(data.initDs);

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.imageSampler({{{}, envMap_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
}

void Environment::createPipe(const vpp::Device& dev,
		vk::DescriptorSetLayout camDsLayout, vk::RenderPass rp,
		unsigned subpass, vk::SampleCountBits samples,
		nytl::Span<const vk::PipelineColorBlendAttachmentState> battachments) {
	pipeLayout_ = {dev, {{camDsLayout, dsLayout_.vkHandle()}}, {}};

	vpp::ShaderModule vertShader(dev, tkn_skybox_vert_data);
	vpp::ShaderModule fragShader(dev, tkn_skybox_frag_data);
	vpp::GraphicsPipelineInfo gpi {rp, pipeLayout_, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}, subpass, samples};

	// enable depth testing to only write where it's really needed
	// (where no geometry was rendered yet)
	gpi.depthStencil.depthTestEnable = true;
	gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
	// gpi.depthStencil.depthCompareOp = vk::CompareOp::greaterOrEqual;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleStrip;
	// culling not really needed here
	// gpi.rasterization.cullMode = vk::CullModeBits::back;
	gpi.rasterization.cullMode = vk::CullModeBits::none;
	// gpi.rasterization.frontFace = vk::FrontFace::clockwise;
	gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

	gpi.blend.attachmentCount = battachments.size();
	gpi.blend.pAttachments = battachments.begin();

	pipe_ = {dev, gpi.info()};
}

void Environment::render(vk::CommandBuffer cb) const {
	tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 1, {ds_});
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
	vk::cmdDraw(cb, 14, 1, 0, 0, 0);
}

// SkyboxRenderer
void SkyboxRenderer::create(const vpp::Device& dev, const PipeInfo& pi,
		nytl::Span<const vk::PipelineColorBlendAttachmentState> battachments) {
	auto bindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &pi.linear),
	};

	dsLayout_ = {dev, bindings};
	vpp::nameHandle(dsLayout_, "SkyboxRenderer:dsLayout");

	pipeLayout_ = {dev, {{pi.camDsLayout, dsLayout_.vkHandle()}}, {}};
	vpp::nameHandle(pipeLayout_, "SkyboxRenderer:pipeLayout");

	vk::SpecializationMapEntry specEntry;
	specEntry.constantID = 0u;
	specEntry.offset = 0u;
	specEntry.size = sizeof(u32);

	u32 specData = u32(pi.reverseDepth);

	vk::SpecializationInfo spec;
	spec.pData = &specData;
	spec.dataSize = sizeof(u32);
	spec.mapEntryCount = 1u;
	spec.pMapEntries = &specEntry;

	vpp::ShaderModule vertShader(dev, tkn_skybox_vert_data);
	vpp::ShaderModule fragShader(dev, tkn_skybox_frag_data);
	vpp::GraphicsPipelineInfo gpi {pi.renderPass, pipeLayout_, {{{
		{vertShader, vk::ShaderStageBits::vertex, &spec},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}, pi.subpass, pi.samples};

	// enable depth testing to only write where it's needed
	// (where no geometry was rendered yet)
	gpi.depthStencil.depthTestEnable = true;
	gpi.depthStencil.depthCompareOp = pi.reverseDepth ?
		vk::CompareOp::greaterOrEqual :
		vk::CompareOp::lessOrEqual;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleStrip;
	// culling not really needed here
	gpi.rasterization.cullMode = vk::CullModeBits::none;
	gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

	gpi.blend.attachmentCount = battachments.size();
	gpi.blend.pAttachments = battachments.begin();

	pipe_ = {dev, gpi.info()};
	vpp::nameHandle(pipe_, "SkyboxRenderer:pipe");
}

void SkyboxRenderer::render(vk::CommandBuffer cb, vk::DescriptorSet ds) {
	vpp::DebugLabel lbl(dsLayout_.device(), cb, "SkyboxRenderer");

	tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 1, {ds});
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
	vk::cmdDraw(cb, 14, 1, 0, 0, 0);
}

// Sky
// Returns unnormalized direction
Vec3f faceUVToDir(unsigned face, float u, float v) {
	u = 2 * u - 1;
	v = 2 * v - 1;

	constexpr auto x = Vec3f{1.f, 0.f, 0.f};
	constexpr auto y = Vec3f{0.f, 1.f, 0.f};
	constexpr auto z = Vec3f{0.f, 0.f, 1.f};
	constexpr struct {
		nytl::Vec3f dir;
		nytl::Vec3f s;
		nytl::Vec3f t;
	} faces[] = {
		{+x, -z, -y},
		{-x, +z, -y},
		{+y, +x, +z},
		{-y, +x, -z},
		{+z, +x, -y},
		{-z, -x, -y},
	};

	auto& data = faces[face];
	return data.dir + u * data.s + v * data.t;
}

// Computes the outer product of two vectors and returns a nested vector.
template<size_t D1, size_t D2, typename T1, typename T2>
auto outerProductVec(const nytl::Vec<D1, T1>& a, const nytl::Vec<D2, T2>& b) {
	nytl::Vec<D1, nytl::Vec<D2, decltype(a[0] * b[0])>> ret;
	for(auto i = 0u; i < D1; ++i) {
		for(auto j = 0u; j < D2; ++j) {
			ret[i][j] = a[i] * b[j];
		}
	}

	return ret;
}

// Computes the outer product of two vectors and returns a matrix.
template<size_t D1, size_t D2, typename T1, typename T2>
auto outerProductMat(const nytl::Vec<D1, T1>& a, const nytl::Vec<D2, T2>& b) {
	nytl::Mat<D1, D2, decltype(a[0] * b[0])> ret;
	for(auto i = 0u; i < D1; ++i) {
		for(auto j = 0u; j < D2; ++j) {
			ret[i][j] = a[i] * b[j];
		}
	}

	return ret;
}

auto mangle(Vec3f a, Vec3f b) {
	return std::acos(std::max(0.001f, dot(a, b)));
}

Sky::Sky(const vpp::Device& dev, const vpp::TrDsLayout& dsLayout,
		Vec3f sunDir, Vec3f albedo, float turbidity) {
	dlg_assert(sunDir.y >= 0.f);
	dlg_assert(turbidity >= 1.f && turbidity <= 10.f);
	normalize(sunDir);

	using nytl::constants::pi;

	constexpr auto up = Vec3f{0.f, 1.f, 0.f};
	constexpr auto fullLumEfficacy = 683.f;
	constexpr auto cubemapFormat = vk::Format::r16g16b16a16Sfloat;

	auto theta = mangle(sunDir, up);
	auto elevation = 0.5f * pi - theta;

	auto sR = arhosek_rgb_skymodelstate_alloc_init(turbidity, albedo.x, elevation);
	auto sG = arhosek_rgb_skymodelstate_alloc_init(turbidity, albedo.y, elevation);
	auto sB = arhosek_rgb_skymodelstate_alloc_init(turbidity, albedo.z, elevation);

	// store cubemap pixels and already project onto SH9
	constexpr auto width = 32u;
	constexpr auto height = 32u;

	using Pixel = nytl::Vec4<f16>;
	std::array<std::vector<Pixel>, 6> faces;
	std::array<const std::byte*, 6> ptrs;

	float wsum = 0.f;
	for(auto face = 0u; face < 6u; ++face) {
		faces[face].reserve(width * height);
		ptrs[face] = reinterpret_cast<std::byte*>(faces[face].data());

		for(auto y = 0u; y < height; ++y) {
			for(auto x = 0u; x < width; ++x) {
				float u = (x + 0.5f) / width;
				float v = (y + 0.5f) / height;

				auto dir = normalized(faceUVToDir(face, u, v));
				float theta = mangle(dir, up);
				float gamma = mangle(dir, sunDir);

				Vec4f rad;
				rad[0] = arhosek_tristim_skymodel_radiance(sR, theta, gamma, 0);
				rad[1] = arhosek_tristim_skymodel_radiance(sG, theta, gamma, 1);
				rad[2] = arhosek_tristim_skymodel_radiance(sB, theta, gamma, 2);
				rad[3] = 1.f;

				// convert radiance to luminance
				auto lum = fullLumEfficacy * rad;
				lum *= 0.0001; // TODO: use PBR units in shaders and pp to make this work
				faces[face].push_back(Pixel(lum));

				u = 2 * u - 1;
				v = 2 * v - 1;
				float t = 1.f + u * u + v * v;
				float w = 4.f / (std::sqrt(t) * t);

				wsum += w;
				lum *= w;
				this->luminance.coeffs += outerProductVec(projectSH9(dir).coeffs, lum);
			}
		}
	}

	auto normalization = 4 * pi / wsum;
	this->luminance.coeffs *= normalization;


	arhosekskymodelstate_free(sR);
	arhosekskymodelstate_free(sG);
	arhosekskymodelstate_free(sB);

	// create cubemap on device
	auto img = wrap({width, height}, vk::Format::r16g16b16a16Sfloat,
		1u, 1u, 6u, nytl::span(ptrs));
	TextureCreateParams params;
	params.format = cubemapFormat;
	params.cubemap = true;
	this->cubemap = {dev, std::move(img), params};
	vpp::nameHandle(this->cubemap.viewableImage(), "Sky:cubemap");

	// create ds
	this->ds = {dev.descriptorAllocator(), dsLayout};
	vpp::DescriptorSetUpdate dsu(this->ds);
	dsu.imageSampler({{{}, cubemap.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	vpp::nameHandle(this->ds, "Sky:ds");
}

} // namespace tkn
