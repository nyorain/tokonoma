#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/audio.hpp>
#include <tkn/sound.hpp>
#include <tkn/sampling.hpp>
#include <tkn/audioFeedback.hpp>
#include <tkn/kiss_fft.h>
#include <dlg/dlg.hpp>
#include <rvg/context.hpp>
#include <rvg/state.hpp>
#include <rvg/shapes.hpp>

class Visualizer : public tkn::App {
public:
	using FeedbackMP3Audio = tkn::FeedbackAudioSource<tkn::StreamedMP3Audio>;

	// On how many frames to perform the DFT.
	static constexpr auto frameCount = 1024;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		ap_.emplace();
		auto& ap = *ap_;

		auto asset = openAsset("test.mp3");
		audio_ = &ap.create<FeedbackMP3Audio>(ap, std::move(asset));

		timeBuf_.resize(frameCount);
		freqBuf_.resize(frameCount);
		fft_ = kiss_fft_alloc(frameCount, 0, nullptr, nullptr);

		paint_ = {rvgContext(), rvg::colorPaint({255, 255, 255, 255})};

		auto width = 20.f;
		auto startx = 50.f;
		auto spacex = 5.f;
		auto starty = 100.f;
		for(auto i = 0u; i < 100; ++i) {
			auto x = startx + i * (width + spacex);
			rvg::DrawMode dm;
			dm.fill = true;
			bars_.push_back({rvgContext(), {x, starty}, {width, 0}, dm});
		}

		ap.start();
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		rvgContext().bindDefaults(cb);
		windowTransform().bind(cb);
		paint_.bind(cb);
		for(auto& bar : bars_) {
			bar.fill(cb);
		}
	}

	void update(double dt) override {
		App::update(dt);
		App::scheduleRedraw();

		auto available = audio_->available();
		sampleBuf_.resize(available);
		audio_->dequeFeedback(sampleBuf_.data(), available);

		if(available > 2 * frameCount) {
			sampleBuf_.erase(sampleBuf_.begin(),
				sampleBuf_.begin() + (available - 2 * frameCount));
		}

		dlg_assert(!(available % 2));
		available /= 2; // TODO: don't assume stereo

		auto ec = std::min<unsigned>(available, timeBuf_.size());
		if(timeBuf_.size() < frameCount) {
			ec = ec - std::max<int>(frameCount - timeBuf_.size(), 0);
		}

		timeBuf_.erase(timeBuf_.begin(), timeBuf_.begin() + ec);
		timeBuf_.reserve(frameCount);
		for(auto i = 0u; i < available; ++i) {
			float r = 0.5 * (sampleBuf_[2 * i] + sampleBuf_[2 * i + 1]);
			timeBuf_.push_back({r, 0.f});
		}

		kiss_fft(fft_, timeBuf_.data(), freqBuf_.data());

		// logarithmic bar spacing
		// https://www.audiocheck.net/soundtests_nonlinear.php
		// TODO: make number of bars independent from the number
		// of frames used for fft (i.e. the number of relative
		// frequencies returned).
		auto i = 0u;
		auto size = 1.f;
		auto c = 0u;
		while(i < freqBuf_.size() / 2) {
			auto sum = 0.f;
			auto end = i + size;
			for(; i < end; ++i) {
				auto& cpx = freqBuf_[i];
				auto amp = std::sqrt(cpx.r * cpx.r + cpx.i * cpx.i);
				sum += amp / frameCount;
			}

			// smooth the bars a bit over time (friction-like)
			float ndt = 1 - std::pow(1000, -dt);
			if(c < bars_.size()) {
				auto tc = bars_[c].change();
				tc->size.y += ndt * (2000 * sum - tc->size.y);
			}

			++c;
			size *= 1.1;
		}
	}

	const char* name() const override { return "visualizer"; }

protected:
	std::optional<tkn::AudioPlayer> ap_;
	FeedbackMP3Audio* audio_;
	kiss_fft_cfg fft_;

	std::vector<float> sampleBuf_;
	std::vector<kiss_fft_cpx> timeBuf_;
	std::vector<kiss_fft_cpx> freqBuf_;

	std::vector<rvg::RectShape> bars_;
	rvg::Paint paint_;
};

int main(int argc, const char** argv) {
	Visualizer app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

