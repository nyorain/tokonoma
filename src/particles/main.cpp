#include <tkn/config.hpp>
#include <tkn/singlePassApp.hpp>
#include <tkn/bits.hpp>
#include <tkn/types.hpp>
#include <argagg.hpp>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/shader.hpp>
#include <vpp/descriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/vk.hpp>

#include <nytl/vecOps.hpp>
#include <vui/gui.hpp>
#include <vui/dat.hpp>
#include <dlg/dlg.hpp>

#include <random>

#include <shaders/particles.particle.comp.h>
#include <shaders/particles.particle.frag.h>
#include <shaders/particles.particle.vert.h>
#include <shaders/particles.particle.audio.comp.h>

#if defined(TKN_WITH_AUDIO) || defined(TKN_WITH_PULSE_SIMPLE)
#include <tkn/audioFeedback.hpp>
#include <tkn/kiss_fft.h>
#endif

#ifdef TKN_WITH_AUDIO
#include <tkn/audio.hpp>
#include <tkn/sound.hpp>
#include <tkn/sampling.hpp>
using FeedbackMP3Audio = tkn::FeedbackAudioSource<tkn::StreamedMP3Audio>;
#endif // TKN_WITH_AUDIO

#ifdef TKN_WITH_PULSE_SIMPLE
  #include <tkn/pulseRecorder.hpp>
#endif // TKN_WITH_PULSE_SIMPLE

// velocity and position is always given in screen-space coords
// position in range [-1, 1] means its on screen

// TODO: alternatively, allow to visualize audio recorded via mic instead of
//   audio file or system audio

using namespace tkn::types;

class ParticleSystem {
public:
	struct CompUboData {
		// packed as vec4[4] on gpu
		nytl::Vec2f attractPoints[8];

		// packed as vec4 on gpu
		nytl::Vec3f attractFacs;
		float friction;

		float maxVel;
		float distOff;
		float dt;
		u32 count;
		float time;
		float ysize;
	};

	struct GfxUboData {
		float alpha;
		float pointSize;
		float ysize;
	};

	struct Particle {
		nytl::Vec2f pos;
		nytl::Vec2f vel;
	};

public:
	// params and stuff
	float alpha {0.1f};
	float pointSize {1.f};
	float ysize {1.f};

	// set to true if one of above (gfx params) was changed
	bool paramsChanged {true};


	// it looks nicer if we add normalized (first component) and inv-linear
	// (second component) in addition to the physically correct
	// inv-squared (third component) distance as well.
	nytl::Vec3f attractionFactors {0.05, 0.1, 0.4};
	float friction {0.4f}; // lower -> more friction; 1/s

