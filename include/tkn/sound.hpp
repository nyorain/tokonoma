#pragma once

#include <tkn/audio.hpp>
#include <tkn/ringbuffer.hpp>
#include <tkn/file.hpp>
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

// Loads a SoundBuffer from an audio file.
// Throws on error
UniqueSoundBuffer loadVorbis(nytl::StringParam file);
UniqueSoundBuffer loadMP3(nytl::StringParam file);

// AudioSource that simply outputs a static audio buffer.
// It does not own its audio buffer, only a view into it.
// Resample the audio buffer to fit your output needs before creating
// a SoundBufferAudio with it. The buffer must stay valid during
// the lifetime of this object.
class SoundBufferAudio : public AudioSource {
public:
	SoundBufferAudio(SoundBufferView view) : buffer_(view) {}
	void render(unsigned nb, float* buf, bool mix) override;

	auto& buffer() const { return buffer_; }

	// Set to 0.0 to pause the audio.
	void volume(float v) { volume_.store(v); }
	float volume() const { return volume_.load(); }
	bool playing() const { return volume_.load() != 0.f; }

private:
	SoundBufferView buffer_;
	std::size_t frame_ {0};
	std::atomic<float> volume_ {1.f};
};

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

	unsigned rate() const { return rate_; }
	unsigned channels() const { return 2; }

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

// Continous decoder classes, to be used with tkn::Streamed.
class VorbisDecoder {
public:
	explicit VorbisDecoder(nytl::StringParam file);
	explicit VorbisDecoder(nytl::Span<const std::byte> buf);
	explicit VorbisDecoder(File&& file);
	~VorbisDecoder();

	int get(float* buf, unsigned nf);
	unsigned rate() const;
	unsigned channels() const;

private:
	stb_vorbis* vorbis_ {};
};

class MP3Decoder {
public:
	explicit MP3Decoder(nytl::StringParam file);
	explicit MP3Decoder(File&& file);

	int get(float* buf, unsigned nf);
	unsigned rate() const { return rate_; }
	unsigned channels() const { return channels_; }

private:
	void init();

	// How much we read from the file before trying to decode a frame.
	// Follows the recommendation of minimp3
	static constexpr auto readSize = 16 * 1024;

	File file_;
	mp3dec_t mp3_ {};
	unsigned rate_ {};
	unsigned channels_ {};
	std::unique_ptr<std::uint8_t[]> rbuf_ {}; // readSize bytes; read from file_
	std::size_t rbufSize_ {}; // how many valid bytes in rbuf_

	// already extracted samples
	// Valid in range [samplesCount_, samplesCount_ + samplesOff_)
	std::vector<float> samples_ {};
	std::size_t samplesCount_ {};
	std::size_t samplesOff_ {};
};

} // namespace tkn
