#include <tkn/singlePassApp.hpp>
#include <tkn/bits.hpp>
#include <tkn/formats.hpp>
#include <tkn/render.hpp>
#include <tkn/ccam.hpp>
#include <tkn/types.hpp>
#include <tkn/shader.hpp>
#include <tkn/image.hpp>
#include <tkn/util.hpp>
#include <argagg.hpp>

#include <vpp/device.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>
#include <vpp/formats.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/submit.hpp>
#include <vpp/util/file.hpp>
#include <vpp/imageOps.hpp>

#include <nytl/stringParam.hpp>
#include <nytl/vecOps.hpp>

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>

#include <shaders/tkn.fullscreen.vert.h>

// #define STB_IMAGE_WRITE_IMPLEMENTATION
// #include <tkn/stb_image_write.h>

// Specification of the frag shader ubo in ./spec.glsl

using namespace tkn::types;

class ViewerApp : public tkn::SinglePassApp {
public:
	struct ShaderData {
		Vec2f mpos;
		float time;
		u32 effect;

		Vec3f camPos;
		float aspect;
		Vec3f camDir;
		float fov {0.5 * nytl::constants::pi};
	};

	struct ShaderPCR {
		u32 tonemap {1u};
	};

	static constexpr auto screenshotFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto screenshotSize = vk::Extent2D {2048, 2048};

public:
	using Base = tkn::SinglePassApp;
	static constexpr auto cacheFile = "sviewer.cache";

	~ViewerApp() {
		if(screenshot_.writer.joinable()) {
			screenshot_.writer.join();
		}
	}

	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		auto& dev = vkDevice();
		auto mem = dev.hostMemoryTypes();
		ubo_ = {dev.bufferAllocator(), sizeof(ShaderData),
			vk::BufferUsageBits::uniformBuffer, mem};

		auto renderBindings = std::array{
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
		};

		dsLayout_.init(dev, renderBindings);

