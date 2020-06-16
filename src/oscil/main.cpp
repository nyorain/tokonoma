#include <tkn/config.hpp>
#include <tkn/singlePassApp.hpp>
#include <tkn/types.hpp>
#include <tkn/audioFeedback.hpp>
#include <tkn/bits.hpp>
#include <tkn/sound.hpp>
#include <tkn/sampling.hpp>
#include <tkn/audio.hpp>
#include <tkn/render.hpp>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/init.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/vk.hpp>

#include <nytl/math.hpp>
#include <dlg/dlg.hpp>
#include <argagg.hpp>

#include <shaders/oscil.particle.vert.h>
#include <shaders/tkn.color.frag.h>

#ifdef TKN_WITH_PULSE_SIMPLE
  #include <tkn/pulseRecorder.hpp>
#endif // TKN_WITH_PULSE_SIMPLE

using nytl::constants::pi;
using namespace tkn::types;

class Source : public tkn::AudioSource {
public:
	float amp = 0.5;
	float freq0 = 440.0;
	float freq1 = 440.0;

	void write(float& dst, float val, bool mix) {
		dst = mix ? dst + val : val;
	}

	void render(unsigned nb, float* buf, bool mix) {
		auto nf = tkn::AudioPlayer::blockSize * nb;
		float rps0 = freq0 * 2.0 * pi;
		float rps1 = freq1 * 2.0 * pi;
		for(auto i = 0u; i < nf; ++i) {
			auto dst = buf + i * 2;
			write(dst[0], amp * std::sin(soffset0_ + rps0 * (i * spf_)), mix);
			write(dst[1], amp * std::cos(soffset1_ + rps1 * (i * spf_)), mix);
		}

        soffset0_ += rps0 * spf_ * nf;
        soffset1_ += rps1 * spf_ * nf;
	}

	double soffset0_ {};
	double soffset1_ {};
	double spf_ {}; // second per frame
};

class Oscil : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	// 2 * 44100 or 2 * 48000 is one second
	static constexpr auto maxFrameCount = 2 * 128 * 1024u;

	struct Args : public Base::Args {
		std::string mp3File {};
		bool systemAudio {};
	};

public:
	bool init(nytl::Span<const char*> cargs) override {
		Args args;
		if(!Base::doInit(cargs, args)) {
			return false;
		}

		auto& dev = vkDevice();
		auto bindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer)
		};

		dsLayout_.init(dev, bindings);
		pipeLayout_ = {dev, {{dsLayout_}}, {}};

		nytl::Vec4f color{0.f, 1.f, 0.f, 0.2f};

		vk::SpecializationMapEntry sentries[4];
		for(auto i = 0u; i < 4; ++i) {
			sentries[i].constantID = i;
			sentries[i].offset = 4 * i;
			sentries[i].size = 4u;
		}

		vk::SpecializationInfo sci;
		sci.dataSize = sizeof(nytl::Vec4f);
		sci.pData = &color;
		sci.mapEntryCount = 4u;
		sci.pMapEntries = sentries;

		vpp::ShaderModule vert{dev, oscil_particle_vert_data};
		vpp::ShaderModule frag{dev, tkn_color_frag_data};

		vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment, &sci},
		}}}, 0u, samples());

		gpi.assembly.topology = vk::PrimitiveTopology::pointList;
		gpi.blend.pAttachments = &tkn::defaultBlendAttachment();

		pipe_ = {dev, gpi.info()};

		// buffers
		auto initUbo = vpp::Init<vpp::SubBuffer>(dev.bufferAllocator(),
			3 * sizeof(float), vk::BufferUsageBits::uniformBuffer,
			dev.hostMemoryTypes());
		auto initSsbo = vpp::Init<vpp::SubBuffer>(dev.bufferAllocator(),
			2 * maxFrameCount * sizeof(float), vk::BufferUsageBits::storageBuffer,
			dev.hostMemoryTypes());

		ubo_ = initUbo.init();
		ssbo_ = initSsbo.init();

		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform({{{ubo_}}});
		dsu.storage({{{ssbo_}}});
		dsu.apply();

		// audio
		ap_.init();

		bool useOwn = true;
		if(!args.mp3File.empty()) {
			auto asset = openAsset(args.mp3File);
			if(!asset) {
				dlg_error("Couldn't open {}", args.mp3File);
				return false;
			}

			mp3Source_ = &ap_.create<FeedbackMP3Audio>(ap_, std::move(asset));
			useOwn = false;
		}

