#include "light.hpp"
#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/bits.hpp>
#include <tkn/transform.hpp>
#include <vui/gui.hpp>
#include <vui/dat.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <ny/event.hpp>
#include <ny/key.hpp>
#include <ny/appContext.hpp>
#include <ny/keyboardContext.hpp>
#include <vpp/vk.hpp>
#include <vpp/handles.hpp>
#include <vpp/pipeline.hpp>
#include <dlg/dlg.hpp>

#include <optional>
#include <random>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/smooth_shadow.light_pp.frag.h>

class ShadowApp : public tkn::App {
public:
	bool init(const nytl::Span<const char*> args) override {
		if(!App::init(args)) {
			return false;
		}

		auto& device = vulkanDevice();
		ubo_ = vpp::SubBuffer(device.bufferAllocator(),
			sizeof(nytl::Mat4f), vk::BufferUsageBits::uniformBuffer,
			device.hostMemoryTypes(), 4u);

		auto viewLayoutBindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex)};
		viewDsLayout_ = {device, viewLayoutBindings};
		viewDs_ = {device.descriptorAllocator(), viewDsLayout_};

		vpp::DescriptorSetUpdate update(viewDs_);
		update.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		update.apply();

		lightSystem_.emplace(device, viewDsLayout_);
		lightSystem().addSegment({{{1.f, 1.f}, {3.f, 1.f}}, -1.f});
		lightSystem().addSegment({{{3.f, 1.f}, {3.f, 3.f}}, -1.f});
		lightSystem().addSegment({{{3.f, 3.f}, {1.f, 3.f}}, -1.f});
		lightSystem().addSegment({{{1.f, 3.f}, {1.f, 1.f}}, -1.f});
		lightSystem().addSegment({{{4.f, 1.f}, {6.f, 1.f}}, -1.f});

		auto& light = lightSystem().addLight();
		light.position = {4.f, 1.5f};
		light.color = static_cast<nytl::Vec4f>(blackbody(4100));
		light.color[3] = 1.f;
		light.radius(0.25);
		light.strength(1.f);
		currentLight_ = &light;

		std::mt19937 rgen;
		rgen.seed(std::time(nullptr));

		std::uniform_real_distribution<float> posDistr(0.f, 5.f);
		std::uniform_int_distribution<unsigned> colDistr(2000u, 10000u);
		std::uniform_real_distribution<float> radDistr(0.008f, 0.3f);
		std::uniform_real_distribution<float> strengthDistr(0.2f, 1.5f);

		for(auto i = 0u; i < 2u && false; ++i) {
			auto& light = lightSystem().addLight();
			light.position = {posDistr(rgen), posDistr(rgen)};
			light.color = static_cast<nytl::Vec4f>(blackbody(colDistr(rgen)));
			light.color[3] = 1.f;
			light.radius(radDistr(rgen));
			dlg_info(light.position);
			// light.strength(strengthDistr(rgen));
		}

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
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment)
		};

		pp_.dsLayout = vpp::TrDsLayout(device, ppBindings);
		auto pipeSets = {
			pp_.dsLayout.vkHandle(),
			lightSystem().lightDsLayout().vkHandle()
		};

		vk::PipelineLayoutCreateInfo plInfo;
		plInfo.setLayoutCount = pipeSets.size();
		plInfo.pSetLayouts = pipeSets.begin();
		pp_.pipeLayout = {device, plInfo};

		auto combineVertex = vpp::ShaderModule(device,
			tkn_fullscreen_vert_data);
		auto combineFragment = vpp::ShaderModule(device,
			smooth_shadow_light_pp_frag_data);

		vpp::GraphicsPipelineInfo combinePipeInfo(renderPass(),
			pp_.pipeLayout, vpp::ShaderProgram({{
				{combineVertex, vk::ShaderStageBits::vertex},
				{combineFragment, vk::ShaderStageBits::fragment}
		}}));

		combinePipeInfo.assembly.topology = vk::PrimitiveTopology::triangleFan;

		vk::Pipeline vkPipe;
		vk::createGraphicsPipelines(device, {}, 1, combinePipeInfo.info(),
			nullptr, vkPipe);
		pp_.pipe = {device, vkPipe};

		pp_.ds = vpp::TrDs(device.descriptorAllocator(), pp_.dsLayout);
		vpp::DescriptorSetUpdate ppDsUpdate(pp_.ds);

		auto uboSize = sizeof(float) * 19;
		pp_.ubo = {device.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, device.hostMemoryTypes(), 4u};

		auto imgview = lightSystem().renderTarget().vkImageView();
		ppDsUpdate.imageSampler({{{}, imgview,
			vk::ImageLayout::shaderReadOnlyOptimal}});
		ppDsUpdate.uniform({{pp_.ubo.buffer(), pp_.ubo.offset(),
			pp_.ubo.size()}});
		ppDsUpdate.apply();

		// gui
		using namespace vui::dat;
		panel_ = &gui().create<Panel>(nytl::Vec2f{50.f, 0.f}, 300.f, 150.f);

		auto createValueTextfield = [&](auto& at, auto name, float& value) {
			auto start = std::to_string(value);
			start.resize(4);
			auto& t = at.template create<Textfield>(name, start).textfield();
			t.onSubmit = [&, name](auto& tf) {
				try {
					value = std::stof(std::string(tf.utf8()));
				} catch(const std::exception& err) {
					dlg_error("Invalid float for {}: {}", name, tf.utf8());
					return;
				}
			};
		};

		createValueTextfield(*panel_, "gamma", pp_.gamma);
		createValueTextfield(*panel_, "exposure", pp_.exposure);
		createValueTextfield(*panel_, "viewFac", pp_.viewFac);

		return true;
	}

	void updateDevice() override {
		App::updateDevice();
		rerecord_ |= lightSystem().updateDevice();

		if(updateView_) {
			updateView_ = false;
			auto map = ubo_.memoryMap();
			std::memcpy(map.ptr(), &viewTransform_, sizeof(viewTransform_));
		}

		auto map = pp_.ubo.memoryMap();
		auto ptr = map.span();
		auto ws = App::window().size();
		tkn::write(ptr, tkn::windowToLevelMatrix(ws, levelView_));
		tkn::write(ptr, pp_.exposure);
		tkn::write(ptr, pp_.gamma);
		tkn::write(ptr, pp_.viewFac);
	}

	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			lightSystem().lightPipeLayout(), 0, {{viewDs_.vkHandle()}}, {});
		lightSystem().renderLights(cb);
	}

	void render(vk::CommandBuffer cb) override {
		App::render(cb);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pp_.pipeLayout, 0, {{pp_.ds.vkHandle()}}, {});

		auto lightds = currentLight_->lightDs();
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pp_.pipeLayout, 1, {{lightds}}, {});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
		vk::cmdDraw(cb, 4, 1, 0, 0);
		gui().draw(cb);
	}

	void update(double dt) override {
		App::update(dt);
		if(currentLight_) {
			auto fac = dt;
			auto kc = appContext().keyboardContext();
			if(kc->pressed(ny::Keycode::d)) {
				currentLight_->position += nytl::Vec {fac, 0.f};
				refreshMatrices();
			}
			if(kc->pressed(ny::Keycode::a)) {
				currentLight_->position += nytl::Vec {-fac, 0.f};
				refreshMatrices();
			}
			if(kc->pressed(ny::Keycode::w)) {
				currentLight_->position += nytl::Vec {0.f, fac};
				refreshMatrices();
			}
			if(kc->pressed(ny::Keycode::s)) {
				currentLight_->position += nytl::Vec {0.f, -fac};
				refreshMatrices();
			}
		}

		App::scheduleRedraw();
	}

	void refreshMatrices() {
		nytl::Vec2ui wsize = App::window().size();
		levelView_.size = tkn::levelViewSize(wsize.x / float(wsize.y), 10.f);
		if(currentLight_) {
			levelView_.center = currentLight_->position;
		}

		viewTransform_ = tkn::levelMatrix(levelView_);

		updateView_ = true;
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		refreshMatrices();
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		auto pos = tkn::windowToLevel(App::window().size(),
			levelView_, ev.position);
		for(auto& light : lightSystem().lights()) {
			if(contains(light, pos)) {
				currentLight_ = &light;
				App::scheduleRerecord();
				refreshMatrices();
				return true;
			}
		}

		return false;
	}

	bool contains(const Light& light, nytl::Vec2f pos) {
		tkn::Circle circle {light.position, light.radius()};
		return tkn::contains(circle, pos);
	}

	LightSystem& lightSystem() { return *lightSystem_; }
	const char* name() const override { return "2D Smooth Shadw"; }

protected:
	std::optional<LightSystem> lightSystem_;
	vpp::SubBuffer ubo_;
	bool updateView_ {};
	vpp::TrDsLayout viewDsLayout_;
	vpp::TrDs viewDs_;
	vpp::Sampler sampler_;

	tkn::LevelView levelView_ {};
	nytl::Mat4f viewTransform_;

	struct {
		vpp::SubBuffer ubo;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;

		float exposure {1.f};
		float gamma {1.f};
		float viewFac {1.f};
	} pp_; // post process; combine pass

	Light* currentLight_ {};

	vui::dat::Panel* panel_ {};
};

// main
int main(int argc, const char** argv) {
	ShadowApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
