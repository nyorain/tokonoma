#include <tkn/singlePassApp.hpp>
#include <tkn/window.hpp>
#include <tkn/render.hpp>
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
#include <vpp/vk.hpp>

#include <dlg/dlg.hpp>
#include <argagg.hpp>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/tkn.texture.frag.h>
#include <shaders/tkn.skybox.vert.h>
#include <shaders/tkn.skybox.frag.h>

// TODO: add checkerboard pattern for visualizing alpha

using namespace tkn::types;

/// ImageProvider wrapper that returns all faces as layers instead and
/// only the first original layer.
class FaceLayersProvider : public tkn::ImageProvider {
public:
	std::unique_ptr<tkn::ImageProvider> impl_;

public:
	unsigned mipLevels() const override { return impl_->mipLevels(); }
	unsigned layers() const override { return impl_->faces(); }
	unsigned faces() const override { return 1u; }
	nytl::Vec2ui size() const override { return impl_->size(); }
	vk::Format format() const override { return impl_->format(); }

	nytl::Span<const std::byte> read(unsigned mip = 0, unsigned layer = 0,
			unsigned face = 0) override {
		dlg_assert(face < faces() && layer < layers() && mip < mipLevels());
		// make sure two forward layer as face
		return impl_->read(mip, 0u, layer);
	}

	bool read(nytl::Span<std::byte> data, unsigned mip = 0, unsigned layer = 0,
			unsigned face = 0) override {
		dlg_assert(face < faces() && layer < layers() && mip < mipLevels());
		// make sure to forward layer as face
		return impl_->read(data, mip, 0u, layer);
	}
};

