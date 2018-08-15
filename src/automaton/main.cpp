#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/bits.hpp>
#include <stage/transform.hpp>

#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/formats.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/pipelineInfo.hpp>

#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/stringParam.hpp>

#include <ny/mouseButton.hpp>
#include <vui/dat.hpp>
#include <vui/gui.hpp>

#include "automaton.hpp"
#include <shaders/predprey.comp.h>

#include <random>
#include <optional>
#include <vector>
#include <cmath>
#include <fstream>


class PredPrey : public Automaton {
public:
	PredPrey() = default;
	PredPrey(vpp::Device& dev, vk::RenderPass);
	void init(vpp::Device& dev, vk::RenderPass);

	// void click(nytl::Vec2ui pos) override;
	void settings(vui::dat::Folder&) override;
	std::pair<bool, vk::Semaphore> updateDevice(double) override;

	void initBuffers(unsigned) override;
	void compDsUpdate(vpp::DescriptorSetUpdate&) override;
	void compDsLayout(std::vector<vk::DescriptorSetLayoutBinding>&) override;

protected:
	void reset();
	void readSettings();

protected:
	struct {
		float preyBirth = 0.05;
		float preyDeathPerPred = 0.2;
		float predBirthPerPrey = 0.3;
		float predDeath = 0.1;
		float predWander = 0.05; // 0.01
		float preyWander = 0.05; // 0.01
	} params_;
	std::vector<std::pair<vui::Textfield*, float*>> settings_;
	bool paramsChanged_ {true};
	vpp::SubBuffer ubo_;
};

PredPrey::PredPrey(vpp::Device& dev, vk::RenderPass rp) {
	init(dev, rp);
}

void PredPrey::init(vpp::Device& dev, vk::RenderPass rp) {
	auto size = nytl::Vec2ui {256, 256};
	Automaton::init(dev, rp, predprey_comp_data, size,
		BufferMode::doubled, vk::Format::r8g8b8a8Unorm,
		{}, {}, {}, GridType::hex);
	reset();
}

void PredPrey::reset() {
	std::vector<std::byte> data(sizeof(nytl::Vec4u8) * size().x * size().y);

	std::mt19937 rgen;
	rgen.seed(std::time(nullptr));
	std::uniform_int_distribution<std::uint8_t> distr(0, 255);

	auto* ptr = reinterpret_cast<nytl::Vec4u8*>(data.data());
	for(auto i = 0u; i < size().x * size().y; ++i) {
		ptr[i] = {distr(rgen), distr(rgen), 0u, 0u};
	}

	fill({{0, 0, 0}, {size().x, size().y, 1}, data});
}

void PredPrey::readSettings() {
	paramsChanged_ = true;

	for(auto& s : settings_) {
		try {
			*s.second = std::stof(s.first->utf8());
		} catch(const std::exception& err) {
			dlg_error("Invalid float: {}", s.first->utf8());
		}
	}
}

void PredPrey::settings(vui::dat::Folder& folder) {
	using namespace vui::dat;
	auto createParamField = [&](auto& at, auto name, float& value) {
		auto start = std::to_string(value);
		auto it = start.end();
		while(it != start.begin() && *(--it) == '0');
		if(it != start.end()) {
			start.erase(it + 1, start.end());
		}

		auto& t = at.template create<Textfield>(name, start).textfield();
		settings_.push_back({&t, &value});
		t.onSubmit = [&](auto&) { readSettings(); };
	};

	createParamField(folder, "preyBirth", params_.preyBirth);
	createParamField(folder, "preyDeathPerPred", params_.preyDeathPerPred);
	createParamField(folder, "predBirthPerPrey", params_.predBirthPerPrey);
	createParamField(folder, "predDeath", params_.predDeath);
	createParamField(folder, "predWander", params_.predWander);
	createParamField(folder, "preyWander", params_.preyWander);
	folder.create<Button>("reset").onClick = [&]{
		readSettings();
		reset();
	};

	folder.create<Textfield>("save").textfield().onSubmit = [&](auto& tf){
		auto name = tf.utf8();
		dlg_debug("Trying to save {}", name);
		try {
			auto out = std::ofstream("saved/predprey/" + name);
			for(auto& s : settings_) {
				out << *s.second << "\n";
			}

			dlg_debug("Saving successful");
		} catch(const std::exception& err) {
			dlg_error("Failed to write file: {}", err.what());
		}
	};

	folder.create<Textfield>("load").textfield().onSubmit = [&](auto& tf){
		auto name = tf.utf8();
		dlg_debug("Trying to load {}", name);
		try {
			auto in = std::ifstream("saved/predprey/" + name);
			for(auto& s : settings_) {
				in >> *s.second;
				in.ignore(1); // newline

				auto start = std::to_string(*s.second);
				auto it = start.end();
				while(it != start.begin() && *(--it) == '0');
				if(it != start.end()) {
					start.erase(it + 1, start.end());
				}

				s.first->utf8(start);
			}

			dlg_debug("Loading successful");
		} catch(const std::exception& err) {
			dlg_error("Invalid file '{}':", name, err.what());
			return;
		}

		paramsChanged_ = true;
	};
}

