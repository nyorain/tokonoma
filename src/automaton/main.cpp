#include <tkn/singlePassApp.hpp>
#include <tkn/bits.hpp>
#include <tkn/features.hpp>
#include <tkn/transform.hpp>

#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/formats.hpp>
#include <vpp/imageOps.hpp>

#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/stringParam.hpp>

#include <swa/swa.h>
#include <vui/dat.hpp>
#include <vui/gui.hpp>

#include "automaton.hpp"
#include <shaders/automaton.predprey.comp.h>
#include <shaders/automaton.ant.comp.h>

#include <random>
#include <optional>
#include <vector>
#include <cmath>
#include <fstream>

// TODO: this whole project is broken atm, fix it!
// TODO: use LevelView

// TODO: ensure correct memory barriers
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
	void reset(bool clear);
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
	float oldPrey_ {};
	float oldPred_ {};
	bool uploadField_ {};
};

PredPrey::PredPrey(vpp::Device& dev, vk::RenderPass rp) {
	init(dev, rp);
}

void PredPrey::init(vpp::Device& dev, vk::RenderPass rp) {
	auto size = nytl::Vec2ui {4 * 256, 4 * 256};
	Automaton::init(dev, rp, automaton_predprey_comp_data, size,
		BufferMode::doubled, vk::Format::r32g32Sfloat,
		{}, {}, {}, GridType::hex);
	reset(false);
}

void PredPrey::reset(bool clear) {
	// std::vector<std::byte> data(sizeof(nytl::Vec2u8) * size().x * size().y, {});
	std::vector<std::byte> data(sizeof(nytl::Vec2f) * size().x * size().y, {});

	if(!clear) {
		std::mt19937 rgen;
		rgen.seed(std::time(nullptr));
		// std::uniform_int_distribution<std::uint8_t> distr(0, 255);
		std::uniform_real_distribution<float> distr(0, 1.f);

		auto* ptr = reinterpret_cast<nytl::Vec2f*>(data.data());
		for(auto i = 0u; i < size().x * size().y; ++i) {
			ptr[i] = {distr(rgen), distr(rgen)};
		}
	}

	fill({{0, 0, 0}, {size().x, size().y, 1}, data});
}