class ImageView final : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	struct Args : public Base::Args {
		bool noCube;
		std::string file;
	};

	static constexpr float near = 0.05f;
	static constexpr float far = 25.f;
	static constexpr float fov = 0.5 * nytl::constants::pi;

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

		// load image
		auto p = tkn::read(parsed.file);
		if(!p) {
			return false;
		}

		if(parsed.noCube) {
			auto wrapper = std::make_unique<FaceLayersProvider>();
			wrapper->impl_ = std::move(p);
			p = std::move(wrapper);
		}

		tkn::TextureCreateParams params;
		params.cubemap = cubemap_ = (p->faces() == 6);
		params.format = p->format();
		if(!params.cubemap) {
			params.view.baseArrayLayer = 0;
			params.view.layerCount = 1u;
			params.view.baseMipLevel = 0;
			params.view.levelCount = 1u;
		} else {
			params.view.layerCount = 6u;
		}

		format_ = p->format();
		layerCount_ = p->layers();
		levelCount_ = p->mipLevels();

		dlg_info("Image has size {}", p->size());
		dlg_info("Image has format {}", (int) p->format());
		dlg_info("Image has {} levels", levelCount_);
		dlg_info("Image has {} layers", layerCount_);
		dlg_info("Image has {} faces", p->faces());

		auto wb = tkn::WorkBatcher::createDefault(dev);
		wb.cb = cb;
		auto tex = tkn::Texture(wb, std::move(p), params);
		auto [img, view] = tex.viewableImage().split();
		image_ = std::move(img);
		view_ = std::move(view);

		// sampler
		vk::SamplerCreateInfo sci;
		sci.addressModeU = vk::SamplerAddressMode::clampToEdge;
		sci.addressModeV = vk::SamplerAddressMode::clampToEdge;
		sci.addressModeW = vk::SamplerAddressMode::clampToEdge;
		sci.magFilter = vk::Filter::linear;
		sci.minFilter = vk::Filter::linear;
		sci.mipmapMode = vk::SamplerMipmapMode::linear;
		sci.minLod = 0.0;
		sci.maxLod = 100.0;
		sci.anisotropyEnable = false;
		sampler_ = {dev, sci};

		// layouts
		auto tbindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		};

		texDsLayout_ = {dev, tbindings};

		if(cubemap_) {
			auto cbindings = {
				vpp::descriptorBinding(
					vk::DescriptorType::uniformBuffer,
					vk::ShaderStageBits::vertex),
			};

			camDsLayout_ = {dev, cbindings};
			pipeLayout_ = {dev, {{
				camDsLayout_.vkHandle(),
				texDsLayout_.vkHandle()}}, {}};

			// pipeline
			vpp::ShaderModule vertShader(dev, tkn_skybox_vert_data);
			vpp::ShaderModule fragShader(dev, tkn_skybox_frag_data);

			vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
				{vertShader, vk::ShaderStageBits::vertex},
				{fragShader, vk::ShaderStageBits::fragment},
			}}});

			gpi.assembly.topology = vk::PrimitiveTopology::triangleStrip;
			pipe_ = {dev, gpi.info()};
		} else {
			pipeLayout_ = {dev, {{texDsLayout_.vkHandle()}}, {}};

			// pipeline
			vpp::ShaderModule vertShader(dev, tkn_fullscreen_vert_data);
			vpp::ShaderModule fragShader(dev, tkn_texture_frag_data);

			vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
				{vertShader, vk::ShaderStageBits::vertex},
				{fragShader, vk::ShaderStageBits::fragment},
			}}});

			gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
			pipe_ = {dev, gpi.info()};
		}

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		// camera
		auto cameraUboSize = sizeof(nytl::Mat4f);
		cameraUbo_ = {dev.bufferAllocator(), cameraUboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		// ds
		texDs_ = {dev.descriptorAllocator(), texDsLayout_};
		vpp::DescriptorSetUpdate dsu(texDs_);
		dsu.imageSampler({{{{}, view_,
			vk::ImageLayout::shaderReadOnlyOptimal}}});

		if(cubemap_) {
			camDs_ = {dev.descriptorAllocator(), camDsLayout_};
			vpp::DescriptorSetUpdate cdsu(camDs_);
			cdsu.uniform({{{cameraUbo_}}});
		}

		return true;
	}

	argagg::parser argParser() const override {
		auto parser = Base::argParser();
		parser.definitions.push_back({
			"nocube", {"--no-cube"},
			"Don't show a cubemap, interpret faces as layers", 0});
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

		out.file = result.pos[0];
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		if(cubemap_) {
			tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {camDs_, texDs_});
			vk::cmdDraw(cb, 14, 1, 0, 0, 0);
		} else {
			tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {texDs_});
			vk::cmdDraw(cb, 4, 1, 0, 0);
		}
	}

	void update(double delta) override {
		Base::update(delta);
		camera_.update(swaDisplay(), delta);
		if(camera_.needsUpdate) {
			Base::scheduleRedraw();
		}
	}

	void updateDevice() override {
		// update scene ubo
		if(camera_.needsUpdate) {
			camera_.needsUpdate = false;
			auto map = cameraUbo_.memoryMap();
			auto span = map.span();
			tkn::write(span, camera_.fixedViewProjectionMatrix());
			map.flush();
		}

		if(recreateView_) {
			recreateView_ = false;
			vk::ImageViewCreateInfo viewInfo;
			viewInfo.viewType = cubemap_ ?
				vk::ImageViewType::cube :
				vk::ImageViewType::e2d;
			viewInfo.format = format_;
			viewInfo.image = image_;
			viewInfo.subresourceRange.aspectMask = vk::ImageAspectBits::color;
			viewInfo.subresourceRange.baseArrayLayer = cubemap_ ? 6u * layer_ : layer_;
			viewInfo.subresourceRange.baseMipLevel = level_;
			viewInfo.subresourceRange.layerCount = cubemap_ ? 6u : 1u;
			viewInfo.subresourceRange.levelCount = 1u;
			view_ = {vkDevice(), viewInfo};

			vpp::DescriptorSetUpdate dsu(texDs_);
			dsu.imageSampler({{{{}, view_,
				vk::ImageLayout::shaderReadOnlyOptimal}}});

			Base::scheduleRerecord();
		}
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		if(cubemap_) {
			camera_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
		}
	}

	bool mouseWheel(float dx, float dy) override {
		if(Base::mouseWheel(dx, dy)) {
			return true;
		}

		camera_.mouseWheel(dy);
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
		// TODO
		/*
		} else if(ev.keycode == swa_key_pageup) {
			maxLight *= 1.1;
			Base::scheduleRedraw();
		} else if(ev.keycode == swa_key_pagedown) {
			maxLight /= 1.1;
			Base::scheduleRedraw();
		} else if(ev.keycode == swa_key_k9) {
			maxAvgLight *= 1.1;
			Base::scheduleRedraw();
		} else if(ev.keycode == swa_key_k8) {
			maxAvgLight /= 1.1;
			Base::scheduleRedraw();
		*/
		} else {
			return false;
		}

		return true;
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		camera_.aspect({w, h});
	}

	bool needsDepth() const override { return false; }
	const char* name() const override { return "iv"; }
	const char* usageParams() const override { return "file [options]"; }

protected:
	vpp::Image image_;
	vpp::ImageView view_;

	vpp::Sampler sampler_;
	vpp::TrDsLayout texDsLayout_;
	vpp::TrDs texDs_;
	// cubemap only
	vpp::TrDsLayout camDsLayout_;
	vpp::TrDs camDs_;

	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::Pipeline boxPipe_;

	vk::Format format_;
	unsigned layerCount_;
	unsigned levelCount_;
	unsigned layer_ {0};
	unsigned level_ {0};
	bool recreateView_ {};

	// cubemap
	bool cubemap_ {};
	vpp::SubBuffer cameraUbo_;
	tkn::ControlledCamera camera_ {tkn::ControlledCamera::ControlType::firstPerson};
};

int main(int argc, const char** argv) {
	ImageView app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

