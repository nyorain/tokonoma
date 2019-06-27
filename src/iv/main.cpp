#include <stage/app.hpp>
#include <stage/window.hpp>
#include <stage/render.hpp>
#include <stage/texture.hpp>
#include <stage/bits.hpp>
#include <stage/defer.hpp>
#include <stage/types.hpp>
#include <stage/camera.hpp>
#include <stage/scene/environment.hpp>

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
#include <ny/key.hpp>
#include <ny/mouseButton.hpp>

#include <argagg.hpp>

#include <shaders/stage.fullscreen.vert.h>
#include <shaders/stage.texture.frag.h>
#include <shaders/stage.skybox.vert.h>
#include <shaders/stage.skybox.frag.h>

// TODO: add checkerboard pattern for visualizing alpha

using namespace doi::types;

/// ImageProvider wrapper that returns all faces as layers instead and
/// only the first original layer.
class FaceLayersProvider : public doi::ImageProvider {
public:
	std::unique_ptr<doi::ImageProvider> impl_;

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
		// make sure two forward layer as face
		return impl_->read(data, mip, 0u, layer);
	}
};

class ImageView : public doi::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!doi::App::init(args)) {
			return false;
		}

		auto& dev = device();
		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		// load image
		auto p = doi::read(file_);
		if(noCube_) {
			auto wrapper = std::make_unique<FaceLayersProvider>();
			wrapper->impl_ = std::move(p);
			p = std::move(wrapper);
		}

		doi::TextureCreateParams params;
		params.cubemap = cubemap_ = (p->faces() == 6 && p->layers() == 1);
		params.format = p->format();
		if(!params.cubemap) {
			params.view.baseArrayLayer = 0;
			params.view.layerCount = 1u;
			params.view.baseMipLevel = 0;
			params.view.levelCount = 1u;
		}

		format_ = p->format();
		layerCount_ = p->layers();
		levelCount_ = p->mipLevels();

		dlg_info("Image has size {}", p->size());
		dlg_info("Image has format {}", (int) p->format());
		dlg_info("Image has {} levels", levelCount_);
		dlg_info("Image has {} layers", layerCount_);
		dlg_info("Image has {} faces", p->faces());

		auto wb = doi::WorkBatcher::createDefault(dev);
		wb.cb = cb;
		auto tex = doi::Texture(wb, std::move(p), params);
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
		vpp::SubBuffer boxIndicesStage;
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

			// indices
			auto usage = vk::BufferUsageBits::indexBuffer |
				vk::BufferUsageBits::transferDst;
			auto inds = doi::boxInsideIndices;
			boxIndices_ = {dev.bufferAllocator(), sizeof(inds),
				usage, dev.deviceMemoryTypes(), 4u};
			boxIndicesStage = vpp::fillStaging(cb, boxIndices_, inds);

			// pipeline
			vpp::ShaderModule vertShader(dev, stage_skybox_vert_data);
			vpp::ShaderModule fragShader(dev, stage_skybox_frag_data);

			vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
				{vertShader, vk::ShaderStageBits::vertex},
				{fragShader, vk::ShaderStageBits::fragment},
			}}});

			gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
			pipe_ = {dev, gpi.info()};
		} else {
			pipeLayout_ = {dev, {{texDsLayout_.vkHandle()}}, {}};

			// pipeline
			vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
			vpp::ShaderModule fragShader(dev, stage_texture_frag_data);

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
		auto parser = App::argParser();
		parser.definitions.push_back({
			"nocube", {"--no-cube"},
			"Don't show a cubemap, interpret faces as layers", 0});
		return parser;
	}

	bool handleArgs(const argagg::parser_results& result) override {
		if(!App::handleArgs(result)) {
			return false;
		}

		noCube_ = result.has_option("nocube");
		if(result.pos.empty()) {
			dlg_fatal("No image argument given");
			return false;
		}

		file_ = result.pos[0];
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		if(cubemap_) {
			doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {camDs_, texDs_});
			vk::cmdBindIndexBuffer(cb, boxIndices_.buffer(),
				boxIndices_.offset(), vk::IndexType::uint16);
			vk::cmdDrawIndexed(cb, 36, 1, 0, 0, 0);
		} else {
			doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {texDs_});
			vk::cmdDraw(cb, 4, 1, 0, 0);
		}
	}

	void update(double delta) override {
		App::update(delta);
	}

	void updateDevice() override {
		// update scene ubo
		if(camera_.update) {
			camera_.update = false;
			auto map = cameraUbo_.memoryMap();
			auto span = map.span();
			doi::write(span, fixedMatrix(camera_));
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
			viewInfo.subresourceRange.baseArrayLayer = layer_;
			viewInfo.subresourceRange.baseMipLevel = level_;
			viewInfo.subresourceRange.layerCount = 1u;
			viewInfo.subresourceRange.levelCount = 1u;
			view_ = {device(), viewInfo};
			vpp::DescriptorSetUpdate dsu(texDs_);
			dsu.imageSampler({{{{}, view_,
				vk::ImageLayout::shaderReadOnlyOptimal}}});

			App::scheduleRerecord();
		}
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(cubemap_ && rotateView_) {
			doi::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
			App::scheduleRedraw();
		}
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(cubemap_ && ev.button == ny::MouseButton::left) {
			rotateView_ = ev.pressed;
			return true;
		}

		return false;
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(!cubemap_ && ev.keycode == ny::Keycode::right) {
			layer_ = (layer_ + 1) % layerCount_;
			recreateView_ = true;
			dlg_info("Showing layer {}", layer_);
			App::scheduleRedraw();
		} else if(!cubemap_ && ev.keycode == ny::Keycode::left) {
			layer_ = (layer_ + layerCount_ - 1) % layerCount_;
			recreateView_ = true;
			dlg_info("Showing layer {}", layer_);
			App::scheduleRedraw();
		} else if(ev.keycode == ny::Keycode::up) {
			level_ = (level_ + 1) % levelCount_;
			recreateView_ = true;
			dlg_info("Showing level {}", level_);
			App::scheduleRedraw();
		} else if(ev.keycode == ny::Keycode::down) {
			level_ = (level_ + levelCount_ - 1) % levelCount_;
			recreateView_ = true;
			dlg_info("Showing level {}", level_);
			App::scheduleRedraw();
		} else {
			return false;
		}

		return true;
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	const char* name() const override { return "iv"; }
	const char* usageParams() const override { return "file [options]"; }

protected:
	std::string file_;
	bool noCube_;

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
	vpp::SubBuffer boxIndices_;
	vpp::SubBuffer cameraUbo_;
	bool rotateView_ {};
	doi::Camera camera_;
};

int main(int argc, const char** argv) {
	ImageView app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

