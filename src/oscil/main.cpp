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

#include <rvg/context.hpp>
#include <rvg/shapes.hpp>
#include <rvg/paint.hpp>
#include <rvg/text.hpp>

#include <nytl/math.hpp>
#include <dlg/dlg.hpp>
#include <argagg.hpp>

#include <shaders/oscil.particle.vert.h>
#include <shaders/tkn.incolor.frag.h>

#ifdef TKN_WITH_PULSE_SIMPLE
  #include <tkn/pulseRecorder.hpp>
#endif // TKN_WITH_PULSE_SIMPLE

using nytl::constants::pi;
using namespace tkn::types;

// TODO: allow loading of wav files as there are some HD osciloscope
//   wav files on the internet.
// TODO: when loading a file from disk, allow stepping through it.
// TODO: maybe optionally implement some temporal super sampling stuff?
//   think about why we even really see the signal on a real oscilloscope
//   (because at any given time it basically just draws a single point)
//   and try to simulate that more closely.

// TODO: should be moved to tkn general headers
// TODO: add groups? like "1/2: increase/decrase value"
// TODO: the associated values should probably be displayed here and be
//   input fields. At that point we basically have numeral text fields
//   controlled by shortcuts.
class ShortcutWidget {
public:
	using Handler = std::function<void(swa_key)>;
	static constexpr float width = 300;
	static constexpr float entryHeight = 20;
	static constexpr float nameWidth = 50;
	static constexpr float textSize = 14.f;
	static constexpr float yPad = entryHeight;
	static constexpr float xPad = yPad;

public:
	ShortcutWidget() = default;

	void init(rvg::Context& ctx, const rvg::Font& font);
	bool add(swa_key key, std::string description, Handler handler);
	void hide(bool h);
	bool key(swa_key key, bool pressed);
	void draw(vk::CommandBuffer);
	void move(nytl::Vec2f pos);

	auto& rvgContext() const { return *ctx_; }

private:
	struct Shortcut {
		swa_key key;
		Handler handler;
		rvg::Text keyText;
		rvg::Text descriptionText;
	};

	rvg::Context* ctx_ {};
	rvg::Font font_ {};

	std::unordered_map<swa_key, u32> idMap_;
	std::vector<Shortcut> shortcuts_;
	rvg::RectShape bg_;
	rvg::Paint bgPaint_;
	rvg::Paint fgPaint_;
	float y_ {yPad};
};

void ShortcutWidget::init(rvg::Context& ctx, const rvg::Font& font) {
	ctx_ = &ctx;
	font_ = font;

	bgPaint_ = {rvgContext(), rvg::colorPaint({20, 20, 20, 200})};
	fgPaint_ = {rvgContext(), rvg::colorPaint({230, 230, 230, 255})};

	auto dm = rvg::DrawMode {};
	dm.deviceLocal = true;
	dm.fill = true;
	dm.stroke = 0.f;
	dm.aaFill = false;
	bg_ = {rvgContext(), {}, {}, dm};
}

bool ShortcutWidget::add(swa_key key, std::string desc, Handler handler) {
	auto id = shortcuts_.size();
	if(!idMap_.emplace(key, id).second) {
		dlg_debug("ShortcutWidget: key already has shortcut");
		return false; // shortcut was already present
	}

	auto& s = shortcuts_.emplace_back();
	s.key = key;
	s.handler = std::move(handler);

	auto pos = nytl::Vec2f{bg_.position().x + xPad, y_};
	auto keyName = swa_key_to_name(key);
	auto kn = std::string(keyName ? keyName : "???") + ": ";
	s.keyText = {rvgContext(), pos, std::move(kn), font_, textSize};

	pos.x += nameWidth;
	s.descriptionText = {rvgContext(), pos, std::move(desc), font_, textSize};


	auto bgc = bg_.change();
	bgc->size.x = width;
	bgc->size.y = (y_ + yPad) - bgc->position.y;

	y_ += entryHeight;
	return true;
}

