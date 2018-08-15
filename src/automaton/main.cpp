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
#include <ny/key.hpp>
#include <vui/dat.hpp>
#include <vui/gui.hpp>

#include "automaton.hpp"
#include <shaders/predprey.comp.h>
#include <shaders/ant.comp.h>

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

	void click(std::optional<nytl::Vec2ui> pos) override;
	void display(vui::dat::Folder&) override;
	std::pair<bool, vk::Semaphore> updateDevice() override;
	void update(double) override;

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

	// display
	std::optional<nytl::Vec2ui> selected_;
	vpp::SubBuffer read_;
	vui::dat::Folder* field_;
	vui::dat::Textfield* preyField_;
	vui::dat::Textfield* predField_;
	std::uint8_t oldPrey_ {};
	std::uint8_t oldPred_ {};
	bool uploadField_ {};
};

PredPrey::PredPrey(vpp::Device& dev, vk::RenderPass rp) {
	init(dev, rp);
}

void PredPrey::init(vpp::Device& dev, vk::RenderPass rp) {
	auto size = nytl::Vec2ui {256, 256};
	Automaton::init(dev, rp, predprey_comp_data, size,
		BufferMode::doubled, vk::Format::r8g8Unorm,
		{}, {}, {}, GridType::hex);
	reset();
}

