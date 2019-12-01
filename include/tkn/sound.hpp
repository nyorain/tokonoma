#pragma once

#include <tkn/audio.hpp>
#include <tkn/ringbuffer.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/span.hpp>
#include <tml.h>
#include <tsf.h>
#include <minimp3.h>
#include <cmath>
#include <speex_resampler.h>

typedef struct SpeexResamplerState_ SpeexResamplerState;
typedef struct stb_vorbis stb_vorbis;
typedef struct mp3dec_t mp3dec_t;

namespace tkn {

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

// Resamples (if needed) the data in `src` into the data in `dst`.
// Expects dst to be able to hold enough frames, i.e. it should hold
// ceil(src.frameCount * ((double) dst.rate / src.rate)) frames.
// Will not read from `dst.data` and not write to `src.data`.
// Will return the number of samples written to dst.
unsigned resample(SoundBufferView dst, SoundBufferView src);

// This overload should be used when continously resampling
// smaller pieces of a stream. The speex resampler state must have
// created been with parameters matching the parameters in dst and src.
unsigned resample(SpeexResamplerState*, SoundBufferView dst, SoundBufferView src);

// - fr: frame rate in Hz
// - nc: number channels
UniqueSoundBuffer resample(SoundBufferView, unsigned fr, unsigned nc);

// Loads a SoundBuffer from an audio file.
// Throws on error
UniqueSoundBuffer loadVorbis(nytl::StringParam file);
UniqueSoundBuffer loadMP3(nytl::StringParam file);

/// Generates audio from a midi file and a soundfont.
class MidiAudio : public AudioSource {
public:
	/// Moves ownership of the given tsf object here (since that
	/// is used to generate audio). Will always use interleaved
	/// stereo output.
	MidiAudio(tsf* tsf, nytl::StringParam midiPath, unsigned rate);
	MidiAudio(nytl::StringParam tsfPath,
		nytl::StringParam midiPath, unsigned rate);
	~MidiAudio();

	// Set to 0.0 to pause the audio.
	void volume(float v) { volume_.store(v); }
	float volume() const { return volume_.load(); }
	bool playing() const { return volume_.load() != 0.f; }

protected:
	void handle(const tml_message&);
	void render(unsigned nb, float* buf, bool) override;

protected:
	tsf* tsf_;
	const tml_message* messages_;
	const tml_message* current_ {};
	double time_ {}; // relative, in milliseconds
	unsigned rate_ {};
	std::atomic<float> volume_ {1.f};
};

/// Streams audio from a vorbis file.
class StreamedVorbisAudio : public AudioSource {
public:
	// TODO
	BufCache bufCacheR;
	BufCache bufCacheU;

public:
	/// Opens a vorbis file for streaming.
	/// - rate: frame rate in khz for output
	/// - nc: number of channels for output
	StreamedVorbisAudio(nytl::StringParam file, unsigned rate, unsigned nc);

	/// Opens an in-memory buffer for streaming.
	/// The buffer must remaing valid for the lifetime of this object.
	/// - rate: frame rate in khz for output
	/// - nc: number of channels for output
	StreamedVorbisAudio(nytl::Span<const std::byte>, unsigned rate, unsigned nc);
	~StreamedVorbisAudio();

	void update() override;
	void render(unsigned nb, float* buf, bool mix) override;

	// Set to 0.0 to pause the audio.
	void volume(float v) { volume_.store(v); }
	float volume() const { return volume_.load(); }
	bool playing() const { return volume_.load() != 0.f; }

protected:
	stb_vorbis* vorbis_ {};
	SpeexResamplerState* speex_ {};

	std::atomic<float> volume_ {1.f};
	std::unique_ptr<float[]> resampleBuf_ {};
	unsigned rate_ {};
	unsigned channels_ {};

