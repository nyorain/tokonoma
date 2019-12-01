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

// Continous decoder classes, to be used with tkn::StreamedResampler.
class VorbisDecoder {
public:
	VorbisDecoder(nytl::StringParam file);
	VorbisDecoder(nytl::Span<const std::byte> buf);
	~VorbisDecoder();

	int get(float* buf, unsigned nf);
	unsigned rate() const;
	unsigned channels() const;

private:
	stb_vorbis* vorbis_ {};
};

class MP3Decoder {
public:
	MP3Decoder(nytl::StringParam file);
	~MP3Decoder();

	int get(float* buf, unsigned nf);
	unsigned rate() const { return rate_; }
	unsigned channels() const { return channels_; }

private:
	// How much we read from the file before trying to decode a frame.
	// Follows the recommendation of minimp3
	static constexpr auto readSize = 16 * 1024;

	std::FILE* file_ {};
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

} // namespace tkn
