#include <tkn/singlePassApp.hpp>
#include <tkn/fswatch.hpp>
#include <tkn/pipeline.hpp>
#include <tkn/render.hpp>
#include <tkn/levelView.hpp>
#include <tkn/texture.hpp>
#include <tkn/bits.hpp>
#include <tkn/defer.hpp>
#include <tkn/types.hpp>
#include <tkn/ccam.hpp>
#include <tkn/scene/environment.hpp>

#include <vpp/handles.hpp>
#include <vpp/submit.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/queue.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/shader.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/debug.hpp>
#include <vpp/vk.hpp>

#include <dlg/dlg.hpp>
#include <argagg.hpp>
#include <array>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/tkn.texture.frag.h>
#include <shaders/tkn.textureTonemap.frag.h>
#include <shaders/tkn.skybox.vert.h>
#include <shaders/tkn.skybox.frag.h>
#include <shaders/tkn.skybox.tonemap.frag.h>

// TODO: add checkerboard pattern background for visualizing alpha
// TODO: the whole descriptor set setup is somewhat messy
//   due to the two separate sets in skybox.frag.
//   This could probably be changed (simplified) by now.
//   We might also just use SkyboxRenderer here for skyboxes.

using namespace tkn::types;

class ImageViewer final : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	struct Args : public Base::Args {
		bool noCube;
		std::string file;
	};

	enum class ImageType {
		e2d,
		e3d,
		cubemap,
	};