	float maxVel {5.f}; // screen space per second
	float distOff {0.05f};

public:
	ParticleSystem() = default;
	void init(vpp::Device& dev, vk::RenderPass rp,
			vk::SampleCountBits samples, unsigned count,
			vpp::BufferSpan freqFactors = {}) {
		dev_ = &dev;

		// gfx stuff
		auto gfxBindings = std::array {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		gfxDsLayout_.init(dev, gfxBindings);
		gfxDs_ = {dev.descriptorAllocator(), gfxDsLayout_};
		gfxPipelineLayout_ = {dev, {{gfxDsLayout_.vkHandle()}}, {}};

		auto usage = nytl::Flags(vk::BufferUsageBits::uniformBuffer);
		auto mem = dev.memoryTypeBits(vk::MemoryPropertyBits::hostVisible);
		gfxUbo_ = {device().bufferAllocator(), sizeof(GfxUboData), usage, mem};

		vpp::ShaderModule vertShader(device(), particles_particle_vert_data);
		vpp::ShaderModule fragShader(device(), particles_particle_frag_data);
		vpp::GraphicsPipelineInfo gpi {rp, gfxPipelineLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0, samples};

		constexpr auto stride = sizeof(float) * 4; // vec2 pos, velocity
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::vertex};

		// vertex position attribute
		vk::VertexInputAttributeDescription attributes[2];
		attributes[0].format = vk::Format::r32g32Sfloat;

		attributes[1].format = vk::Format::r32g32Sfloat;
		attributes[1].location = 1;
		attributes[1].offset = sizeof(float) * 2;

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 2u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;
		gpi.assembly.topology = vk::PrimitiveTopology::pointList;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(device(), {},
			1, gpi.info(), NULL, vkpipe);

		gfxPipeline_ = {device(), vkpipe};

		// compute stuff
		std::vector compBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute)
		};

		bool withFactors = freqFactors.size() != 0;
		if(withFactors) {
			compBindings.push_back(vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute));
		}

		compDsLayout_.init(dev, compBindings);
		compDs_ = {dev.descriptorAllocator(), compDsLayout_};
		compPipelineLayout_ = {dev, {{compDsLayout_.vkHandle()}}, {}};

		vk::ComputePipelineCreateInfo cpi;
		vpp::ShaderModule compShader = withFactors ?
			vpp::ShaderModule(device(), particles_particle_audio_comp_data) :
			vpp::ShaderModule(device(), particles_particle_comp_data);

		vk::ComputePipelineCreateInfo info;
		info.layout = compPipelineLayout_;
		info.stage.module = compShader;
		info.stage.pName = "main";
		info.stage.stage = vk::ShaderStageBits::compute;

		vk::createComputePipelines(device(), {}, 1, info, nullptr, vkpipe);
		compPipeline_ = {dev, vkpipe};

		// buffer
		mem = 0xFFFFFFFF; // choose memory type here
		auto bits = dev.memoryTypeBits(vk::MemoryPropertyBits::deviceLocal);
		if(mem == 0xFFFFFFFF || !(bits & (1 << mem))) {
			mem = bits;
		}

		auto bufSize = sizeof(Particle) * count;
		usage = vk::BufferUsageBits::vertexBuffer
			| vk::BufferUsageBits::storageBuffer
			| vk::BufferUsageBits::transferDst;
		particleBuffer_ = {device().bufferAllocator(), bufSize, usage, mem};

		usage = vk::BufferUsageBits::uniformBuffer;
		mem = dev.memoryTypeBits(vk::MemoryPropertyBits::hostVisible);
		compUbo_ = {device().bufferAllocator(), sizeof(CompUboData), usage, mem};

		// create & upload particles
		count_ = count;
		reset();

		// write descriptor
		{
			vpp::DescriptorSetUpdate update1(compDs_);
			update1.storage({{particleBuffer_.buffer(),
				particleBuffer_.offset(), particleBuffer_.size()}});
			update1.uniform({{compUbo_.buffer(), compUbo_.offset(),
				compUbo_.size()}});

			if(withFactors) {
				update1.storage({{freqFactors.buffer(),
					freqFactors.offset(), freqFactors.size()}});
			}

			vpp::DescriptorSetUpdate update2(gfxDs_);
			update2.uniform({{gfxUbo_.buffer(), gfxUbo_.offset(),
				gfxUbo_.size()}});

			vpp::apply({{{update1}, {update2}}});
		}
	}

	void reset() {
		constexpr auto distrFrom = -0.85f;
		constexpr auto distrTo = 0.85f;
		auto& dev = *dev_;

		std::mt19937 rgen;
		rgen.seed(std::time(nullptr));
		std::uniform_real_distribution<float> distr(distrFrom, distrTo);

		std::vector<Particle> particles(count_);
		for(auto& part : particles) {
			part.pos[0] = distr(rgen);
			part.pos[1] = distr(rgen);
			part.vel = {0.f, 0.f};
		}

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		auto pspan = nytl::span(particles);
		auto stage = vpp::fillStaging(cb, particleBuffer_,
			nytl::as_bytes(pspan));

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));
	}

	void compute(vk::CommandBuffer cb) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, compPipeline_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			compPipelineLayout_, 0, {{compDs_.vkHandle()}}, {});

		auto size = std::ceil(count_ / 64.f);
		vk::cmdDispatch(cb, size, 1, 1);
	}

	void render(vk::CommandBuffer cb) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfxPipeline_);
		vk::cmdBindVertexBuffers(cb, 0, {{particleBuffer_.buffer().vkHandle()}},
			{{particleBuffer_.offset()}});
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gfxPipelineLayout_, 0, {{gfxDs_.vkHandle()}}, {});
		vk::cmdDraw(cb, count_, 1, 0, 0);
	}

	void updateDevice(double delta, nytl::Span<nytl::Vec2f> attractors) {
		time_ += delta;
		if(attractors.size() > 8) {
			 attractors = attractors.first(8);
		}

		auto view = compUbo_.memoryMap();
		auto span = view.span();

		CompUboData data;
		std::copy(attractors.begin(), attractors.end(), data.attractPoints);
		data.attractFacs = attractionFactors;
		data.friction = friction;
		data.maxVel = maxVel;
		data.distOff = distOff;
		data.dt = delta;
		data.count = attractors.size();
		data.time = time_;
		data.ysize = ysize;
		tkn::write(span, data);
		view.flush();

		if(paramsChanged) {
			paramsChanged = false;
			auto view = gfxUbo_.memoryMap();
			auto span = view.span();

			GfxUboData data;
			data.alpha = alpha;
			data.pointSize = pointSize;
			data.ysize = ysize;

			tkn::write(span, data);
			view.flush();
		}
	}

	const vpp::Device& device() const { return *dev_; }