		vk::PushConstantRange pcr;
		pcr.offset = 0u;
		pcr.size = sizeof(ShaderPCR);
		pcr.stageFlags = vk::ShaderStageBits::fragment;
		pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {{pcr}}};
		vertShader_ = {dev, tkn_fullscreen_vert_data};

		cache_ = {dev, cacheFile};
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		{
			vpp::DescriptorSetUpdate update(ds_);
			update.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		}

		if(!initPipe(shaderFile_, renderPass(), pipeline_)) {
			return false;
		}

		return true;
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if (ev.pressed) {
			if(ev.utf8 && !std::strcmp(ev.utf8, "+")) {
				++effect_;
				dlg_info("Effect: {}", effect_);
			} else if(effect_ && ev.utf8 && !std::strcmp(ev.utf8, "-")) {
				--effect_;
				dlg_info("Effect: {}", effect_);
			} else if(ev.keycode == swa_key_r) {
				reload_ = true;
				screenshot_.reloadPipe = true;
			} else if(ev.keycode == swa_key_p) {
				screenshot();
			} else if(ev.utf8) {
				// check if its a number
				unsigned num;
				if (tkn::stoi(ev.utf8, num)) {
					effect_ = num;
					dlg_info("Effect: {}", effect_);
				} else {
					return false;
				}
			}
		} else {
			return false;
		}

		return true;
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		cam_.aspect({w, h});
	}

	void update(double dt) override {
		time_ += dt;
		cam_.update(swaDisplay(), dt);
		Base::update(dt);
		Base::scheduleRedraw(); // we always redraw; shaders are assumed dynamic
	}

	void updateDevice() override {
		// static constexpr auto align = 0.f;
		auto map = ubo_.memoryMap();
		auto span = map.span();

		ShaderData data;
		data.time = time_;
		data.mpos = mpos_;
		data.effect = effect_;
		data.camPos = cam_.position();
		data.camDir = cam_.dir();
		data.aspect = cam_.aspect();

		tkn::write(span, data);

		if(reload_) {
			initPipe(shaderFile_, renderPass(), pipeline_);
			Base::scheduleRerecord();
			reload_ = false;
		}
	}

	void render(vk::CommandBuffer cb, vk::Pipeline pipe, const ShaderPCR& pcr) {
		vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::fragment,
			0, sizeof(pcr), &pcr);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {{ds_.vkHandle()}}, {});
		vk::cmdDraw(cb, 6, 1, 0, 0);
	}

	void render(vk::CommandBuffer cb) override {
		ShaderPCR pcr;
		pcr.tonemap = false; // TODO: support hdr swapchain formats and color spaces
		render(cb, pipeline_, pcr);
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		using namespace nytl::vec::cw::operators;
		mpos_ = nytl::Vec2f{float(ev.x), float(ev.y)} / windowSize();
		cam_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	bool initPipe(std::string_view shaderFile, vk::RenderPass rp,
			vpp::Pipeline& out) {
		auto spv = shaderFile.find(".spv");
		if(spv == shaderFile.length() - 4) {
			try {
				fragShader_ = vpp::ShaderModule(vkDevice(), shaderFile);
			} catch(const std::exception& error) {
				dlg_error("Failed loading SPIR-V shader: {}", error.what());
				return false;
			}
		} else {
			auto name = std::string(shaderFile);
			if(shaderFile[0] != '/' && shaderFile[0] != '.') {
				name = "sviewer/shaders/" + std::string(shaderFile);
				name += ".frag";
			}
			auto mod = tkn::loadShader(vkDevice(), name);
			if(!mod) {
				return false;
			}

			fragShader_ = std::move(*mod);
		}

		initPipe(fragShader_, rp, out);
		return true;
	}

	void initPipe(const vpp::ShaderModule& frag, vk::RenderPass rp,
			vpp::Pipeline& out) {
		auto& dev = vkDevice();
		vpp::GraphicsPipelineInfo pipeInfo(rp, pipeLayout_, {{{
			{vertShader_, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment}
		}}});

		out = {dev, pipeInfo.info(), cache_};
		vpp::save(dev, cache_, cacheFile);
	}

	argagg::parser argParser() const override {
		auto parser = Base::argParser();
		auto& defs = parser.definitions;
		auto it = std::find_if(defs.begin(), defs.end(),
			[](const argagg::definition& def){
				return def.name == "multisamples";
		});
		dlg_assert(it != defs.end());
		defs.erase(it);

		return parser;
	}

	bool handleArgs(const argagg::parser_results& parsed,
			Base::Args& bout) override {
		if(!Base::handleArgs(parsed, bout)) {
			return false;
		}

		// try to interpret positional argument as shader
		if(parsed.pos.size() > 1) {
			dlg_fatal("Invalid usage (requires one shader parameter)");
			return false;
		}

		if(parsed.pos.size() == 1) {
			shaderFile_ = parsed.pos[0];
		}

		return true;
	}

	// TODO: refactor this out to general screenshot functionality
	// (in app class?). allow to just use the swapchain image if it supports
	// the needed usages, the format is okay and size is ok/irrelevant.
	// If format of swapchain is weird, we could just blit it (if supported).
	void initScreenshot() {
		auto& dev = vkDevice();

		auto pass = {0u};
		auto rpi = tkn::renderPassInfo({{screenshotFormat}}, {{pass}});
		rpi.attachments[0].finalLayout = vk::ImageLayout::transferSrcOptimal;
		auto& dep = rpi.dependencies.emplace_back();
		dep.srcSubpass = 0;
		dep.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
		dep.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dep.dstSubpass = vk::subpassExternal;
		dep.dstAccessMask = vk::AccessBits::transferRead;
		dep.dstStageMask = vk::PipelineStageBits::transfer;
		screenshot_.rp = {dev, rpi.info()};

		auto format = screenshotFormat;
		auto info = vpp::ViewableImageCreateInfo(format,
			vk::ImageAspectBits::color, screenshotSize,
			vk::ImageUsageBits::colorAttachment | vk::ImageUsageBits::transferSrc);

		screenshot_.image = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};
		screenshot_.width = screenshotSize.width;
		screenshot_.height = screenshotSize.height;

		vk::FramebufferCreateInfo fbi;
		fbi.attachmentCount = 1;
		fbi.pAttachments = &screenshot_.image.vkImageView();
		fbi.renderPass = screenshot_.rp;
		fbi.width = screenshotSize.width;
		fbi.height = screenshotSize.height;
		fbi.layers = 1;

		screenshot_.fb = {dev, fbi};

		auto qf = dev.queueSubmitter().queue().family();
		screenshot_.cb = dev.commandAllocator().get(qf,
			vk::CommandPoolCreateBits::resetCommandBuffer);
		screenshot_.initialized = true;
	}

	void recordScreenshot() {
		auto& cb = screenshot_.cb;
		auto size = screenshotSize;
		vk::beginCommandBuffer(cb, {});

		auto cv = vk::ClearValue{{0.f, 0.f, 0.f, 0.f}};
		vk::cmdBeginRenderPass(cb, {
			screenshot_.rp,
			screenshot_.fb,
			{0u, 0u, size.width, size.height},
			u32(1), &cv,
		}, {});

		vk::Viewport vp {0.f, 0.f, (float) size.width, (float) size.height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, size.width, size.height});

		ShaderPCR pcr;
		pcr.tonemap = tkn::isHDR(screenshotFormat);
		render(cb, screenshot_.pipe, pcr);
		vk::cmdEndRenderPass(cb);

		// our render pass has a dependency making sure we can directly
		// copy the contents here
		screenshot_.retrieve = vpp::retrieveStaging(cb, screenshot_.image.image(),
			screenshotFormat,
			vk::ImageLayout::transferSrcOptimal,
			{size.width, size.height, 1},
			{vk::ImageAspectBits::color, 0, 0});

		vk::endCommandBuffer(cb);
	}

	void screenshot() {
		if(screenshot_.writing.load()) {
			dlg_warn("Still writing screenshot");
			return;
		}

		if(!screenshot_.initialized) {
			initScreenshot();
		}

		if(screenshot_.reloadPipe) {
			if(!initPipe(shaderFile_, screenshot_.rp, screenshot_.pipe)) {
				return;
			}

			recordScreenshot();
		}

		auto& dev = vkDevice();
		auto& qs = dev.queueSubmitter();
		qs.wait(qs.add(screenshot_.cb));

		// get data
		screenshot_.writing.store(true);
		if(screenshot_.writer.joinable()) {
			screenshot_.writer.join();
		}

		dlg_info("start writing screenshot...");
		screenshot_.writer = std::thread([this]{
			auto map = screenshot_.retrieve.memoryMap();

			auto size = nytl::Vec3ui{screenshot_.width, screenshot_.height, 1u};
			auto img = tkn::wrapImage(size, screenshotFormat, map.span());
			if(screenshotFormat == vk::Format::r8g8b8a8Unorm) {
				auto fname = "sviewer.png";
				// - using stbi -
				// stbi_write_png(fname, screenshot_.width, screenshot_.height, 4,
				// 	(char*) map.ptr(), screenshot_.width * 4);

				// - using writePng -
				// much faster than stbi (seems around 10x to me) but
				// obviously depends on libpng.
				writePng(fname, *img);
			} else if(screenshotFormat == vk::Format::r16g16b16a16Sfloat) {
				// - using writeEXR, only for hdr formats -
				auto fname = "sviewer.exr";
				writeExr(fname, *img);
			} else {
				// - ktx(1) can handle pretty much anything we can do -
				auto fname = "sviewer.ktx";
				writeKtx(fname, *img);
			}

			screenshot_.writing.store(false);
			dlg_info("done writing screenshot");
		});
	}

	const char* name() const override { return "Shader Viewer"; }
	bool needsDepth() const override { return false; }

protected:
	const char* shaderFile_ = "dummy";
	bool reload_ {};

	// glsl ubo vars
	nytl::Vec2f mpos_ {}; // normalized (0 to 1)
	float time_ {}; // in seconds
	std::uint32_t effect_ {};

	vpp::ShaderModule vertShader_;
	vpp::ShaderModule fragShader_;
	vpp::SubBuffer ubo_;
	vpp::Pipeline pipeline_;
	vpp::PipelineLayout pipeLayout_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineCache cache_;
	tkn::ControlledCamera cam_;

	struct {
		bool reloadPipe {true};
		vpp::RenderPass rp;
		vpp::Pipeline pipe;

		vpp::CommandBuffer cb;
		vpp::Framebuffer fb;
		vpp::ViewableImage image;
		vpp::SubBuffer retrieve;
		unsigned width, height;

		bool initialized {false};
		std::atomic<bool> writing {};
		std::thread writer;
	} screenshot_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<ViewerApp>(argc, argv);
}

