#pragma once

#include <tkn/audio.hpp>
#include <tkn/ringbuffer.hpp>
#include <nytl/stringParam.hpp>
#include <tml.h>
#include <tsf.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.h>

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
// Expects dst to be able to hold enough frames.
// Will not read from `dst.data` and not write to `src.data`.
void resample(SoundBufferView dst, SoundBufferView src);

// - fr: frame rate in Hz
// - nc: number channels
UniqueSoundBuffer resample(SoundBufferView, unsigned fr, unsigned nc);

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
	StreamedVorbisAudio(nytl::StringParam file, unsigned rate,
		unsigned channels);
	~StreamedVorbisAudio();

	void update() override;
	void render(unsigned nb, float* buf, bool mix) override;

	// Set to 0.0 to pause the audio.
	void volume(float v) { volume_.store(v); }
	float volume() const { return volume_.load(); }
	bool playing() const { return volume_.load() != 0.f; }

protected:
	stb_vorbis* vorbis_ {};
	std::atomic<float> volume_ {1.f};
	std::unique_ptr<float[]> resampleBuf_ {};
	unsigned rate_ {};
	unsigned channels_ {};

	// TODO: should probably be dependent on rate and channels count
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

protected:
	SoundBufferView buffer_;
	std::size_t frame_ {};
	std::atomic<float> volume_ {1.f};
};

} // namespace tkn
