#include "light.hpp"
#include <stage/app.hpp>
#include <stage/render.hpp>
#include <vui/gui.hpp>
#include <vui/dat.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <ny/event.hpp>
#include <ny/key.hpp>
#include <ny/appContext.hpp>
#include <ny/keyboardContext.hpp>
#include <vpp/vk.hpp>
#include <vpp/pipelineInfo.hpp>
#include <optional>

#include <shaders/fullscreen.vert.h>
#include <shaders/light_pp.frag.h>

// TODO: move somewhere else
template<typename T>
void scale(nytl::Mat4<T>& mat, nytl::Vec3<T> fac) {
	for(auto i = 0; i < 3; ++i) {
		mat[i][i] *= fac[i];
	}
}

template<typename T>
void translate(nytl::Mat4<T>& mat, nytl::Vec3<T> move) {
	for(auto i = 0; i < 3; ++i) {
		mat[i][3] += move[i];
	}
}


class ShadowApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!App::init(settings)) {
			return false;
		}

		auto& device = vulkanDevice();
		ubo_ = vpp::SubBuffer(device.bufferAllocator(),
			sizeof(nytl::Mat4f), vk::BufferUsageBits::uniformBuffer,
			4u, device.hostMemoryTypes());

		auto viewLayoutBindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex)};
		viewDsLayout_ = {device, viewLayoutBindings};
		viewDs_ = {device.descriptorAllocator(), viewDsLayout_};

		vpp::DescriptorSetUpdate update(viewDs_);
		update.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		update.apply();

		lightSystem_.emplace(device, viewDsLayout_);
		lightSystem().addSegment({{{1.f, 1.f}, {2.f, 1.f}}, -1.f});

		light1_ = &lightSystem().addLight();
		light1_->position = {0.5f, 0.5f};
		light1_->color = static_cast<nytl::Vec4f>(blackbody(8000u));
		light1_->color[3] = 1.f;

		light2_ = &lightSystem().addLight();
		light2_->color = static_cast<nytl::Vec4f>(blackbody(2000u));
		light2_->color[3] = 1.f;
		light2_->position = {2.0f, 2.0f};

		// post-process/combine
		auto info = vk::SamplerCreateInfo {};
		info.maxAnisotropy = 1.0;
		info.magFilter = vk::Filter::linear;
		info.minFilter = vk::Filter::linear;
		info.minLod = 0;
		info.maxLod = 0.25;
		info.mipmapMode = vk::SamplerMipmapMode::nearest;
		sampler_ = vpp::Sampler(device, info);
		auto ppBindings = {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		};

		pp_.dsLayout = vpp::TrDsLayout(device, ppBindings);
		auto pipeSets = {pp_.dsLayout.vkHandle()};

		vk::PipelineLayoutCreateInfo plInfo;
		plInfo.setLayoutCount = 1;
		plInfo.pSetLayouts = pipeSets.begin();
		pp_.pipeLayout = {device, plInfo};

		auto combineVertex = vpp::ShaderModule(device, fullscreen_vert_data);
		auto combineFragment = vpp::ShaderModule(device, light_pp_frag_data);

		vpp::GraphicsPipelineInfo combinePipeInfo(renderer().renderPass(),
			pp_.pipeLayout, vpp::ShaderProgram({
				{combineVertex, vk::ShaderStageBits::vertex},
				{combineFragment, vk::ShaderStageBits::fragment}
		}));

		combinePipeInfo.assembly.topology = vk::PrimitiveTopology::triangleFan;

		vk::Pipeline vkPipe;
		vk::createGraphicsPipelines(device, {}, 1, combinePipeInfo.info(),
			nullptr, vkPipe);
		pp_.pipe = {device, vkPipe};

		pp_.ds = vpp::TrDs(device.descriptorAllocator(), pp_.dsLayout);
		vpp::DescriptorSetUpdate ppDsUpdate(pp_.ds);

		auto imgview = lightSystem().renderTarget().vkImageView();
		ppDsUpdate.imageSampler({{{}, imgview,
			vk::ImageLayout::shaderReadOnlyOptimal}});
		ppDsUpdate.apply();

		return true;
	}

	bool updateDevice() override {
		auto ret = App::updateDevice();
		ret |= lightSystem().updateDevice();

		if(updateView_) {
			updateView_ = false;
			auto map = ubo_.memoryMap();
			std::memcpy(map.ptr(), &viewTransform_, sizeof(viewTransform_));
		}

		return ret;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			lightSystem().lightPipeLayout(), 0, {viewDs_}, {});
		lightSystem().renderLights(cb);
	}

	void render(vk::CommandBuffer cb) override {
		App::render(cb);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pp_.pipeLayout, 0, {pp_.ds}, {});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
		vk::cmdDraw(cb, 4, 1, 0, 0);
		gui().draw(cb);
	}

	void update(double dt) override {
		App::update(dt);
		if(currentLight_) {
			auto fac = 2 * dt;
			auto kc = appContext().keyboardContext();
			if(kc->pressed(ny::Keycode::d)) {
				currentLight_->position += nytl::Vec {fac, 0.f};
			}
			if(kc->pressed(ny::Keycode::a)) {
				currentLight_->position += nytl::Vec {-fac, 0.f};
			}
			if(kc->pressed(ny::Keycode::w)) {
				currentLight_->position += nytl::Vec {0.f, fac};
			}
			if(kc->pressed(ny::Keycode::s)) {
				currentLight_->position += nytl::Vec {0.f, -fac};
			}
		}
	}

	void key(const ny::KeyEvent& ev) override {
		App::key(ev);
		if(ev.pressed && ev.keycode == ny::Keycode::k1) {
			currentLight_ = light1_;
		} else if(ev.pressed && ev.keycode == ny::Keycode::k2) {
			currentLight_ = light2_;
		}
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);

		auto w = ev.size.x / float(ev.size.y);
		auto h = 1.f;
		auto fac = 10 / std::sqrt(w * w + h * h);

		auto s = nytl::Vec {
			(2.f / (fac * w)),
			(-2.f / (fac * h)), 1
			// 2.f / ev.size.x,
			// -2.f / ev.size.y, 1
		};

		viewTransform_ = nytl::identity<4, float>();
		scale(viewTransform_, s);
		translate(viewTransform_, {-1, 1, 0});
		updateView_ = true;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
	}

	void mouseButton(const ny::MouseButtonEvent& ev) override {
		App::mouseButton(ev);
	}

	LightSystem& lightSystem() { return *lightSystem_; }

protected:
	std::optional<LightSystem> lightSystem_;
	vpp::SubBuffer ubo_;
	bool updateView_ {};
	vpp::TrDsLayout viewDsLayout_;
	vpp::TrDs viewDs_;
	vpp::Sampler sampler_;

	nytl::Mat4f viewTransform_;
	// nytl::Mat4f windowToLevel_;

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} pp_; // post process; combine pass

	Light* light1_ {};
	Light* light2_ {};

	Light* currentLight_ {};
};

// main
int main(int argc, const char** argv) {
	ShadowApp app;
	if(!app.init({"smooth_shadow", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