void PredPrey::readSettings() {
	paramsChanged_ = true;

	for(auto& s : settings_) {
		try {
			*s.second = std::stof(std::string(s.first->utf8()));
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
		reset(false);
	};

	folder.create<Button>("clear").onClick = [&]{
		readSettings();
		reset(true);
	};

	folder.create<Textfield>("save").textfield().onSubmit = [&](auto& tf){
		auto name = tf.utf8();
		dlg_debug("Trying to save {}", name);
		try {
			auto out = std::ofstream("saved/predprey/" + std::string(name));
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
			auto in = std::ifstream("saved/predprey/" + std::string(name));
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

	field_ = &folder.create<Folder>("field");
	preyField_ = &field_->create<Textfield>("prey");
	predField_ = &field_->create<Textfield>("predators");
	// dlg_assert(field_->panel().disable(*field_));

	auto f = [this](auto&) { uploadField_ = true; };
	preyField_->textfield().onSubmit = f;
	predField_->textfield().onSubmit = f;
}

void PredPrey::initBuffers(unsigned add) {
	Automaton::initBuffers(add);
	auto memBits = device().hostMemoryTypes();
	ubo_ = {device().bufferAllocator(), sizeof(float) * 6,
		vk::BufferUsageBits::uniformBuffer, memBits, 16u};
}

std::pair<bool, vk::Semaphore> PredPrey::updateDevice() {
	// TODO: insert host memory barrier for ubo in compute
	if(paramsChanged_) {
		auto map = ubo_.memoryMap();
		auto span = map.span();
		tkn::write(span, params_);
	}

	// must be here since otherwise we can't be sure that it's finished
	// TODO: move it to the next frames update (needs buffered/defered
	// read_ by one frame)
	if(read_.size() && selected_) {
		auto map = read_.memoryMap();
		auto span = map.span();
		// auto x = tkn::read<std::uint8_t>(span);
		// auto y = tkn::read<std::uint8_t>(span);
		auto x = tkn::read<float>(span);
		auto y = tkn::read<float>(span);
		if(x != oldPrey_ || y != oldPred_) {
			auto& gui = preyField_->gui();

			// we don't update the content if they are focused to
			// allow writing. NOTE: this would be easier if the automaton
			// has knowledge about whether simulation is stopped or not.
			if(gui.focus() != &preyField_->textfield()) {
				preyField_->textfield().utf8(std::to_string(x));
			}
			if(gui.focus() != &predField_->textfield()) {
				predField_->textfield().utf8(std::to_string(y));
			}

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
		// field_->panel().enable(*field_);
	} else if(selected_ && !pos) {
		// field_->panel().disable(*field_);
	}

	selected_ = pos;
}

void PredPrey::update(double delta) {
	Automaton::update(delta);

	if(uploadField_ && selected_) {
		uploadField_ = false;
		try {
			float x = std::stof(std::string(preyField_->textfield().utf8()));
			float y = std::stof(std::string(predField_->textfield().utf8()));

			std::vector<std::byte> data(8);
			std::memcpy(data.data() + 0u, &x, sizeof(float));
			std::memcpy(data.data() + 4u, &y, sizeof(float));

			set(*selected_, std::move(data));
		} catch(const std::exception& err) {
			dlg_error("Invalid float. '{}'", err.what());
		}
	}
}

// TODO: not finished yet
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
	// vpp::SubBuffer storage_; // contains current positions
};


Ant::Ant(vpp::Device& dev, vk::RenderPass rp) {
	init(dev, rp);
}

void Ant::init(vpp::Device& dev, vk::RenderPass rp) {
	auto size = nytl::Vec2ui {256, 256};
	Automaton::init(dev, rp, automaton_ant_comp_data, size, BufferMode::single,
		vk::Format::r8Uint, {}, {}, {1}, GridType::hex);

	// TODO
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

// TODO
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

class AutomatonApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		rvgInit();
		automaton_.init(vkDevice(), renderPass());

		mat_ = nytl::identity<4, float>();
		automaton_.transform(mat_);

		auto& panel = gui().create<vui::dat::Panel>(
			nytl::Vec2f {50.f, 0.f}, 300.f, 150.f);
		auto& folder = panel.create<vui::dat::Folder>("Automaton");
		automaton_.display(folder);

		return true;
	}

	bool features(tkn::Features& enable, const tkn::Features& supported) override {
		if(!Base::features(enable, supported)) {
			return false;
		}

		// TODO: really not needed, just check manually for format support...
		if(!supported.base.features.shaderStorageImageExtendedFormats) {
			dlg_fatal("shaderStorageImageExtendedFormats not supported");
			return false;
		}

		enable.base.features.shaderStorageImageExtendedFormats = true;
		return true;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		// TODO: possible to do this without rerecord every time?
		// maybe record compute into different command buffer and submit
		// that dynamically?
		// nah, probably best to just use dynamic dispatch in automaon_
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
			Base::scheduleRerecord();
		}

		Base::update(delta);
		automaton_.update(delta);

		// TODO: we sometimes need this even when paused
		if(!paused_) {
			Base::scheduleRedraw();
		}
	}

	bool mouseWheel(float dx, float dy) override {
		if(Base::mouseWheel(dx, dy)) {
			return true;
		}

		using namespace nytl::vec::cw::operators;

		int mx, my;
		swa_display_mouse_position(swaDisplay(), &mx, &my);

		auto s = std::pow(1.05f, dy);
		auto n = nytl::Vec2f{float(mx), float(my)} / windowSize();
		auto d = 2 * nytl::Vec2f{n.x - 0.5f, n.y - 0.5f};

		tkn::translate(mat_, -d);
		tkn::scale(mat_, nytl::Vec {s, s});
		tkn::translate(mat_, d);

		automaton_.transform(mat_);
		Base::scheduleRedraw();
		return true;
	}

	void updateDevice() override {
		Base::updateDevice();
		auto [rec, sem] = automaton_.updateDevice();
		if(rec) {
			Base::scheduleRerecord();
		}

		if(sem) {
			addSemaphore(sem, vk::PipelineStageBits::topOfPipe);
		}
	}

	bool mouseButton(const swa_mouse_button_event& ev) override {
		if(Base::mouseButton(ev)) {
			return true;
		}

		if(ev.button == swa_mouse_button_left) {
			mouseDown_ = ev.pressed;

			if(!ev.pressed && !dragAdded_) {
				// inverse transform matrix
				auto i = mat_;
				i[0][0] = 1.f / i[0][0];
				i[1][1] = 1.f / i[1][1];
				i[0][3] /= -mat_[0][0];
				i[1][3] /= -mat_[1][1];

				using namespace nytl::vec::cw::operators;
				auto p = nytl::Vec2f{float(ev.x), float(ev.y)} / windowSize();
				p = 2 * p - nytl::Vec2f {1.f, 1.f};
				auto world = nytl::Vec2f(i * nytl::Vec4f{p.x, p.y, 0.f, 1.f});
				automaton_.worldClick(world);
				Base::scheduleRedraw();
			}

			dragged_ = {};
			dragAdded_ = false;
			return true;
		}

		return false;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		if(!mouseDown_) {
			return;
		}

		constexpr auto thresh = 16.f;
		auto delta = nytl::Vec2f{float(ev.dx), float(ev.dy)};
		dragged_ += delta;
		if(!dragAdded_ && dot(dragged_, dragged_) > thresh) {
			dragAdded_ = true;
			delta += dragged_;
		}

		using namespace nytl::vec::cw::operators;
		auto normed = 2 * delta / windowSize();

		tkn::translate(mat_, normed);
		automaton_.transform(mat_);
		Base::scheduleRedraw();
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		auto fac = (float(h) / w);
		mat_[0][0] *= fac / facOld_;
		facOld_ = fac;
		automaton_.transform(mat_);
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(ev.pressed && ev.keycode == swa_key_p) {
			paused_ ^= true;
			Base::scheduleRerecord();
		} else if(paused_ && ev.pressed && ev.keycode == swa_key_n) {
			oneStep_ = true;
			Base::scheduleRerecord();
		} else if(ev.pressed && ev.keycode == swa_key_l) {
			automaton_.hexLines(!automaton_.hexLines());
			Base::scheduleRerecord();
		} else {
			return false;
		}

		return true;
	}

	const char* name() const override { return "automaton"; }

protected:
	float facOld_ {1.f};
	bool paused_ {true};
	bool oneStep_ {};
	nytl::Mat4f mat_;
	PredPrey automaton_;
	// Ant automaton_;

	bool mouseDown_ {};
	nytl::Vec2f dragged_ {};
	bool dragAdded_ {};
};

int main(int argc, const char** argv) {
	return tkn::appMain<AutomatonApp>(argc, argv);
}
