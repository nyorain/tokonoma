#include <tkn/scene/environment.hpp>
#include <tkn/scene/pbr.hpp>
#include <tkn/texture.hpp>
#include <tkn/bits.hpp>
#include <tkn/util.hpp>
#include <tkn/color.hpp>
#include <tkn/transform.hpp>
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
#include <shaders/tkn.skybox.layered.frag.h>
#include <shaders/tkn.skybox.tonemap.frag.h>
#include <shaders/tkn.skybox.layered.tonemap.frag.h>

using nytl::constants::pi;

namespace tkn {

// Environment
void Environment::create(InitData& data, WorkBatcher& wb,
		nytl::StringParam envMapPath, nytl::StringParam irradiancePath,
		vk::Sampler linear) {
	auto envMap = tkn::loadImage(envMapPath);
	auto irradiance = tkn::loadImage(irradiancePath);
	create(data, wb, std::move(envMap), std::move(irradiance), linear);
}

void Environment::create(InitData& data, WorkBatcher& wb,
		std::unique_ptr<ImageProvider> envMap,
		std::unique_ptr<ImageProvider> irradiance,
		vk::Sampler linear) {
	auto& dev = wb.dev;
	dlg_assert(envMap->cubemap());
	dlg_assert(irradiance->cubemap());

	// textures
	tkn::TextureCreateParams params;
	params.cubemap = true;
	params.format = vk::Format::r16g16b16a16Sfloat;
	convolutionMipmaps_ = envMap->mipLevels();
	data.initEnvMap = createTexture(wb, std::move(envMap), params);
	data.initIrradiance = createTexture(wb, std::move(irradiance), params);

	// pipe
	// ds layout
	auto bindings = std::array {
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, &linear),
	};

	dsLayout_.init(dev, bindings);

	// ubo
	ds_ = {data.initDs, dev.descriptorAllocator(), dsLayout_};
}

void Environment::init(InitData& data, WorkBatcher& wb) {
	envMap_ = initTexture(data.initEnvMap, wb);
	irradiance_ = initTexture(data.initIrradiance, wb);
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
	vk::cmdDraw(cb, 14, 1, 0, 0, 0); // skybox triangle strip
}

// SkyboxRenderer
void SkyboxRenderer::create(const vpp::Device& dev, const PipeInfo& pi,
		nytl::Span<const vk::PipelineColorBlendAttachmentState> battachments) {
	auto bindings = std::array {
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, &pi.sampler),
	};

	dsLayout_.init(dev, bindings);
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

	vpp::ShaderModule fragShader;
	if(!pi.tonemap && !pi.layered) {
		fragShader = {dev, tkn_skybox_frag_data};
	} else if(!pi.tonemap && pi.layered) {
		fragShader = {dev, tkn_skybox_layered_frag_data};
	} else if(pi.tonemap && !pi.layered) {
		fragShader = {dev, tkn_skybox_tonemap_frag_data};
	} else if(pi.tonemap && pi.layered) {
		fragShader = {dev, tkn_skybox_layered_tonemap_frag_data};
	}

	vpp::ShaderModule vertShader(dev, tkn_skybox_vert_data);
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