public:
	bool init(nytl::Span<const char*> args) override {
		Args parsed;
		if(!Base::doInit(args, parsed)) {
			return false;
		}

		auto& dev = vkDevice();
		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		camera_.perspectiveFov(0.4 * nytl::constants::pi);

		// load image
		auto p = tkn::loadImage(parsed.file);
		if(!p) {
			dlg_fatal("Cannot read image '{}'", parsed.file);
			return false;
		}

		if(!parsed.noCube && p->cubemap()) {
			imageType_ = ImageType::cubemap;
		} else if(p->size().z > 1) {
			imageType_ = ImageType::e3d;
		} else {
			imageType_ = ImageType::e2d;
		}

		bool cubemap = (imageType_ == ImageType::cubemap);

		tkn::TextureCreateParams params;
		params.cubemap = cubemap;
		params.format = p->format();
		params.view.levelCount = 1u;
		params.view.layerCount = cubemap ? 6u : 1u;
		params.format = displayFormat(p->format());
		params.usage = vk::ImageUsageBits::sampled;

		// TODO: Only for vulkan 1.1
		// if(p->size().z > 1) {
		// 	params.imageCreateFlags |= vk::ImageCreateBits::e2dArrayCompatible;
		// }

		format_ = p->format();
		layerCount_ = p->layers() / (cubemap ? 6u : 1u);
		levelCount_ = p->mipLevels();

		dlg_info("Image has size {}", p->size());
		dlg_info("Image has format {}", (int) p->format());
		dlg_info("Image has {} levels", levelCount_);
		dlg_info("Image has {} layers", layerCount_);
		dlg_info("Image {} a cubemap", p->cubemap() ? "is" : "isn't");

		auto wb = tkn::WorkBatcher(dev);
		wb.cb = cb;

		auto initTex = createTexture(wb, std::move(p), params);
		auto vi = initTexture(initTex, wb);
		vpp::nameHandle(vi, "viewed image");
		std::tie(image_, view_) = vi.split();


		// sampler
		auto sci = tkn::linearSamplerInfo();
		sampler_ = {dev, sci};

		// pipe
		const char* vertShader;
		const char* fragShader;

		vpp::GraphicsPipelineInfo gpi;
		gpi.renderPass(renderPass());

		if(cubemap) {
			vertShader = "tkn/shaders/skybox.vert";
			fragShader = "tkn/shaders/skybox.frag";
			gpi.assembly.topology = vk::PrimitiveTopology::triangleStrip;
		} else {
			vertShader = "tkn/shaders/fullscreen.vert";
			fragShader = "iv/texture.frag";
			gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		}

		std::string fragPreamble;
		if(tonemap_) {
			fragPreamble += "#define TONEMAP\n";
		}
		if(imageType_ == ImageType::e3d) {
			fragPreamble += "#define TEX3D\n";
		}

		pipe_ = {dev, {vertShader}, {fragShader, fragPreamble},
			fileWatcher_, tkn::GraphicsPipeInfoProvider::create(gpi, sampler_)};

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		// camera
		auto uboSize = std::max<vk::DeviceSize>(4u,
			(cubemap ? sizeof(nytl::Mat4f) : 2 * sizeof(nytl::Vec2f)) +
			(tonemap_ ? sizeof(float) : 0)) +
			(imageType_ == ImageType::e3d ? sizeof(float) : 0);
		ubo_ = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		updateDs();

		return true;
	}

	void updateDs() {
		auto& dsu = pipe_.dsu();
		if(imageType_ == ImageType::cubemap) {
			dsu(ubo_);
			dsu(view_);
		} else {
			dsu(view_);
			dsu(ubo_);
		}
	}

	argagg::parser argParser() const override {
		auto parser = Base::argParser();
		parser.definitions.push_back({
			"nocube", {"--no-cube"},
			"Don't show a cubemap, interpret faces as layers", 0});
		parser.definitions.push_back({
			"tonemap", {"--tonemap"},
			"Tonemap the image (use pageDown/Up for exposure)", 1});
		return parser;
	}

	bool handleArgs(const argagg::parser_results& result,
			Base::Args& bout) override {
		if(!Base::handleArgs(result, bout)) {
			return false;
		}

		auto& out = static_cast<Args&>(bout);
		out.noCube = result.has_option("nocube");
		if(result.pos.empty()) {
			dlg_fatal("No image argument given");
			return false;
		}

		auto& tonemap = result["tonemap"];
		if(tonemap) {
			tonemap_ = true;
			auto exposure = tonemap.as<const char*>();
			dlg_assert(exposure);

			char* end;
			exposure_ = std::strtold(exposure, &end);
			dlg_assertm(end != exposure, "Couldn't parse given exposure: {}",
				exposure);
		}

		out.file = result.pos[0];
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		if(pipe_.pipe()) {
			tkn::cmdBind(cb, pipe_);
			vk::cmdDraw(cb, imageType_ == ImageType::cubemap ? 14 : 4, 1, 0, 0, 0);

			// make sure redraw is triggered
			camera_.needsUpdate = true;
		}
	}

	void update(double delta) override {
		Base::update(delta);
		camera_.update(swaDisplay(), delta);

		fileWatcher_.update();
		pipe_.update();

		if(camera_.needsUpdate || pipe_.reloadablePipe().updatePending()) {
			Base::scheduleRedraw();
		}
	}

	void updateDevice() override {
		// update scene ubo
		if(camera_.needsUpdate) {
			camera_.needsUpdate = false;

			auto map = ubo_.memoryMap();
			auto span = map.span();
			if(imageType_ == ImageType::cubemap) {
				tkn::write(span, camera_.fixedViewProjectionMatrix());
			} else {
				auto off = nytl::Vec2f(levelView_.center - 0.5f * levelView_.size);
				tkn::write(span, off);
				tkn::write(span, levelView_.size);
			}

			if(tonemap_) {
				tkn::write(span, exposure_);
			}

			if(imageType_ == ImageType::e3d) {
				tkn::write(span, depth_);
			}

			map.flush();
		}

		if(recreateView_) {
			recreateView_ = false;

			vk::ImageViewCreateInfo viewInfo;
			switch(imageType_) {
				case ImageType::e2d:
					viewInfo.viewType = vk::ImageViewType::e2d;
					break;
				case ImageType::e3d:
					viewInfo.viewType = vk::ImageViewType::e3d;
					break;
				case ImageType::cubemap:
					viewInfo.viewType = vk::ImageViewType::cube;
					break;
			}

			viewInfo.format = format_;
			viewInfo.image = image_;
			viewInfo.subresourceRange.aspectMask = vk::ImageAspectBits::color;
			viewInfo.subresourceRange.baseArrayLayer = isCubemap() ? 6u * layer_ : layer_;
			viewInfo.subresourceRange.baseMipLevel = level_;
			viewInfo.subresourceRange.layerCount = isCubemap() ? 6u : 1u;
			viewInfo.subresourceRange.levelCount = 1u;
			view_ = {vkDevice(), viewInfo};

			updateDs();
			Base::scheduleRerecord();
		}

		if(pipe_.updateDevice()) {
			Base::scheduleRerecord();
		}
	}

	bool isCubemap() const {
		return imageType_ == ImageType::cubemap;
	}

	bool is3D() const {
		return imageType_ == ImageType::e3d;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		if(isCubemap()) {
			camera_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
		} else if(swa_display_mouse_button_pressed(swaDisplay(), swa_mouse_button_left)) {
			float dx = levelView_.size.x * float(ev.dx) / windowSize().x;
			float dy = levelView_.size.y * float(ev.dy) / windowSize().y;
			levelView_.center -= nytl::Vec2f{dx, dy};
			camera_.needsUpdate = true;
		} else if(is3D() && swa_display_mouse_button_pressed(swaDisplay(), swa_mouse_button_right)) {
			depth_ += 0.005 * ev.dx;
			depth_ = std::clamp(depth_, 0.f, 1.f);
			camera_.needsUpdate = true;
		}
	}

	bool mouseWheel(float dx, float dy) override {
		if(Base::mouseWheel(dx, dy)) {
			return true;
		}

		if(isCubemap()) {
			camera_.mouseWheel(dy);
		} else {
			levelView_.size *= std::pow(1.025f, dy);
			camera_.needsUpdate = true;
		}

		return true;
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == swa_key_right) {
			layer_ = (layer_ + 1) % layerCount_;
			recreateView_ = true;
			dlg_info("Showing layer {}", layer_);
			Base::scheduleRedraw();
		} else if(ev.keycode == swa_key_left) {
			layer_ = (layer_ + layerCount_ - 1) % layerCount_;
			recreateView_ = true;
			dlg_info("Showing layer {}", layer_);
			Base::scheduleRedraw();
		} else if(ev.keycode == swa_key_up) {
			level_ = (level_ + 1) % levelCount_;
			recreateView_ = true;
			dlg_info("Showing level {}", level_);
			Base::scheduleRedraw();
		} else if(ev.keycode == swa_key_down) {
			level_ = (level_ + levelCount_ - 1) % levelCount_;
			recreateView_ = true;
			dlg_info("Showing level {}", level_);
			Base::scheduleRedraw();
		} else if(ev.keycode == swa_key_k) {
			using Ctrl = tkn::ControlledCamera::ControlType;
			auto ctrl = camera_.controlType();
			if(ctrl == Ctrl::arcball) {
				camera_.useSpaceshipControl();
			} else if(ctrl == Ctrl::spaceship) {
				camera_.useFirstPersonControl();
			} else if(ctrl == Ctrl::firstPerson) {
				camera_.useArcballControl();
			}
		} else if(ev.keycode == swa_key_f) {
			swa_window_set_state(swaWindow(), swa_window_state_fullscreen);
		} else if(tonemap_ && ev.keycode == swa_key_pageup) {
			exposure_ *= 1.05;
			dlg_info("exposure: {}", exposure_);
			camera_.needsUpdate = true;
			Base::scheduleRedraw();
		} else if(tonemap_ && ev.keycode == swa_key_pagedown) {
			exposure_ /= 1.05;
			dlg_info("exposure: {}", exposure_);
			camera_.needsUpdate = true;
			Base::scheduleRedraw();
		} else {
			return false;
		}

		return true;
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		camera_.aspect({w, h});
	}

	vk::Format displayFormat(vk::Format source) {
		// TODO: extend this list of format conversions to perform.
		// Should probably be based dynamically upon the capabilities
		// of the device.
		switch(source) {
			case vk::Format::r16g16b16Sfloat:
				return vk::Format::r16g16b16a16Sfloat;
			default:
				return source;
		}
	}

	// Even for 3D cubemap visualization, we don't need depth
	bool needsDepth() const override { return false; }
	const char* name() const override { return "iv"; }
	const char* usageParams() const override { return "file [options]"; }

protected:
	vpp::Image image_;
	vpp::ImageView view_;
	vpp::Sampler sampler_;

	tkn::ManagedGraphicsPipe pipe_;
	tkn::FileWatcher fileWatcher_;

	vk::Format format_;
	unsigned layerCount_;
	unsigned levelCount_;
	unsigned layer_ {0};
	unsigned level_ {0};
	bool recreateView_ {};

	bool tonemap_ {};
	float exposure_ {1.f};
	float depth_ {0.f};

	// cubemap
	ImageType imageType_;
	vpp::SubBuffer ubo_;
	tkn::ControlledCamera camera_;

	// non-cubemap, 2D
	tkn::LevelView levelView_ {{0.5f, 0.5f}, {1.f, 1.f}};
};

int main(int argc, const char** argv) {
	return tkn::appMain<ImageViewer>(argc, argv);
}