	// TODO: should probably be dependent on rate and channels count
	static constexpr auto bufSize = 48000 * 2;
	static constexpr auto minWrite = 4096;
	tkn::RingBuffer<float> buffer_{bufSize};
};

class StreamedMP3Audio : public AudioSource {
	// TODO
	BufCache bufCacheR;
	BufCache bufCacheU;

public:
	/// Opens a mp3 file for streaming.
	StreamedMP3Audio(nytl::StringParam file);
	~StreamedMP3Audio();

	void update() override;
	void render(unsigned nb, float* buf, bool mix) override;

	unsigned rate() const { return rate_; }
	unsigned channels() const { return channels_; }

	// Set to 0.0 to pause the audio.
	void volume(float v) { volume_.store(v); }
	float volume() const { return volume_.load(); }
	bool playing() const { return volume_.load() != 0.f; }

protected:
	std::FILE* file_ {};
	mp3dec_t mp3_ {};

	std::vector<std::uint8_t> rbuf_ {};
	size_t rbufSize_ {};

	std::atomic<float> volume_ {1.f};
	unsigned rate_ {};
	unsigned channels_ {};

	// TODO: should probably be dependent on rate and channels count
	static constexpr auto bufSize = 48000 * 2;
	static constexpr auto minWrite = 4096;
	tkn::RingBuffer<float> buffer_{bufSize};
};

template<unsigned CSrc, unsigned CDst> struct Downmix;
template<unsigned CSrc> struct Downmix<CSrc, 1> {
	static void apply(float* buf) {
		float sum = 0.0;
		for(unsigned i = 0u; i < CSrc; ++i) {
			sum += buf[i] / CSrc;
		}
		buf[0] = sum;
	}
};

// http://avid.force.com/pkb/KB_Render_FAQ?id=kA031000000P4di&lang=en_US
// we are using movie order

// https://trac.ffmpeg.org/wiki/AudioChannelManipulation#a5.1stereo
template<> struct Downmix<6, 2> {
	// film ordering
	// https://superuser.com/questions/852400
	static constexpr float lfe = 0.0; // discard
	static constexpr std::array<std::array<float, 2>, 6> facs = {{
		{1.f, 0.0f}, // L
		{0.0f, 1.f}, // R
		{0.707f, 0.707f}, // C
		{0.707f, 0.f}, // Ls
		{0.f, 0.707f}, // Rs
		{lfe, lfe}, // LFE
	}};

	static void apply(float* buf) {
		std::array<float, 2> res {0.f, 0.f};
		for(auto i = 0u; i < facs.size(); ++i) {
			res[0] += facs[i][0] * buf[i];
			res[1] += facs[i][1] * buf[i];
		}

		buf[0] = res[0];
		buf[1] = res[1];
	}
};

template<> struct Downmix<8, 2> {
	static constexpr float lfe = 0.0; // discard
	static constexpr float fl = 0.707f;

	static constexpr std::array<std::array<float, 2>, 8> facs = {{
		{1.f, 0.0f}, // L
		{0.0f, 1.f}, // R
		{0.707f, 0.707f}, // C
		{0.707f, 0.f}, // Lss
		{0.f, 0.707f}, // Rss
		{0.707f, 0.f}, // Lsr
		{0.f, 0.707f}, // Rsr
		{lfe, lfe}, // LFE
	}};

	static void apply(float* buf) {
		float l = buf[0] + fl * buf[2] + fl * buf[3] + fl * buf[5] + lfe * buf[7];
		float r = buf[1] + fl * buf[2] + fl * buf[4] + fl * buf[6] + lfe * buf[7];
		buf[0] = l;
		buf[1] = r;

		std::array<float, 2> res {0.f, 0.f};
		for(auto i = 0u; i < facs.size(); ++i) {
			res[0] += facs[i][0] * buf[i];
			res[1] += facs[i][1] * buf[i];
		}

		buf[0] = res[0];
		buf[1] = res[1];
	}
};

template<> struct Downmix<8, 5> {
	static void apply(float* buf) {
		// first 3 channels are the same
		buf[3] += buf[5]; // add l rear to l side
		buf[4] += buf[6]; // add r rear to r side
		buf[5] = buf[7]; // move lfe to channel 5
	}
};

// Only works if srcc >= dstc.
void downmix(float* buf, unsigned nf, unsigned srcc, unsigned dstc) {
}

void upmix(float* src, float* dst, unsigned nf, unsigned srcc, unsigned dstc) {
}


// Writes nf frames from 'rb' into 'buf'. If not enough samples
// are available, uses 0 for the remaining frames.
// When mix is true, will mix itself into 'buf', otherwise overwrite it.
// If dstChannels is larger than srcChannels, will perform upmixing.
// Will apply the given 'volume' and use 'tmpbuf' for temporary buffers.
void writeSamples(unsigned nf, float* buf, RingBuffer<float>& rb,
		bool mix, unsigned srcChannels, unsigned dstChannels,
		Buffers::Buf& tmpbuf, float volume) {
	auto dns = dstChannels * nf;
	if(volume <= 0.f) {
		if(!mix) {
			std::memset(buf, 0x0, dns * sizeof(float));
		}

		return;
	}

	if(mix) {
		auto sns = nf * srcChannels;
		auto b0 = tmpbuf.get(sns).data();
		auto src = b0;
		auto count = rb.dequeue(b0, sns);

		if(srcChannels < dstChannels) {
			count /= srcChannels;
			auto b1 = tmpbuf.get<1>(count * dstChannels).data();
			upmix(b0, b1, count, srcChannels, dstChannels);
			src = b1;
			count *= dstChannels;
		}

		for(auto i = 0u; i < count; ++i) {
			buf[i] += volume * src[i];
		}
	} else if(volume != 1.f || srcChannels > dstChannels) {
		auto sns = nf * srcChannels;
		auto b0 = tmpbuf.get(sns).data();
		auto src = b0;
		auto count = rb.dequeue(b0, sns);

		if(srcChannels < dstChannels) {
			count /= srcChannels;
			auto b1 = tmpbuf.get<1>(count * dstChannels).data();
			upmix(b0, b1, count, srcChannels, dstChannels);
			src = b1;
			count *= dstChannels;
		}

		if(volume == 1.f) {
			std::memcpy(buf, src, count * sizeof(float));
		} else {
			for(auto i =0u; i < count; ++i) {
				buf[i] = volume * src[i];
			}
		}

		std::memset(buf + count, 0x0, (dns - count) * sizeof(float));
	} else {
		auto count = rb.dequeue(buf, dns);
		std::memset(buf + count, 0x0, (dns - count) * sizeof(float));
	}
}

// Expects T with the following public interface:
// - int get(float* buf, unsigned ns)
// - unsigned channels() const
// - unsigned rate() const
template<typename T>
class StreamedResampler : public AudioSource {
public:
	template<typename... Args>
	StreamedResampler(Buffers& bufs, unsigned rate, unsigned channels, Args&&... args) :
			inner_(std::forward<Args>(args)...), bufs_(bufs),
			rate_(rate), channels_(channels) {
		if(inner_.rate() != rate_) {
			int err {};
			speex_ = speex_resampler_init(inner_.channels(),
				inner_.rate(), rate_, SPEEX_RESAMPLER_QUALITY_DESKTOP, &err);
			dlg_assert(speex_ && !err);
		}
	}