// Some code taken from MJP's (Matt Pettineo) sample framework, licensed
// under MIT as well. Copyright (c) 2016 MJP
// github.com/TheRealMJP/DeferredTexturing/tree/master/SampleFramework12/v1.01
Vec3f sampleDirectionCone(float u1, float u2, float cosThetaMax) {
    float cosTheta = (1.0f - u1) + u1 * cosThetaMax;
    float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
    float phi = u2 * 2.0f * pi;
    return Vec3f{std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
}

float sampleDirectionCone_PDF(float cosThetaMax) {
    return 1.0f / (2.0f * pi * (1.0f - cosThetaMax));
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

float irradianceIntegral(float theta) {
    float sinTheta = std::sin(theta);
    return pi * sinTheta * sinTheta;
}

Sky::Baked Sky::bake(Vec3f sunDir, Vec3f ground, float turbidity) {
	Baked out;

	dlg_assert(sunDir.y >= 0.f);
	dlg_assert(turbidity >= 1.f && turbidity <= 10.f);
	normalize(sunDir);

	auto theta = mangle(sunDir, up);
	auto elevation = 0.5f * pi - theta;

	auto sR = arhosek_rgb_skymodelstate_alloc_init(turbidity, ground.x, elevation);
	auto sG = arhosek_rgb_skymodelstate_alloc_init(turbidity, ground.y, elevation);
	auto sB = arhosek_rgb_skymodelstate_alloc_init(turbidity, ground.z, elevation);

	// store cubemap pixels and already project onto SH9
	float wsum = 0.f;
	for(auto face = 0u; face < 6u; ++face) {
		out.faces[face].reserve(faceWidth * faceHeight);

		for(auto y = 0u; y < faceHeight; ++y) {
			for(auto x = 0u; x < faceWidth; ++x) {
				float u = (x + 0.5f) / faceWidth;
				float v = (y + 0.5f) / faceHeight;

				auto dir = normalized(tkn::cubemap::faceUVToDir(face, u, v));
				float theta = mangle(dir, up);
				float gamma = mangle(dir, sunDir);

				Vec4f rad;
				rad[0] = arhosek_tristim_skymodel_radiance(sR, theta, gamma, 0);
				rad[1] = arhosek_tristim_skymodel_radiance(sG, theta, gamma, 1);
				rad[2] = arhosek_tristim_skymodel_radiance(sB, theta, gamma, 2);
				rad[3] = 1.f;

				auto lum = fullLumEfficacy * rad;
				lum *= f16Scale; // so we can safely convert to f16 below

				u = 2 * u - 1;
				v = 2 * v - 1;
				float t = 1.f + u * u + v * v;
				float w = 4.f / (std::sqrt(t) * t);

				// convert radiance to luminance
				wsum += w;
				auto wlum = w * lum;
				out.skyRadiance.coeffs +=
					outerProductVec(projectSH9(dir).coeffs, wlum);

				out.faces[face].push_back(Baked::Pixel(lum));
			}
		}
	}

	auto normalization = 4 * pi / wsum;
	out.skyRadiance.coeffs *= normalization;

	arhosekskymodelstate_free(sR);
	arhosekskymodelstate_free(sG);
	arhosekskymodelstate_free(sB);

	out.sunIrradiance = Sky::sunIrradiance(turbidity, ground, sunDir);
	float irrad = 1.f / irradianceIntegral(sunSize);
	out.avgSunRadiance = irrad * out.sunIrradiance;

	return out;
}

Vec3f Sky::sunIrradiance(float turbidity, Vec3f ground, Vec3f toSun) {
	auto theta = mangle(toSun, up);
	auto elevation = 0.5f * pi - theta;
	auto groundSpectral = SpectralColor::fromRGBRefl(ground);
	std::array<ArHosekSkyModelState*, nSpectralSamples> sSpectral;
	for(auto i = 0u; i < nSpectralSamples; ++i) {
		sSpectral[i] = arhosekskymodelstate_alloc_init(elevation, turbidity,
			groundSpectral.samples[i]);
	}

	const auto cosSunSize = std::cos(sunSize);

	auto nSunSamples = 8u;
	auto [sunBaseX, sunBaseY] = base(toSun);
	nytl::Vec3f sunIrradiance {};
	for(auto x = 0u; x < nSunSamples; ++x) {
		for(auto y = 0u; y < nSunSamples; ++y) {
			auto u = (x + 0.5f) / nSunSamples;
			auto v = (y + 0.5f) / nSunSamples;
			auto coneDir = sampleDirectionCone(u, v, cosSunSize);
			auto sampleDir = coneDir.z * toSun +
				coneDir.x * sunBaseX + coneDir.y * sunBaseY;

			auto theta = mangle(sampleDir, up);
			auto gamma = mangle(sampleDir, toSun);

			SpectralColor radiance;
			for(auto i = 0u; i < nSpectralSamples; ++i) {
				radiance.samples[i] = arhosekskymodel_solar_radiance(
					sSpectral[i], theta, gamma, radiance.wavelength(i));
			}

			auto f = std::clamp(dot(sampleDir, toSun), 0.f, 1.f);
			sunIrradiance += f * XYZtoRGB(toXYZ(radiance));
		}
	}

	for(auto& s : sSpectral) {
		arhosekskymodelstate_free(s);
	}

	auto pdf = sampleDirectionCone_PDF(cosSunSize);
	sunIrradiance *= 1.f / (nSunSamples * nSunSamples * pdf);

	// TODO: no idea where factor 100 comes from. We need it as well to get
	// realistic sun irradiance values here (around 100k at daytime).
	// MJP's comment: transform to our coordinate space.
	// But per docs, the hosek model uses the same units we use...
	return 100.f * fullLumEfficacy * sunIrradiance;
}

Sky::Sky(const vpp::Device& dev, const vpp::TrDsLayout* dsLayout,
		Vec3f sunDir, Vec3f ground, float turbidity) {
	auto data = bake(sunDir, ground, turbidity);
	avgSunRadiance_ = data.avgSunRadiance;
	sunIrradiance_ = data.sunIrradiance;
	skyRadiance_ = data.skyRadiance;

	std::array<const std::byte*, 6> ptrs;
	for(auto i = 0u; i < 6u; ++i) {
		ptrs[i] = reinterpret_cast<const std::byte*>(data.faces[i].data());
	}

	// create cubemap on device
	auto img = wrapImage({faceWidth, faceHeight, 1u}, vk::Format::r16g16b16a16Sfloat,
		1u, 6u, nytl::span(ptrs), true);
	TextureCreateParams params;
	params.format = cubemapFormat;
	params.cubemap = true;
	cubemap_ = buildTexture(dev, std::move(img), params);
	vpp::nameHandle(cubemap_, "Sky:cubemap");

	// create ds
	if(dsLayout) {
		ds_ = {dev.descriptorAllocator(), *dsLayout};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.imageSampler({{{}, cubemap_.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		vpp::nameHandle(ds_, "Sky:ds");
	}
}

} // namespace tkn
