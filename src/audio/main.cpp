#include <tkn/audio-cubeb.hpp>
#include <tkn/ringbuffer.hpp>
#include <speex_resampler.h>
#include <nytl/math.hpp>
#include <nytl/stringParam.hpp>
#include <dlg/dlg.hpp>
#include <stb_vorbis.h>

#include <thread>
#include <chrono>
#include <cmath>

using tkn::acb::Audio;
using tkn::acb::AudioPlayer;

class DummyAudio : public Audio {
public:
	void render(const AudioPlayer& ap, float* buf, unsigned nf) override {
        auto pitch = 440.0;
        auto radsPerSec = pitch * 2.0 * nytl::constants::pi;
		auto secondsPerFrame = 1.0 / ap.rate();

		for(auto i = 0u; i < nf; ++i) {
			float val = 0.1 * std::sin((soffset + i * secondsPerFrame) * radsPerSec);
			for(auto c = 0u; c < ap.channels(); ++c) {
				buf[i * ap.channels() + c] += val;
			}
		}

        soffset += secondsPerFrame * nf;
	}

	double soffset {};
};

struct UniqueSoundBuffer {
	std::size_t frameCount;
	unsigned channelCount;
	unsigned rate; // in Hz
	std::unique_ptr<float[]> data; // interleaved
};

struct SoundBufferView {
	std::size_t frameCount;
	unsigned channelCount;
	unsigned rate; // in Hz
	float* data; // interleaved

	SoundBufferView() = default;
	SoundBufferView(UniqueSoundBuffer& rhs) :
		frameCount(rhs.frameCount),
		channelCount(rhs.channelCount),
		rate(rhs.rate),
		data(rhs.data.get()) {}
};


void resample(SoundBufferView dst, SoundBufferView src) {
	if(dst.rate == src.rate) {
		dlg_info("resampling not needed");
		dlg_assert(dst.frameCount == src.frameCount);
		if(dst.channelCount == src.channelCount) {
			auto nbytes = dst.frameCount * src.channelCount * sizeof(float);
			memcpy(dst.data, src.data, nbytes);
		} else {
			auto nbytes = dst.frameCount * sizeof(float);
			for(auto i = 0u; i < dst.channelCount; ++i) {
				// TODO: not sure if that is a good idea, see below
				auto srcc = i % src.channelCount;
				memcpy(dst.data + i, src.data + srcc, nbytes);
			}
		}

		return;
	}

	int err;
	auto speex = speex_resampler_init(src.channelCount,
		src.rate, dst.rate, SPEEX_RESAMPLER_QUALITY_DESKTOP, &err);
	dlg_assert(speex);

	speex_resampler_set_input_stride(speex, src.channelCount);
	speex_resampler_set_output_stride(speex, dst.channelCount);
	for(auto i = 0u; i < dst.channelCount; ++i) {
		spx_uint32_t in_len = src.frameCount;
		spx_uint32_t out_len = dst.frameCount;
		// TODO: not sure if that is a good idea.
		// could be optimized in that case anyways, we don't
		// have to resample the same channel multiple times.
		// Lookup surround sound mixing.
		auto srcc = i % src.channelCount;
		err = speex_resampler_process_float(speex, srcc,
			src.data + srcc, &in_len, dst.data + i, &out_len);
		dlg_assertm(in_len == src.frameCount, "{} vs {}",
			in_len, src.frameCount);
		dlg_assertm(out_len == dst.frameCount, "{} vs {}",
			out_len, dst.frameCount);
	}

	dlg_info("resampling done");
	speex_resampler_destroy(speex);
}

UniqueSoundBuffer resample(SoundBufferView view, unsigned rate,
		unsigned channels) {
	UniqueSoundBuffer ret;
	ret.channelCount = channels;
	ret.rate = rate;
	ret.frameCount = std::ceil(view.frameCount * ((double)rate / view.rate));
	auto count = ret.frameCount * ret.channelCount;
	ret.data = std::make_unique<float[]>(count);
	resample(ret, view);
	return ret;
}


/// Streams audio from a vorbis file.
class StreamedVorbisAudio : public Audio {
public:
	StreamedVorbisAudio(nytl::StringParam file) {
		int error = 0;
		vorbis_ = stb_vorbis_open_filename(file.c_str(), &error, nullptr);
		if(!vorbis_) {
			auto msg = std::string("StreamVorbisAudio: failed to load ");
			msg += file.c_str();
			throw std::runtime_error(msg);
		}
	}

	~StreamedVorbisAudio() {
		if(vorbis_) {
			stb_vorbis_close(vorbis_);
		}
	}

	// TODO: cache data buffers, don't allocate every time
	void update(const AudioPlayer& ap) override {
		if(!play_) {
			return;
		}

		auto info = stb_vorbis_get_info(vorbis_);
		auto cap = buffer_.available_write();
		if(cap < 4096) { // not worth it, still full enough
			dlg_info("Skipping update");
			return;
		}

		std::unique_ptr<float[]> data = std::make_unique<float[]>(cap);
		auto ns = stb_vorbis_get_samples_float_interleaved(vorbis_,
			info.channels, data.get(), cap);
		if(ns == 0) {
			dlg_info("finished stream");
			play_ = false;
		}

		unsigned c = info.channels;
		if(c != ap.channels() || info.sample_rate != ap.rate()) {
			SoundBufferView view;
			view.channelCount = info.channels;
			view.frameCount = ns;
			view.rate = info.sample_rate;
			view.data = data.get();
			auto dst = resample(view, ap.rate(), ap.channels());
			buffer_.enqueue(dst.data.get(), dst.channelCount * dst.frameCount);
		} else {
			buffer_.enqueue(data.get(), ns * c);
		}

#ifndef NDEBUG
		// workaround for the threadid debug-check in our ring buffer.
		// update is called by the main thread first when the audio is
		// added (making this the producer) and (strictly) afterwards
		// only by the update thread. This is valid, the AudioPlayer
		// manages the switch of the producer switch but it defies
		// the debug check logic in the threadbuffer
		if(firstUpdate_) {
			firstUpdate_ = false;
			buffer_.reset_thread_ids();
		}
#endif
	}