	void update() override {
		auto cap = buffer_.available_write();
		if(cap < minWrite) { // not worth it, still full enough
			return;
		}

		auto ns = cap;
		auto srcRate = inner_.rate();
		if(srcRate != rate_) {
			dlg_assert(speex_);
			// in this case we need less original samples since they
			// will be upsampled below. Taking all samples here would
			// result in more upsampled samples than there is capacity.
			// We choose exactly so many source samples that the upsampled
			// result will fill the remaining capacity
			ns = std::floor(ns * ((double) srcRate / rate_));
		}

		auto b0 = bufs_.update.get(ns).data();
		auto nf = inner_.get(b0, ns);
		if(nf <= 0) {
			inner_.restart();
			volume_.store(0.f);
			return;
		}

		auto srcChannels = inner_.channels();
		if(srcRate != rate_) {
			SoundBufferView src;
			src.channelCount = srcChannels;
			src.frameCount = nf;
			src.rate = srcRate;
			src.data = b0;

			SoundBufferView dst;
			dst.channelCount = channels_;
			dst.frameCount = std::ceil(nf * ((double)rate_ / src.rate));
			dst.rate = rate_;
			dst.data = bufs_.update.get<1>(dst.frameCount * channels_).data();
			dst.frameCount = resample(speex_, dst, src);

			auto count = dst.channelCount * dst.frameCount;
			auto written = buffer_.enque(dst.data, count);
		} else {
			if(srcChannels > channels_) {
				downmix(b0, nf, srcChannels, channels_);
			}

			auto written = buffer_.enque(b0, nf * channels_);
		}
	}