protected:
	vpp::Device* dev_ {};
	vpp::Pipeline gfxPipeline_;
	vpp::PipelineLayout gfxPipelineLayout_;

	vpp::Pipeline compPipeline_;
	vpp::PipelineLayout compPipelineLayout_;

	vpp::SubBuffer particleBuffer_;
	vpp::SubBuffer compUbo_;
	vpp::TrDsLayout compDsLayout_;
	vpp::TrDs compDs_;

	vpp::TrDsLayout gfxDsLayout_;
	vpp::TrDs gfxDs_;
	vpp::SubBuffer gfxUbo_;

	float time_ {};
	unsigned count_ {};
};

class ParticlesApp : public tkn::SinglePassApp {
public:
	// On how many frames to perform the DFT.
	static constexpr auto frameCount = 1024;
	static constexpr auto logFac = 1.1;

	using Base = tkn::SinglePassApp;
	struct Args : public Base::Args {
		std::string audioFile {};
		bool systemAudio {};
		unsigned particleCount {500'000};
	};

public:
	using Base::init;
	bool init(nytl::Span<const char*> cargs) override {
		Args args;
		if(!Base::doInit(cargs, args)) {
			return false;
		}

		rvgInit();
		auto& dev = vkDevice();
		vpp::BufferSpan freqFactors;

		bool initFFT = false;
#ifdef TKN_WITH_AUDIO
		if(!args.audioFile.empty()) {
			auto asset = std::fopen(args.audioFile.c_str(), "rb");
			// TODO: android
			// auto asset = openAsset(audioFile_);
			if(!asset) {
				dlg_error("Couldn't open {}", args.audioFile);
				return false;
			}

			// audioPlayer_.emplace("particles", 0, 2, 2);
			audioPlayer_.emplace();
			auto& ap = *audioPlayer_;

			audio_ = &ap.create<FeedbackMP3Audio>(ap, tkn::File{asset});
			ap.start();
			initFFT = true;
		}
#endif // TKN_WITH_AUDIO

#ifdef TKN_WITH_PULSE_SIMPLE
		if(args.systemAudio) {
			if(!args.audioFile.empty()) {
				dlg_error("Can't use audio file and system audio");
				return false;
			}

			pulseRecorder_.emplace();
			pulseRecorder_->init();
			initFFT = true;
		}
#endif

#if defined(TKN_WITH_AUDIO) || defined(TKN_WITH_PULSE_SIMPLE)
		if(initFFT) {
			dft_.kiss = kiss_fft_alloc(frameCount, 0, nullptr, nullptr);
			dft_.freq.resize(frameCount);
			dft_.time.resize(frameCount);

			// TODO: there is probably a better way to estimate this
			auto i = 0u;
			auto size = 1.f;
			auto count = 0u;
			while(i < dft_.freq.size() / 4) {
				i += size;
				size *= logFac;
				++count;
			}

			unsigned fbs = sizeof(float) * count;
			factorBuf_ = {dev.bufferAllocator(), fbs,
				vk::BufferUsageBits::storageBuffer, dev.hostMemoryTypes()};
			freqFactors = factorBuf_;
		}
#else
		dlg_assert(!initFFT);
#endif

		// system
		system_.init(vkDevice(), renderPass(), samples(), args.particleCount,
			freqFactors);

		// gui
		auto& panel = gui().create<vui::dat::Panel>(nytl::Vec2f {100.f, 0.f});
		auto tfChangeProp = [&](auto& prop) {
			return [&](auto& tf) {
				try {
					prop = std::stof(std::string(tf.utf8()));
					system_.paramsChanged = true;
				} catch(const std::exception& err) {
					dlg_error("Invalid float: {}", tf.utf8());
				}
			};
		};

		auto createProp = [&](auto name, auto& prop) {
			auto& tf = panel.create<vui::dat::Textfield>(name).textfield();
			tf.onSubmit = tfChangeProp(prop);
			auto str = std::to_string(prop);
			str.resize(4);
			tf.utf8(str);
		};

		createProp("Alpha", system_.alpha);
		createProp("Point Size", system_.pointSize);
		createProp("Friction", system_.friction);
		createProp("Max. Velocity", system_.maxVel);
		createProp("Distance Offset", system_.distOff);
		createProp("Attraction 0", system_.attractionFactors.x);
		createProp("Attraction 1", system_.attractionFactors.y);
		createProp("Attraction 2", system_.attractionFactors.z);

		panel.create<vui::dat::Button>("Reset").onClick = [&]{ reset_ = true; };

		return true;
	}