void PredPrey::initBuffers(unsigned add) {
	Automaton::initBuffers(add);
	auto memBits = device().hostMemoryTypes();
	ubo_ = {device().bufferAllocator(), sizeof(float) * 6,
		vk::BufferUsageBits::uniformBuffer, 16u, memBits};
}

std::pair<bool, vk::Semaphore> PredPrey::updateDevice(double delta) {
	// TODO: insert host memory barrier for ubo in compute
	if(paramsChanged_) {
		auto map = ubo_.memoryMap();
		auto span = map.span();
		doi::write(span, params_);
	}

	return Automaton::updateDevice(delta);
}

void PredPrey::compDsUpdate(vpp::DescriptorSetUpdate& update) {
	Automaton::compDsUpdate(update);
	update.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
}

void PredPrey::compDsLayout(std::vector<vk::DescriptorSetLayoutBinding>& b) {
	Automaton::compDsLayout(b);
	b.push_back(vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
		vk::ShaderStageBits::compute));
}

class Ant : public Automaton {
public:
	Ant(vpp::Device& dev);

protected:
	struct {
		unsigned count;
		std::vector<nytl::Vec4f> colors;
		std::vector<uint32_t> movement;
	} params_;
	vpp::SubBuffer ubo_;
};

class GameOfLife : public Automaton {
protected:
	struct {
		uint32_t neighbors; // 4, 6 (hex), 8, 12
		uint32_t max; // death threshold
		uint32_t min; // bearth threshold
	} params_;
};

// Like game of life but each pixel has a scalar value.
class ScalarGameOfLife : public Automaton {
protected:
	struct {
		float fac;
		float min;
		float max;
	} params_;
};

class AutomatonApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		automaton_.init(vulkanDevice(), renderer().renderPass());

		mat_ = nytl::identity<4, float>();
		automaton_.transform(mat_);

		auto& panel = gui().create<vui::dat::Panel>(
			nytl::Rect2f {50.f, 0.f, 250.f, vui::autoSize}, 27.f);
		auto& folder = panel.create<vui::dat::Folder>("Automaton");
		automaton_.settings(folder);

		return true;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		automaton_.compute(cb);
	}

	void render(vk::CommandBuffer cb) override {
		automaton_.render(cb);
		gui().draw(cb);
	}

	void update(double delta) override {
		App::update(delta);
		delta_ = delta;
	}

	void mouseWheel(const ny::MouseWheelEvent& ev) override {
		App::mouseWheel(ev);
		using namespace nytl::vec::cw::operators;

		auto s = std::pow(1.05f, ev.value.y);

		// NOTE: garbage. Need to practive linear transfomrations and spaces
		// auto i = mat_;
		// i[0][0] = 1.f / i[0][0];
		// i[1][1] = 1.f / i[1][1];
		// i[0][3] /= -mat_[0][0];
		// i[1][3] /= -mat_[1][1];

		// mat_ maps from world space into normalize [0, 1] window space
		// so now we map from window space to world space, i.e. we
		// get the world space position shown in the center of the screen.
		// auto center = i * nytl::Vec {0.5f, 0.5f};
		// auto normed = ev.position / window().size();
		// auto d = (normed - nytl::Vec {0.5f, 0.5f}) - center;

		// auto normed = nytl::Vec2f(ev.position) / window().size();
		// auto world4 = mat_ * nytl::Vec4f {normed.x, normed.y, 0.f, 1.f};
		// auto c4 = mat_ * nytl::Vec4f {0.5f, 0.5f, 0.f, 1.f};
		// auto d = nytl::Vec {world4[0] - c4[0], world4[1] - c4[1]};
		// /garabge

		auto n = nytl::Vec2f(ev.position) / window().size();
		auto d = 2 * nytl::Vec2f{n.x - 0.5f, n.y - 0.5f};

		doi::translate(mat_, -d);
		doi::scale(mat_, nytl::Vec {s, s});
		doi::translate(mat_, d);

		automaton_.transform(mat_);
	}

	void updateDevice() override {
		App::updateDevice();
		auto [rec, sem] = automaton_.updateDevice(delta_);
		if(rec) {
			rerecord();
		}

		if(sem) {
			addSemaphore(sem, vk::PipelineStageBits::topOfPipe);
		}
	}

	void mouseButton(const ny::MouseButtonEvent& ev) override {
		App::mouseButton(ev);
		if(ev.button == ny::MouseButton::left) {
			mouseDown_ = ev.pressed;;
		}
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(!mouseDown_) {
			return;
		}

		using namespace nytl::vec::cw::operators;
		auto normed = 2 * nytl::Vec2f(ev.delta) / window().size();
		doi::translate(mat_, normed);
		automaton_.transform(mat_);
	}

	void resize(const ny::SizeEvent& ev) override {
		auto fac = (float(ev.size.y) / ev.size.x);
		mat_[0][0] *= fac / facOld_;
		facOld_ = fac;
		automaton_.transform(mat_);

		App::resize(ev);
	}

protected:
	float facOld_ {1.f};
	double delta_ {};
	nytl::Mat4f mat_;
	PredPrey automaton_;
	bool mouseDown_ {};
};

int main(int argc, const char** argv) {
	AutomatonApp app;
	if(!app.init({"automaton", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