	void render(const AudioPlayer& ap, float* buf, unsigned nf) override {
		if(!play_) {
			return;
		}

		if(drain_.load()) {
			// empty it
			buffer_.dequeue(nullptr, buffer_.available_read());
			drain_.store(false);
			return;
		}

		// there are multiple cases where this might not be the
		// case and it's alright. When draining (restarting)
		// or when the stream has finished.
		// dlg_assert(nf <= (unsigned) buffer_.available_read());

		unsigned count = buffer_.dequeue(rbuf_.get(), nf * ap.channels());
		for(auto i = 0u; i < count; ++i) {
			buf[i] += rbuf_[i];
		}
	}

	void toggle() {
		play_ = !play_;
	}

	void restart() {
		stb_vorbis_seek_start(vorbis_);
		drain_.store(true);
	}

protected:
	stb_vorbis* vorbis_ {};

	static constexpr auto bufSize = 48000 * 2;
	tkn::ring_buffer_base<float> buffer_{bufSize};
	std::unique_ptr<float[]> rbuf_ =
		std::make_unique<float[]>(bufSize);
	bool play_ {true};
	std::atomic<bool> drain_ {}; // when restarting

#ifndef NDEBUG
	bool firstUpdate_ {true};
#endif
};


/// Loads a SoundBuffer from a vorbis file.
UniqueSoundBuffer loadVorbis(nytl::StringParam file) {
	int error = 0;
	auto vorbis = stb_vorbis_open_filename(file.c_str(), &error, nullptr);
	if(!vorbis) {
		auto msg = std::string("loadVorbis: failed to load");
		msg += file.c_str();
		throw std::runtime_error(msg);
	}

	auto info = stb_vorbis_get_info(vorbis);

	UniqueSoundBuffer ret;
	ret.channelCount = info.channels;
	ret.rate = info.sample_rate;
	ret.frameCount = stb_vorbis_stream_length_in_samples(vorbis);

	std::size_t count = ret.channelCount * ret.frameCount;
	ret.data = std::make_unique<float[]>(count);

	auto ns = stb_vorbis_get_samples_float_interleaved(vorbis, info.channels,
		ret.data.get(), count);
	dlg_assert((std::size_t) ns == ret.frameCount);
	stb_vorbis_close(vorbis);
	return ret;
}

// Will perform no resampling or adaption to different number of channels.
class SoundBufferAudio : public Audio {
public:
	// Note that the underlaying SoundBuffer data must stay valid
	// until this Audio object is destroyed
	SoundBufferAudio(SoundBufferView buffer) : buffer_(buffer) {}

	void render(const AudioPlayer& ap, float* buf, unsigned nf) override {
		if(!play_) {
			return;
		}

		dlg_assert(buffer_.channelCount == ap.channels());
		dlg_assert(buffer_.rate == ap.rate());

		auto cc = buffer_.channelCount;
		for(auto i = 0u; i < nf; ++i, ++frame_) {
			for(auto c = 0u; c < cc; ++c) {
				buf[i * cc + c] += buffer_.data[frame_ * cc + c];
			}
		}
	}

	void restart() {
		frame_ = 0;
	}

	void toggle() {
		play_ = !play_;
	}

protected:
	SoundBufferView buffer_;
	std::size_t frame_ {};
	bool play_ {true};
};

int main() {
	UniqueSoundBuffer buf;
	SoundBufferAudio* sba = nullptr;
	StreamedVorbisAudio* streamAudio = nullptr;
	Audio* dummy = nullptr;

	tkn::acb::AudioPlayer ap;

	auto stream = true;
	auto samplePath = DOI_BASE_DIR "/assets/punch.ogg";
	if(stream) {
		auto a = std::make_unique<StreamedVorbisAudio>(samplePath);
		streamAudio = a.get();
		ap.add(std::move(a));
	} else {
		auto sample = loadVorbis(samplePath);
		dlg_assert(sample.data);
		if(ap.rate() != sample.rate || ap.channels() != sample.channelCount) {
			buf = resample(sample, ap.rate(), ap.channels());
		} else {
			buf = std::move(sample);
		}

		auto usba = std::make_unique<SoundBufferAudio>(buf);
		sba = usba.get();
		ap.add(std::move(usba));
	}

	auto run = true;
	while(run) {
		char c;
		int ret = std::fread(&c, 1, 1, stdin);
		if(ret != 1) {
			dlg_error("fread failed: {}", ret);
			break;
		}

		switch(c) {
			case 's':
				if(sba) {
					sba->restart();
				} else if(streamAudio) {
					streamAudio->restart();
				}
				break;
			case 'a':
				if(!dummy) {
					dummy = &ap.add(std::make_unique<DummyAudio>());
				}
				break;
			case 'r':
				if(dummy) {
					ap.remove(*dummy);
					dummy = nullptr;
				}
				break;
			case 'q':
				run = false;
				break;
			case 'p':
				if(sba) {
					sba->toggle();
				} else if(streamAudio) {
					streamAudio->toggle();
				}
				break;
			default:
				break;
		}
	}
}