	nytl::Vec2f attractorPos(const nytl::Vec2i pos) {
		using namespace nytl::vec::cw::operators;
		auto ws = windowSize();
		auto normed = (1.f / ws.x) * pos;
		return 2 * normed - nytl::Vec{1.f, system_.ysize};
	}

	// TODO: mouse and touch currently not usable at the same time
	// might crash application i guess
	bool mouseButton(const swa_mouse_button_event& ev) override {
		if(Base::mouseButton(ev)) {
			return true;
		}

		if(ev.button == swa_mouse_button_left) {
			attractors_.resize(ev.pressed);
			if(ev.pressed) {
				attractors_[0] = attractorPos({ev.x, ev.y});
			}

			return true;
		}

		return false;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		if(!attractors_.empty()) {
			attractors_[0] = attractorPos({ev.x, ev.y});
		}
	}

	bool touchBegin(const swa_touch_event& ev) override {
		if(Base::touchBegin(ev)) {
			return true;
		}

		auto pos = attractorPos(nytl::Vec2i{ev.x, ev.y});
		attractors_.push_back(pos);
		touchIDs_.push_back(ev.id);
		// dlg_trace("begin: {}", ev.id);
		return true;
	}

	bool touchEnd(unsigned id) override {
		if(Base::touchEnd(id)) {
			return true;
		}

		auto it = std::find(touchIDs_.begin(), touchIDs_.end(), id);
		if(it != touchIDs_.end()) {
			// dlg_trace("erase: {}", ev.id);
			attractors_.erase(attractors_.begin() + (it - touchIDs_.begin()));
			touchIDs_.erase(it);
		} else {
			dlg_error("Invalid touch id");
		}

		return true;
	}

	void touchCancel() override {
		Base::touchCancel();
		attractors_.clear();
		touchIDs_.clear();
	}

	void touchUpdate(const swa_touch_event& ev) override {
		Base::touchUpdate(ev);

		auto pos = attractorPos(nytl::Vec2i{ev.x, ev.y});
		auto it = std::find(touchIDs_.begin(), touchIDs_.end(), ev.id);
		if(it != touchIDs_.end()) {
			attractors_[it - touchIDs_.begin()] = pos;
		} else {
			dlg_error("Invalid touch id");
		}
	}

	argagg::parser argParser() const override {
		auto parser = Base::argParser();
		parser.definitions.push_back({
			"count",
			{"-c", "--count"},
			"Number of particles", 1
		});

#ifdef TKN_WITH_AUDIO
		parser.definitions.push_back({
			"audio",
			{"-a", "--audio"},
			"MP3 file to play and visualize", 1
		});
#endif // TKN_WITH_AUDIO

#ifdef TKN_WITH_PULSE_SIMPLE
		parser.definitions.push_back({
			"system-audio",
			{"-s", "--system-audio"},
			"Visualize system audio (using pulseaudio)", 0
		});
#endif // TKN_WITH_PULSE_SIMPLE

		return parser;
	}


	bool handleArgs(const argagg::parser_results& result,
			App::Args& bout) override {
		if (!Base::handleArgs(result, bout)) {
			return false;
		}

		auto& out = static_cast<Args&>(bout);
		if(result["count"].count()) {
			out.particleCount = result["count"].as<unsigned>();
		}

#ifdef TKN_WITH_AUDIO
		if(result.has_option("audio")) {
			out.audioFile = result["audio"].as<const char*>();
			if(out.audioFile.empty()) {
				dlg_info("The 'audio' option expects and argument");
				return false;
			}
		}
#endif // TKN_WITH_AUDIO

#ifdef TKN_WITH_PULSE_SIMPLE
		if(result.has_option("system-audio")) {
			out.systemAudio = true;
		}
#endif // TKN_WITH_AUDIO

		return true;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		system_.compute(cb);
	}

