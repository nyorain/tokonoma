#pragma once

#include <tkn/audio.hpp>
#include <tkn/sound.hpp>
#include <tkn/ringbuffer.hpp>
#include <speex_resampler.h>
#include <cassert>

namespace tkn {

// Writes nf frames from 'rb' into 'buf'. If not enough samples
// are available, uses 0 for the remaining frames.
// When mix is true, will mix itself into 'buf', otherwise overwrite it.
// If dstChannels is larger than srcChannels, will perform upmixing.
// Will apply the given 'volume' and use 'tmpbuf' for temporary buffers.
void writeSamples(unsigned nf, float* buf, RingBuffer<float>& rb,
	bool mix, unsigned srcChannels, unsigned dstChannels,
	BufCache& tmpbuf, float volume);

/// Downmixing works in place.
/// Template only implemented for supported conversions.
template<unsigned CSrc, unsigned CDst>
void downmix(float* buf, unsigned nf);
void downmix(float* buf, unsigned nf, unsigned srcc, unsigned dstc);

template<unsigned CSrc, unsigned CDst>
void downmix(float* src, float* dst, unsigned nf);
void downmix(float* src, float* dst, unsigned nf, unsigned srcc, unsigned dstc);

/// Downmixing works by copying.
/// Template only implemented for supported conversions.
template<unsigned CSrc, unsigned CDst>
void upmix(float* src, float* dst, unsigned nf);
void upmix(float* src, float* dst, unsigned nf, unsigned srcc, unsigned dstc);

/// Returns how many samples or frames are needed when resampling an audio
/// buffer with a rate of `srcRate` and `src` number of samples or frames into
/// an audio buffer with `dstRate`.
/// Note that for continous resampling, sometimes one fewer frame is needed.
unsigned resampleCount(unsigned srcRate, unsigned dstRate, unsigned srcn);

/// Returns how many samples or frames an audio buffer with `srcRate` must have
/// to result in a resampled audio buffer with `dstRate` and `dstn` number of
/// samples or frames. Note that for continous resampling, resampling will
/// sometimes produce one fewer frame than `dstn` with this formula.
unsigned invResampleCount(unsigned srcRate, unsigned dstRate, unsigned dstn);

// Resamples (if needed) the data in `src` into the data in `dst`.
// Does not perform remixing, expects both views have the same channel count.
// Expects dst to be able to hold enough frames, i.e. it should hold
// ceil(src.frameCount * ((double) dst.rate / src.rate)) frames.
// Will not read from `dst.data` and not write to `src.data`.
// Will return the number of samples written to dst.
unsigned resample(SoundBufferView dst, SoundBufferView src);

// Like resample above, but this overload should be used when continously
// resampling smaller pieces of a stream. The speex resampler state must have
// created been with parameters matching the parameters in dst and src.
// Does not perform remixing, expects both views have the same channel count.
// Will not read from `dst.data` and not write to `src.data`.
// Will return the number of samples written to dst.
unsigned resample(SpeexResamplerState*, SoundBufferView dst, SoundBufferView src);

// - rate: destination frame rate in Hz
// - nc: destination number of channels
UniqueSoundBuffer resample(SoundBufferView, unsigned rate, unsigned nc);

// Wraps an object of type 'T' into a streamed AudioSource.
// Will resample the audio as needed.
// Expects T with the following public interface:
// - int get(float* buf, unsigned nf)
//   Reads at max 'nf' frames into buf.
//   Returns the number of frames written.
//   A negative return value signals error.
//   A return value of 0 signals that no more samples are available.
// - unsigned channels() const
//   Returns the number of channels. Must not change.
// - unsigned rate() const
//   Returns the sampling rate in Hz. Must not change.
template<typename T>
class Streamed : public AudioSource {
public:
	/// - bc: The buffer cache to use during rendering/updating
	/// - hz: The rate in hz this source should output. If it doesn't
	///   match the rate of the inner object, it will be resampled.
	/// - nc: The number of channels this source should output. If it doesn't
	///   match the number of channels the inner object outputs, the
	///   audio will be remixed.
	/// - args: The arguments to forward to the inner object.
	template<typename... Args>
	Streamed(BufCaches bc, unsigned hz, unsigned nc, Args&&... args) :
			inner_(std::forward<Args>(args)...), bufs_(bc),
			rate_(hz), channels_(nc) {
		if(inner_.rate() != rate_) {
			int err {};
			speex_ = speex_resampler_init(inner_.channels(),
				inner_.rate(), rate_, SPEEX_RESAMPLER_QUALITY_DESKTOP, &err);
			assert(speex_ && !err);
		}
	}

	template<typename... Args>
	Streamed(tkn::AudioPlayer& ap, Args&&... args) :
		Streamed(ap.bufCaches(), ap.rate(), ap.channels(),
			std::forward<Args>(args)...) {}

	~Streamed() {
		if(speex_) {
			speex_resampler_destroy(speex_);
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
			assert(speex_);
			// in this case we need less original samples since they
			// will be upsampled below. Taking all samples here would
			// result in more upsampled samples than there is capacity.
			// We choose exactly so many source samples that the upsampled
			// result will fill the remaining capacity
			ns = invResampleCount(srcRate, rate_, ns);
		}

		auto nf = ns / channels_; // floor
		auto srcChannels = inner_.channels();
		if(srcChannels > channels_) {
			ns = nf * srcChannels;
		}

		auto b0 = bufs_.update.get(ns).data();
		nf = inner_.get(b0, nf);
		if(nf <= 0) {
			volume_.store(0.f);
			return;
		}

		if(srcChannels > channels_) {
			downmix(b0, nf, srcChannels, channels_);
			srcChannels = channels_;
		}

		if(srcRate != rate_) {
			SoundBufferView src;
			src.channelCount = srcChannels;
			src.frameCount = nf;
			src.rate = srcRate;
			src.data = b0;

			SoundBufferView dst;
			dst.channelCount = srcChannels;
			dst.frameCount = resampleCount(src.rate, rate_, nf);
			dst.rate = rate_;
			dst.data = bufs_.update.get<1>(dst.frameCount * srcChannels).data();
			dst.frameCount = resample(speex_, dst, src);

			auto count = dst.channelCount * dst.frameCount;
			auto written = buffer_.enque(dst.data, count);
		} else {
			auto written = buffer_.enque(b0, nf * srcChannels);
		}
	}

	void render(unsigned nb, float* buf, bool mix) override {
		auto nf = AudioPlayer::blockSize * nb;
		auto v = volume_.load();
		auto srcc = std::min(inner_.channels(), channels_);
		writeSamples(nf, buf, buffer_, mix, srcc, channels_, bufs_.render, v);
	}

	auto& inner() { return inner_; }
	const auto& inner() const { return inner_; }

	unsigned rate() const { return rate_; }
	unsigned channels() const { return channels_; }

	// Set volume to 0.0 to pause the audio.
	void volume(float v) { volume_.store(v); }
	float volume() const { return volume_.load(); }
	bool playing() const { return volume_.load() != 0.f; }

private:
	T inner_;
	unsigned rate_ {};
	unsigned channels_ {};
	std::atomic<float> volume_ {1.f};
	BufCaches bufs_;
	SpeexResamplerState* speex_ {};

	static constexpr auto bufSize = 48000 * 2;
	static constexpr auto minWrite = 4096;
	tkn::RingBuffer<float> buffer_{bufSize};
};

using StreamedVorbisAudio = tkn::Streamed<tkn::VorbisDecoder>;
using StreamedMP3Audio = tkn::Streamed<tkn::MP3Decoder>;

} // namespace tkn