void PredPrey::reset() {
	std::vector<std::byte> data(sizeof(nytl::Vec2u8) * size().x * size().y);

	std::mt19937 rgen;
	rgen.seed(std::time(nullptr));
	std::uniform_int_distribution<std::uint8_t> distr(0, 255);

	auto* ptr = reinterpret_cast<nytl::Vec2u8*>(data.data());
	for(auto i = 0u; i < size().x * size().y; ++i) {
		ptr[i] = {distr(rgen), distr(rgen)};
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

void PredPrey::display(vui::dat::Folder& folder) {
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

	field_ = &folder.panel().create<Folder>("field");
	preyField_ = &field_->create<Textfield>("prey");
	predField_ = &field_->create<Textfield>("predators");
	dlg_assert(field_->panel().disable(*field_));

	auto f = [this](auto&) { uploadField_ = true; };
	preyField_->textfield().onSubmit = f;
	predField_->textfield().onSubmit = f;
}

void PredPrey::initBuffers(unsigned add) {
	Automaton::initBuffers(add);
	auto memBits = device().hostMemoryTypes();
	ubo_ = {device().bufferAllocator(), sizeof(float) * 6,
		vk::BufferUsageBits::uniformBuffer, 16u, memBits};
}

std::pair<bool, vk::Semaphore> PredPrey::updateDevice() {
	// TODO: insert host memory barrier for ubo in compute
	if(paramsChanged_) {
		auto map = ubo_.memoryMap();
		auto span = map.span();
		doi::write(span, params_);
	}

	// must be here since otherwise we can't be sure that it's finished
	// TODO: move it to the next frames update (needs buffered/defered
	// read_ by one frame)
	if(read_.size() && selected_) {
		auto map = read_.memoryMap();
		auto span = map.span();
		auto x = doi::read<std::uint8_t>(span);
		auto y = doi::read<std::uint8_t>(span);
		if(x != oldPrey_ || y != oldPred_) {
			preyField_->textfield().utf8(std::to_string(x));
			predField_->textfield().utf8(std::to_string(y));
			oldPrey_ = x;
			oldPred_ = y;
		}
	}

	read_ = {};
	if(selected_) {
		get(*selected_, &read_);
	}

	return Automaton::updateDevice();
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

void PredPrey::click(std::optional<nytl::Vec2ui> pos) {
	if(!selected_ && pos) {
		field_->panel().enable(*field_);
	} else if(selected_ && !pos) {
		field_->panel().disable(*field_);
	}

	selected_ = pos;
}

void PredPrey::update(double delta) {
	Automaton::update(delta);

	if(uploadField_ && selected_) {
		uploadField_ = false;
		try {
			std::uint8_t x = std::stof(preyField_->textfield().utf8());
			std::uint8_t y = std::stof(predField_->textfield().utf8());
			set(*selected_, {std::byte{x}, std::byte{y}});
		} catch(const std::exception& err) {
			dlg_error("Invalid float. '{}'", err.what());
		}
	}
}


class Ant : public Automaton {
public:
	Ant() = default;
	Ant(vpp::Device& dev, vk::RenderPass);
	void init(vpp::Device& dev, vk::RenderPass);

	void display(vui::dat::Folder&) override;
	std::pair<bool, vk::Semaphore> updateDevice() override;

	void initBuffers(unsigned) override;
	void compDsUpdate(vpp::DescriptorSetUpdate&) override;
	void compDsLayout(std::vector<vk::DescriptorSetLayoutBinding>&) override;

protected:
	struct {
		unsigned count;
		std::vector<nytl::Vec4f> colors;
		std::vector<uint32_t> movement;
	} params_;
	vpp::SubBuffer ubo_;
	vpp::SubBuffer storage_; // contains current positions
};


Ant::Ant(vpp::Device& dev, vk::RenderPass rp) {
	init(dev, rp);
}

void Ant::init(vpp::Device& dev, vk::RenderPass rp) {
	auto size = nytl::Vec2ui {256, 256};
	Automaton::init(dev, rp, ant_comp_data, size, BufferMode::single,
		vk::Format::r8Uint, {}, {}, {1}, GridType::hex);
}

void Ant::display(vui::dat::Folder&) {
}

std::pair<bool, vk::Semaphore> Ant::updateDevice() {
	return Automaton::updateDevice();
}

void Ant::initBuffers(unsigned addSize) {
	Automaton::initBuffers(addSize);
}

void Ant::compDsUpdate(vpp::DescriptorSetUpdate& update) {
	Automaton::compDsUpdate(update);
}

void Ant::compDsLayout(std::vector<vk::DescriptorSetLayoutBinding>& bindings) {
	Automaton::compDsLayout(bindings);
}

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
		automaton_.display(folder);

		return true;
	}

	bool features(vk::PhysicalDeviceFeatures& enable,
			const vk::PhysicalDeviceFeatures& supported) override {
		if(!App::features(enable, supported)) {
			return false;
		}

		if(!supported.shaderStorageImageExtendedFormats) {
			dlg_fatal("shaderStorageImageExtendedFormats not supported");
			return false;
		}

		enable.shaderStorageImageExtendedFormats = true;
		return true;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		// TODO: possible to do this without rerecord every time?
		// maybe record compute into different command buffer and submit
		// that dynamically?
		if(!paused_ || oneStep_) {
			automaton_.compute(cb);
		}
	}

	void render(vk::CommandBuffer cb) override {
		automaton_.render(cb);
		gui().draw(cb);
	}

	void update(double delta) override {
		if(oneStep_) {
			oneStep_ = false;
			rerecord();
		}

		App::update(delta);
		automaton_.update(delta);
	}

	bool mouseWheel(const ny::MouseWheelEvent& ev) override {
		if(App::mouseWheel(ev)) {
			return true;
		}

		using namespace nytl::vec::cw::operators;

		auto s = std::pow(1.05f, ev.value.y);
		auto n = nytl::Vec2f(ev.position) / window().size();
		auto d = 2 * nytl::Vec2f{n.x - 0.5f, n.y - 0.5f};

		doi::translate(mat_, -d);
		doi::scale(mat_, nytl::Vec {s, s});
		doi::translate(mat_, d);

		automaton_.transform(mat_);
		return true;
	}

	void updateDevice() override {
		App::updateDevice();
		auto [rec, sem] = automaton_.updateDevice();
		if(rec) {
			rerecord();
		}

		if(sem) {
			addSemaphore(sem, vk::PipelineStageBits::topOfPipe);
		}
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button == ny::MouseButton::left) {
			mouseDown_ = ev.pressed;

			if(!ev.pressed && !dragAdded_) {
				// inverse transform matrix
				auto i = mat_;
				i[0][0] = 1.f / i[0][0];
				i[1][1] = 1.f / i[1][1];
				i[0][3] /= -mat_[0][0];
				i[1][3] /= -mat_[1][1];

				using namespace nytl::vec::cw::operators;
				auto p = nytl::Vec2f(ev.position) / window().size();
				p = 2 * p - nytl::Vec2f {1.f, 1.f};
				auto world = nytl::Vec2f(i * nytl::Vec4f{p.x, p.y, 0.f, 1.f});
				automaton_.worldClick(world);
			}

			dragged_ = {};
			dragAdded_ = false;
			return true;
		}

		return false;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(!mouseDown_) {
			return;
		}

		constexpr auto thresh = 16.f;
		auto delta = nytl::Vec2f(ev.delta);
		dragged_ += delta;
		if(!dragAdded_ && dot(dragged_, dragged_) > thresh) {
			dragAdded_ = true;
			delta += dragged_;
		}

		using namespace nytl::vec::cw::operators;
		auto normed = 2 * delta / window().size();

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

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(ev.pressed && ev.keycode == ny::Keycode::p) {
			paused_ ^= true;
			waitEvents_ ^= true;
			rerecord();
		} else if(paused_ && ev.pressed && ev.keycode == ny::Keycode::n) {
			oneStep_ = true;
			rerecord();
		} else if(ev.pressed && ev.keycode == ny::Keycode::l) {
			automaton_.hexLines(!automaton_.hexLines());
			rerecord();
		} else {
			return false;
		}

		return true;
	}

protected:
	float facOld_ {1.f};
	bool paused_ {};
	bool oneStep_ {};
	nytl::Mat4f mat_;
	PredPrey automaton_;

	bool mouseDown_ {};
	nytl::Vec2f dragged_ {};
	bool dragAdded_ {};
};

int main(int argc, const char** argv) {
	AutomatonApp app;
	if(!app.init({"automaton", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
