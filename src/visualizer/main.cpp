#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/audio.hpp>
#include <tkn/sound.hpp>
#include <tkn/sampling.hpp>
#include <tkn/kiss_fft.h>
#include <dlg/dlg.hpp>
#include <rvg/context.hpp>
#include <rvg/state.hpp>
#include <rvg/shapes.hpp>

// TODO: would be cleaner to do all feedback stuff in update if possible
// (e.g. for streamed sources)
template<typename T>
class FeedbackAudioSource : public tkn::AudioSource {
public:
	template<typename... Args>
	FeedbackAudioSource(Args&&... args) : impl_(std::forward<Args>(args)...) {}

	void update() override {
		impl_.update();
	}

	void render(unsigned nb, float* buf, bool mix) override {
		auto ns = 2 * nb * tkn::AudioPlayer::blockSize; // TODO: don't assume stereo
		float* feedback;
		if(!mix) {
			feedback = buf;
			impl_.render(nb, buf, mix);
		} else {
			tmpBuf_.resize(ns);
			feedback = tmpBuf_.data();
			impl_.render(nb, feedback, false);
			for(auto i = 0u; i < ns; ++i) {
				buf[i] += feedback[i];
			}
		}

		feedback_.enque(feedback, ns);
	}

	unsigned available() const {
		return feedback_.available_read();
	}

	unsigned dequeFeedback(float* buf, unsigned ns) {
		feedback_.deque(buf, ns);
	}

protected:
	T impl_;
	std::vector<float> tmpBuf_; // TODO: use buf cache

	static constexpr auto bufSize = 48000 * 2 * 10;
	tkn::RingBuffer<float> feedback_{bufSize};
};

class Visualizer : public tkn::App {
public:
	using FeedbackMP3Audio = FeedbackAudioSource<tkn::StreamedMP3Audio>;
	static constexpr auto batchSize = 1024u;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		ap_.emplace();
		auto& ap = *ap_;

		auto asset = openAsset("test.mp3");
		audio_ = &ap.create<FeedbackMP3Audio>(ap, std::move(asset));

		sampleBuf_.resize(batchSize * 2);
		timeBuf_.resize(batchSize);
		freqBuf_.resize(batchSize);
		fft_ = kiss_fft_alloc(batchSize, 0, nullptr, nullptr);

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

		while(audio_->available() >= 2 * batchSize) {
			audio_->dequeFeedback(sampleBuf_.data(), 2 * batchSize);
			for(auto i = 0u; i < batchSize; ++i) {
				timeBuf_[i].r = 0.5 * (sampleBuf_[2 * i] + sampleBuf_[2 * i + 1]);
				timeBuf_[i].i = 0.f;
			}

			kiss_fft(fft_, timeBuf_.data(), freqBuf_.data());
			// dlg_info("====================================");

			// auto count = batchSize / 4;
			// auto blocks = 20u;
			// for(auto b = 0u; b < blocks; ++b) {
			// 	auto start = b * (count / float(blocks));
			// 	auto end = (b + 1) * (count / float(blocks));
			// 	float sum = 0.f;
			// 	for(auto i = start; i < end; ++i) {
			// 		auto& cpx = freqBuf_[i];
			// 		auto amp = std::sqrt(cpx.r * cpx.r + cpx.i * cpx.i);
			// 		sum += amp / batchSize;
			// 	}
			// 	dlg_info("  {}: {}", b, 30 * sum);
			// }

			// logarithmic bar spacing
			// https://www.audiocheck.net/soundtests_nonlinear.php
			auto i = 0u;
			auto size = 1.f;
			auto c = 0u;
			while(i < freqBuf_.size() / 2) {
				auto sum = 0.f;
				auto end = i + size;
				for(; i < end; ++i) {
					auto& cpx = freqBuf_[i];
					auto amp = std::sqrt(cpx.r * cpx.r + cpx.i * cpx.i);
					sum += amp / batchSize;
					// sum += amp;
				}
				// dlg_info("  {}: {}", c, sum / size);

				if(c < bars_.size()) {
					bars_[c].change()->size.y = 2000 * sum;
				}

				++c;
				size *= 1.1;
			}

			// auto sum = 0.f;
			// for(auto& cpx : freqBuf_) {
			// 	auto amp = std::sqrt(cpx.r * cpx.r + cpx.i * cpx.i);
			// 	sum += amp / batchSize;
			// }
//
			// dlg_info("sum: {}", sum);
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