#ifdef TKN_WITH_PULSE_SIMPLE
		if(args.systemAudio) {
			if(mp3Source_) {
				dlg_error("Can't use audio file and system audio");
				return false;
			}

			paRec_.init();
			useOwn = false;
		}
#endif

		if(useOwn) {
			oSource_ = &ap_.create<ASource>();
			oSource_->inner().spf_ = 1.f / ap_.rate();
		}

		ap_.start();

		// initialize with 0-samples
		audioSamples_.resize(2 * frameCount_);
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {{ds_}});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdDraw(cb, particleCount_, 1, 0, 0);
	}

	void update(double dt) override {
		Base::update(dt);
		Base::scheduleRedraw();

		// read avilable frames but always but to frameCount
		if(oSource_) {
			auto available = oSource_->available();
			dlg_assert(available % 2 == 0);

			auto off = audioSamples_.size();
			audioSamples_.resize(audioSamples_.size() + available);
			oSource_->dequeFeedback(audioSamples_.data() + off, available);
		} else if(mp3Source_) {
			auto available = mp3Source_->available();
			dlg_assert(available % 2 == 0);

			auto off = audioSamples_.size();
			audioSamples_.resize(audioSamples_.size() + available);
			mp3Source_->dequeFeedback(audioSamples_.data() + off, available);
#ifdef TKN_WITH_PULSE_SIMPLE
		} else {
			auto available = paRec_.available();
			dlg_assert(available % 2 == 0);

			auto off = audioSamples_.size();
			audioSamples_.resize(audioSamples_.size() + available);
			paRec_.deque(audioSamples_.data() + off, available);
#endif // TKN_WITH_PULSE_SIMPLE
		}

		if(advanceTime_) {
			auto maxCount = 2 * maxFrameCount;

			// NOTE: this can be improved. Especially at the beginning,
			// we will always advance the buffer and will it up with
			// zeros...
			auto cut = 2 * unsigned(std::round(dt * ap_.rate()));
			// auto cut = 0u;
			if(audioSamples_.size() < 2 * frameCount_) {
				dlg_info("0");
				cut = 0u;
			} else if(audioSamples_.size() - cut < 2 * frameCount_) {
				cut = audioSamples_.size() - 2 * frameCount_;
			} else if(audioSamples_.size() - cut > 2 * maxCount) {
				dlg_info("2");
				cut = audioSamples_.size() - 2 * maxCount;
			}

			dlg_info("{}", cut);
			auto d = audioSamples_.data();
			auto ns = audioSamples_.size() - cut;
			dlg_assertm(ns >= 2 * frameCount_, "{}, {}", ns, 2 * frameCount_);
			std::memmove(d, d + cut, ns * sizeof(float));

			audioSamples_.resize(std::max<u32>(ns, 2 * frameCount_));
		} else {
			if(audioSamples_.size() > 2 * frameCount_) {
				auto d = audioSamples_.data();
				auto cut = audioSamples_.size() - 2 * frameCount_;
				std::memmove(d, d + cut, 2 * frameCount_ * sizeof(float));
			}

			audioSamples_.resize(2 * frameCount_);
		}
	}

	void updateDevice() override {
		Base::updateDevice();

		// TODO: conditional update. We don't need this every frame I guess
		// or progress own audio sample buffer every frame?
		{
			auto bytes = tkn::bytes(audioSamples_);
			bytes = bytes.first(2 * frameCount_ * sizeof(float));

			auto map = ssbo_.memoryMap(0, bytes.size());
			auto span = map.span();
			tkn::write(span, bytes);
		}

		{
			auto map = ubo_.memoryMap();
			auto span = map.span();
			tkn::write(span, float(frameCount_) / float(particleCount_));
			tkn::write(span, amp_);
			tkn::write(span, frameCount_);
		}
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(ev.pressed == true) {
			if(oSource_) {
				switch(ev.keycode) {
					case swa_key_up:
						oSource_->inner().freq0 *= 1.002;
						dlg_info("freq0: {}", oSource_->inner().freq0);
						return true;
					case swa_key_down:
						oSource_->inner().freq0 /= 1.002;
						dlg_info("freq0: {}", oSource_->inner().freq0);
						return true;
					case swa_key_right:
						oSource_->inner().freq1 *= 1.002;
						dlg_info("freq1: {}", oSource_->inner().freq1);
						return true;
					case swa_key_left:
						oSource_->inner().freq1 /= 1.002;
						dlg_info("freq1: {}", oSource_->inner().freq1);
						return true;
					default: break;
				}
			}

			switch(ev.keycode) {
				case swa_key_a:
					advanceTime_ = !advanceTime_;
					dlg_info("advanceTime: {}", advanceTime_);
					return true;
				case swa_key_pageup:
					amp_ *= 1.1;
					dlg_info("amp: {}", amp_);
					return true;
				case swa_key_pagedown:
					amp_ /= 1.1;
					dlg_info("amp: {}", amp_);
					return true;

				// Only allow these big steps to keep frameCount a power of 2
				// TODO: when using pulse recording, we might wanna adjust
				// buffer size? not sure how that works exactly
				case swa_key_o:
					frameCount_ = std::min(maxFrameCount, frameCount_ * 2);
					dlg_info("frameCount: {}", frameCount_);
					return true;
				case swa_key_i:
					frameCount_ /= 2;
					dlg_info("frameCount: {}", frameCount_);
					return true;

				default: break;
			}
		}

		return false;
	}

	argagg::parser argParser() const override {
		auto parser = Base::argParser();
		parser.definitions.push_back({
			"particle-count",
			{"-p", "--particle-count"},
			"Number of particles", 1
		});

		parser.definitions.push_back({
			"frame-count",
			{"-f", "--frame-count"},
			"Number of audio frames to display per visual frame", 1
		});

		parser.definitions.push_back({
			"audio",
			{"-a", "--audio"},
			"MP3 file to play and visualize", 1
		});

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
		if(!Base::handleArgs(result, bout)) {
			return false;
		}

		auto& out = static_cast<Args&>(bout);
		if(auto& r = result["particle-count"]; r) {
			particleCount_ = r.as<unsigned>();
		}

		if(auto& r = result["frame-count"]; r) {
			frameCount_ = r.as<unsigned>();
			if(frameCount_ > maxFrameCount) {
				dlg_warn("given frame count too large");
				frameCount_ = maxFrameCount;
			}
		}

		if(auto& r = result["audio"]; r) {
			out.mp3File = r.as<const char*>();
		}

#ifdef TKN_WITH_PULSE_SIMPLE
		if(result.has_option("system-audio")) {
			out.systemAudio = true;
		}
#endif // TKN_WITH_AUDIO

		return true;
	}

	const char* name() const override { return "oscil"; }
	bool needsDepth() const override { return false; }

private:
	using ASource = tkn::FeedbackAudioSource<Source>;
	using FeedbackMP3Audio = tkn::FeedbackAudioSource<tkn::StreamedMP3Audio>;

	vpp::SubBuffer ubo_;
	vpp::SubBuffer ssbo_;

	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::TrDs ds_;

	tkn::AudioPlayer ap_;
	ASource* oSource_ {};
	FeedbackMP3Audio* mp3Source_ {};

	unsigned particleCount_ {64 * 1024};
	u32 frameCount_ {8 * 1024};

	float amp_ {1.f};
	bool advanceTime_ {true};

#ifdef TKN_WITH_PULSE_SIMPLE
	tkn::PulseAudioRecorder paRec_;
#endif

	std::vector<float> audioSamples_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<Oscil>(argc, argv);
}