	void render(vk::CommandBuffer cb) override {
		system_.render(cb);
		gui().draw(cb);
	}

	void update(double delta) override {
		Base::update(delta);
		Base::scheduleRedraw();
		delta_ = delta;

		// audio processing
		// first get samples via feedback source or pulse
#ifdef TKN_WITH_AUDIO
		if(audio_) {
			auto available = audio_->available();
			dft_.samples.resize(available);
			audio_->dequeFeedback(dft_.samples.data(), available);
		}
#endif

#ifdef TKN_WITH_PULSE_SIMPLE
		if(pulseRecorder_) {
			auto available = pulseRecorder_->available();
			dft_.samples.resize(available);
			pulseRecorder_->deque(dft_.samples.data(), available);
		}
#endif

		// then perform fft on the samples to get the freq domain
#if defined(TKN_WITH_AUDIO) || defined(TKN_WITH_PULSE_SIMPLE)
		if(!dft_.samples.empty()) {
			auto ns = dft_.samples.size();
			// erase the old samples we don't need
			if(ns > 2 * frameCount) {
				dft_.samples.erase(dft_.samples.begin(),
					dft_.samples.begin() + (ns - 2 * frameCount));
			}

			// TODO: don't assume stereo
			dlg_assert(!(ns % 2));
			ns /= 2;

			auto ec = std::min<unsigned>(ns, dft_.time.size());
			dft_.time.erase(dft_.time.begin(), dft_.time.begin() + ec);
			dft_.time.reserve(frameCount);
			for(auto i = 0u; i < ns; ++i) {
				float r = 0.5 * (dft_.samples[2 * i] + dft_.samples[2 * i + 1]);
				dft_.time.push_back({r, 0.f});
			}

			kiss_fft(dft_.kiss, dft_.time.data(), dft_.freq.data());
		}
#endif
	}

	void updateDevice() override {
		Base::updateDevice();
		system_.updateDevice(delta_, attractors_);

		if(reset_) {
			reset_ = false;
			system_.reset();
		}

#if defined(TKN_WITH_AUDIO) || defined(TKN_WITH_PULSE_SIMPLE)
		if(factorBuf_.size()) {
			auto map = factorBuf_.memoryMap();
			auto vals = reinterpret_cast<float*>(map.ptr());

			auto i = 0u;
			auto size = 1.f;
			auto c = 0u;
			while(i < dft_.freq.size() / 4) {
				dlg_assert((std::byte*) vals < map.ptr() + map.size());

				auto sum = 0.f;
				unsigned end = i + size;
				for(; i < end; ++i) {
					auto& cpx = dft_.freq[i];
					auto amp = std::sqrt(cpx.r * cpx.r + cpx.i * cpx.i);
					sum += amp / frameCount;
				}

				// smooth the bars a bit over time (friction-like)
				// float ndt = 1 - std::pow(1000, -0.02); // TODO: use dt instead of 0.02
				float ndt = 1;
				// *vals += ndt * (std::pow(80.f * sum, 2) - *vals);
				*vals += ndt * (80 * sum - *vals);

				++c;
				++vals;
				size *= logFac;
			}

			map.flush();
		}
#endif // TKN_WITH_AUDIO
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		system_.ysize = float(h) / w;
		system_.paramsChanged = true;
	}

	const char* name() const override { return "particles"; }

protected:
	ParticleSystem system_;
	double delta_ {};
	bool reset_ {};
	std::vector<nytl::Vec2f> attractors_ {};
	std::vector<unsigned> touchIDs_ {};

#if defined(TKN_WITH_AUDIO) || defined(TKN_WITH_PULSE_SIMPLE)
	struct {
		std::vector<float> samples; // buffer cache for reading audio samples
		std::vector<kiss_fft_cpx> time; // time domain constructed from samples
		std::vector<kiss_fft_cpx> freq; // freq domain, output from fft
		kiss_fft_cfg kiss;
	} dft_;

	vpp::SubBuffer factorBuf_;
#endif

#ifdef TKN_WITH_AUDIO
	FeedbackMP3Audio* audio_ {};
	std::optional<tkn::AudioPlayer> audioPlayer_;
#endif // TKN_WITH_AUDIO

#ifdef TKN_WITH_PULSE_SIMPLE
	std::optional<tkn::PulseAudioRecorder> pulseRecorder_;
#endif
};

int main(int argc, const char** argv) {
	ParticlesApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