	void render(unsigned nb, float* buf, bool mix) override {
		auto nf = AudioPlayer::blockSize * nb;
		auto ns = nf * channels_;
		auto v = volume_.load();
		if(v <= 0.f) {
			if(!mix) {
				std::memset(buf, 0x0, ns * sizeof(float));
			}

			return;
		}

		auto srcChannels = inner_.channels();
		if(mix) {
			auto sns = nf * srcChannels;
			auto b0 = bufs_.render.get(sns).data();
			auto src = b0;
			auto count = buffer_.dequeue(b0, sns);

			if(srcChannels < channels_) {
				count /= srcChannels;
				auto b1 = bufs_.render.get<1>(count * channels_).data();
				upmix(b0, b1, count, srcChannels, channels_);
				src = b1;
				count *= channels_;
			}

			for(auto i = 0u; i < count; ++i) {
				buf[i] += v * src[i];
			}
		} else if(v != 1.f || srcChannels > channels_) {
			auto sns = nf * srcChannels;
			auto b0 = bufs_.render.get(sns).data();
			auto src = b0;
			auto count = buffer_.dequeue(b0, sns);

			if(srcChannels < channels_) {
				count /= srcChannels;
				auto b1 = bufs_.render.get<1>(count * channels_).data();
				upmix(b0, b1, count, srcChannels, channels_);
				src = b1;
				count *= channels_;
			}

			if(v == 1.f) {
				std::memcpy(buf, src, count * sizeof(float));
			} else {
				for(auto i =0u; i < count; ++i) {
					buf[i] = v * src[i];
				}
			}

			std::memset(buf + count, 0x0, (ns - count) * sizeof(float));
		} else {
			auto count = buffer_.dequeue(buf, ns);
			std::memset(buf + count, 0x0, (ns - count) * sizeof(float));
		}
	}

	// Set to 0.0 to pause the audio.
	void volume(float v) { volume_.store(v); }
	float volume() const { return volume_.load(); }
	bool playing() const { return volume_.load() != 0.f; }

private:
	T inner_;
	unsigned rate_ {};
	unsigned channels_ {};
	std::atomic<float> volume_ {1.f};
	Buffers bufs_;
	SpeexResamplerState* speex_ {};

	static constexpr auto bufSize = 48000 * 2;
	static constexpr auto minWrite = 4096;
	tkn::RingBuffer<float> buffer_{bufSize};
};

// AudioSource that simply outputs a static audio buffer.
// It does not own its audio buffer, only a view into it.
// Resample the audio buffer to fit your output needs before creating
// a SoundBufferAudio with it. The buffer must stay valid during
// the lifetime of this object.
class SoundBufferAudio : public AudioSource {
public:
	SoundBufferAudio(SoundBufferView view) : buffer_(view) {}
	void render(unsigned nb, float* buf, bool mix) override;

	// Set to 0.0 to pause the audio.
	void volume(float v) { volume_.store(v); }
	float volume() const { return volume_.load(); }
	bool playing() const { return volume_.load() != 0.f; }

private:
	SoundBufferView buffer_;
	std::size_t frame_ {0};
	std::atomic<float> volume_ {1.f};
};

} // namespace tkn