void ShortcutWidget::hide(bool h) {
	bg_.disable(h);

	for(auto& e : shortcuts_) {
		e.keyText.disable(h);
		e.descriptionText.disable(h);
	}
}
bool ShortcutWidget::key(swa_key key, bool pressed) {
	auto it = idMap_.find(key);
	if(it == idMap_.end()) {
		return false;
	}

	if(!pressed) {
		return false;
	}

	dlg_assert(shortcuts_.size() > it->second);
	shortcuts_[it->second].handler(key);

	return true;
}

void ShortcutWidget::draw(vk::CommandBuffer cb) {
	bgPaint_.bind(cb);
	bg_.fill(cb);

	fgPaint_.bind(cb);
	for(auto& e : shortcuts_) {
		e.keyText.draw(cb);
		e.descriptionText.draw(cb);
	}
}

void ShortcutWidget::move(nytl::Vec2f pos) {
	y_ = pos.y + yPad;
	bg_.change()->position = pos;

	// update all entries
	pos.x += xPad;
	for(auto& entry : shortcuts_) {
		entry.keyText.change()->position = {pos.x, y_};
		entry.descriptionText.change()->position = {pos.x + nameWidth, y_};
		y_ += entryHeight;
	}
}

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

	struct UboData {
		float idstep;
		float amp;
		u32 frameCount;
		float alpha;
		float thickness;
		float fadeExp;
		float ySize;
	};

	struct Args : public Base::Args {
		std::string streamFile {};
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

		// for tkn/color.frag
		vpp::ShaderModule vert{dev, oscil_particle_vert_data};
		vpp::ShaderModule frag{dev, tkn_incolor_frag_data};

		vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment /*, &sci*/},
		}}}, 0u, samples());

		gpi.assembly.topology = vk::PrimitiveTopology::pointList;
		gpi.blend.attachmentCount = 1u;

		// simple additive blending.
		const vk::PipelineColorBlendAttachmentState ba = {
			true, // blending enabled
			// color
			vk::BlendFactor::srcAlpha, // src
			vk::BlendFactor::one, // dst
			vk::BlendOp::add,
			// alpha
			// Rationale behind using the dst alpha is that there
			// is no use in storing the src alpha somewhere, as
			// we've already processed it via the color blending above.
			vk::BlendFactor::zero, // src
			vk::BlendFactor::one, // dst
			vk::BlendOp::add,
			// color write mask
			vk::ColorComponentBits::r |
				vk::ColorComponentBits::g |
				vk::ColorComponentBits::b |
				vk::ColorComponentBits::a,
		};
		gpi.blend.pAttachments = &ba;

		pipe_ = {dev, gpi.info()};

		// buffers
		auto initUbo = vpp::Init<vpp::SubBuffer>(dev.bufferAllocator(),
			sizeof(UboData), vk::BufferUsageBits::uniformBuffer,
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
		if(!args.streamFile.empty()) {
			// TODO: wav constructor from File
			// auto asset = openAsset(args.streamFile);
			// if(!asset) {
			// 	dlg_error("Couldn't open {}", args.streamFile);
			// 	return false;
			// }
			// streamedSource_ = &ap_.create<FeedbackAudio>(ap_, std::move(asset));

			streamedSource_ = &ap_.create<FeedbackAudio>(ap_, args.streamFile);
			useOwn = false;
		}

#ifdef TKN_WITH_PULSE_SIMPLE
		if(args.systemAudio) {
			if(streamedSource_) {
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

		// shortcuts
		rvgInit();
		shortcuts_.init(rvgContext(), defaultFont());
		if(oSource_) {
			auto alterFreqFunc = [this](float x, float y) {
				return [this, x, y](swa_key){
					oSource_->inner().freq0 *= x;
					oSource_->inner().freq1 *= y;
					dlg_info("freq0: {}", oSource_->inner().freq0);
					dlg_info("freq1: {}", oSource_->inner().freq1);
				};
			};

			auto step = 1.005;
			shortcuts_.add(swa_key_up, "Increase x frequency",
				alterFreqFunc(step, 0.f));
			shortcuts_.add(swa_key_down, "Descrease x frequency",
				alterFreqFunc(1 / step, 0.f));
			shortcuts_.add(swa_key_right, "Increase y frequency",
				alterFreqFunc(0.f, step));
			shortcuts_.add(swa_key_left, "Descrease y frequency",
				alterFreqFunc(0.f, 1 / step));

		}

		shortcuts_.add(swa_key_p, "Toggle pause", [this](swa_key) {
			paused_ = !paused_;
			dlg_info("paused: {}", paused_);

			if(streamedSource_) {
				auto& inner = streamedSource_->inner();
				inner.volume(paused_ ? tkn::volumePause : 1.f);
			}
		});
		shortcuts_.add(swa_key_a, "Toggle advanceTime", [this](swa_key) {
			advanceTime_ = !advanceTime_;
			dlg_info("advanceTime: {}", advanceTime_);
		});
		shortcuts_.add(swa_key_i, "Increase amplitude", [this](swa_key) {
			amp_ *= 1.1;
			dlg_info("amp: {}", amp_);
		});
		shortcuts_.add(swa_key_o, "Decrease amplitude", [this](swa_key) {
			amp_ /= 1.1;
			dlg_info("amp: {}", amp_);
		});
		shortcuts_.add(swa_key_k1, "Increase alpha", [this](swa_key) {
			alpha_ *= 1.1;
			dlg_info("alpha: {}", alpha_);
		});
		shortcuts_.add(swa_key_k2, "Decrease alpha", [this](swa_key) {
			alpha_ /= 1.1;
			dlg_info("alpha: {}", alpha_);
		});
		shortcuts_.add(swa_key_k3, "Increase faceExp", [this](swa_key) {
			fadeExp_ *= 1.1;
			dlg_info("fadeExp: {}", fadeExp_);
		});
		shortcuts_.add(swa_key_k4, "Decrease faceExp", [this](swa_key) {
			fadeExp_ /= 1.1;
			dlg_info("fadeExp: {}", fadeExp_);
		});
		shortcuts_.add(swa_key_k5, "Increase thickness", [this](swa_key) {
			thickness_ *= 1.1;
			dlg_info("thickness: {}", thickness_);
		});
		shortcuts_.add(swa_key_k6, "Decrease thickness", [this](swa_key) {
			thickness_ /= 1.1;
			dlg_info("thickness: {}", thickness_);
		});
		shortcuts_.add(swa_key_x, "Increase frame count", [this](swa_key) {
			frameCount_ = std::clamp<unsigned>(frameCount_ * 1.1,
				frameCount_ + 1, maxFrameCount);
			dlg_info("frameCount: {}", frameCount_);
		});
		shortcuts_.add(swa_key_z, "Decrease frame count", [this](swa_key) {
			frameCount_ = std::clamp<unsigned>(frameCount_ / 1.1, 1, maxFrameCount);
			dlg_info("frameCount: {}", frameCount_);
		});

		ap_.start();

		// initialize with 0-samples
		audioSamples_.resize(2 * frameCount_);
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {{ds_}});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdDraw(cb, particleCount_, 1, 0, 0);

		rvgContext().bindDefaults(cb);
		rvgWindowTransform().bind(cb);
		shortcuts_.draw(cb);
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
		} else if(streamedSource_) {
			auto available = streamedSource_->available();
			dlg_assert(available % 2 == 0);

			auto off = audioSamples_.size();
			audioSamples_.resize(audioSamples_.size() + available);
			streamedSource_->dequeFeedback(audioSamples_.data() + off, available);
#ifdef TKN_WITH_PULSE_SIMPLE
		} else {
			auto available = paRec_.available();
			dlg_assert(available % 2 == 0);

			auto off = audioSamples_.size();
			audioSamples_.resize(audioSamples_.size() + available);
			paRec_.deque(audioSamples_.data() + off, available);
#endif // TKN_WITH_PULSE_SIMPLE
		}

		if(!paused_ && advanceTime_) {
			auto maxCount = 2 * maxFrameCount;

			// NOTE: this can be improved. Especially at the beginning,
			// we will always advance the buffer and will it up with
			// zeros...
			auto cut = 2 * unsigned(std::round(dt * ap_.rate()));
			// auto cut = 0u;
			if(audioSamples_.size() < 2 * frameCount_) {
				cut = 0u;
			} else if(audioSamples_.size() < 2 * frameCount_ + cut) {
				if(audioSamples_.size() < 2 * frameCount_) {
					cut = 0u;
				} else {
					cut = audioSamples_.size() - 2 * frameCount_;
				}
			} else if(audioSamples_.size() > 2 * maxCount + cut) {
				cut = audioSamples_.size() - 2 * maxCount;
			}

			auto d = audioSamples_.data();
			auto ns = audioSamples_.size() - cut;
			// this might be false when we just increased the frameCount_
			// dlg_assertm(ns >= 2 * frameCount_, "{}, {}", ns, 2 * frameCount_);
			std::memmove(d, d + cut, ns * sizeof(float));

			audioSamples_.resize(std::max<u32>(ns, 2 * frameCount_));
		} else if(!paused_) {
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
		auto sc = std::min<u32>(2 * frameCount_, audioSamples_.size());
		auto fc = sc / 2;

		// TODO: conditional update. We don't need this every frame I guess
		// or progress own audio sample buffer every frame?
		{
			auto bytes = tkn::bytes(audioSamples_);
			bytes = bytes.first(sc * sizeof(float));

			auto map = ssbo_.memoryMap(0, bytes.size());
			auto span = map.span();
			tkn::write(span, bytes);
		}

		{
			auto map = ubo_.memoryMap();
			auto span = map.span();

			UboData data;
			data.alpha = alpha_;
			data.amp = amp_;
			data.frameCount = fc;
			data.idstep = float(fc) / (float(particleCount_));
			data.thickness = thickness_;
			data.fadeExp = fadeExp_;
			data.ySize = ysize_;
			tkn::write(span, data);
		}
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		return shortcuts_.key(ev.keycode, ev.pressed);
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
			"audio (WAV) file to play and visualize", 1
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
			out.streamFile = r.as<const char*>();
		}

#ifdef TKN_WITH_PULSE_SIMPLE
		if(result.has_option("system-audio")) {
			out.systemAudio = true;
		}
#endif // TKN_WITH_AUDIO

		return true;
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		ysize_ = float(h) / w;

		// update time widget position
		auto pos = nytl::Vec2ui{w, h};
		pos.x -= 240;
		pos.y = 10;
		shortcuts_.move(nytl::Vec2f(pos));
	}

	const char* name() const override { return "oscil"; }
	bool needsDepth() const override { return false; }

private:
	using ASource = tkn::FeedbackAudioSource<Source>;
	using FeedbackAudio = tkn::FeedbackAudioSource<tkn::StreamedWavAudio>;

	ShortcutWidget shortcuts_;

	vpp::SubBuffer ubo_;
	vpp::SubBuffer ssbo_;

	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::TrDs ds_;

	tkn::AudioPlayer ap_;
	ASource* oSource_ {};
	FeedbackAudio* streamedSource_ {};

	unsigned particleCount_ {64 * 1024};
	u32 frameCount_ {2 * 1024};

	float thickness_ {0.0005};
	float fadeExp_ {0.0025};
	float ysize_ {1.f};
	float alpha_ {0.1f};
	float amp_ {1.f};
	bool advanceTime_ {true};
	bool paused_ {false};

#ifdef TKN_WITH_PULSE_SIMPLE
	tkn::PulseAudioRecorder paRec_;
#endif

	std::vector<float> audioSamples_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<Oscil>(argc, argv);
}

