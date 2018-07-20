#pragma once

#include <cstddef>
#include <mutex>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>

#include <nytl/span.hpp>
#include <nytl/callback.hpp>

// soundio fwd
struct SoundIo;
struct SoundIoDevice;
struct SoundIoOutStream;
struct SoundIoRingBuffer;

namespace doi {

class Audio;

/// Mirror of SoundIoChannelArea, represents
/// the raw data of one audio channel.
struct ChannelBuffer {
	std::byte* ptr; // beginning of the raw buffer
	unsigned int step; // size in bytes between samples
};

/// The raw audio format of a ChannelBuffer.
/// Audio implementations should support all of them.
/// Formats always imply native endian representation.
/// AudioPlayer will generally prefer the formats in the
/// following order: f32 > f64 > s16 > s32
enum class AudioFormat {
	s16, // 16-bit signed int
	s32, // 32-bit signed int
	f32, // 32-bit float
	f64 // 64-bit float (double)
};

/// Utility functions that converts a sample between the audio formats.
/// \param from The buffer which holds 'count' audio samples in format 'fmtFrom'
/// \param to The out buffer which holds space for 'count' audio samples
///        in format 'fmtTo'
/// \param count The number of audio samples to convert
/// \param add Whether to add the values instead of overwriting them.
void convert(const std::byte* from, AudioFormat fmtFrom, std::byte* to,
	AudioFormat fmtTo, std::size_t count, bool add = true);

// TODO: functionality to clear all buffered data. Automatically on add/remove?
//  we can only clear data in own buffer though since soundio clear is broken
//  (even when supported - we can't know back to which frame was cleared)

/// Combines audio output.
/// Add (and remove) Audio implementations to output them.
/// Always uses 48 kHz sample rate.
class AudioPlayer {
public:
	/// Called after the audio player was reinitialized.
	/// Could e.g. be used to reload optimal formatted sound buffers if
	/// the used format has changed.
	nytl::Callback<void(AudioPlayer&, bool formatChanged)> onReinit;

public:
	/// Throws std::runtime_error if something could not be initialized.
	/// Note that this might happen with unsupported hardware so
	/// better catch it.
	AudioPlayer();
	~AudioPlayer();

	/// Adds the given Audio implementation to the list of audios
	/// to render. Must be called from the thread that created this player.
	Audio& add(std::unique_ptr<Audio>);

	/// Removes the given Audio implementation object.
	/// Returns false and has no effect if the given audio
	/// is not known. Must be called from the thread that created this player.
	bool remove(Audio&);

	/// Should be called from main thread (that created this player)
	/// Used to recover from errors. Might throw an exception
	/// from reinitialization. Must be called from the thread that created
	/// this player.
	void update();

	/// Returns the used audio format. Could be useful to e.g.
	/// store sound buffers already in the needed format.
	/// Audio.render implementations are only ever called with this format.
	/// This format might change when the AudioPlayer is reinitialized (
	/// due to error or backend changes).
	AudioFormat format() const { return format_; }

	/// Returns the internal synchronization mutex.
	/// Can be locked before accessing resources that might
	/// be accessed from the audio thread.
	/// Make sure that you never lock it for short time (otherwise
	/// realtime audio might fail due to a buffer underflow) and
	/// that you don't have it locked at a call to add/remove.
	auto& mutex() { return mutex_; }

protected:
	static void cbWrite(struct SoundIoOutStream*, int, int);
	static void cbUnderflow(struct SoundIoOutStream*);
	static void cbError(struct SoundIoOutStream*, int);
	static void cbBackendDisconnect(struct SoundIo*, int); // TODO

	// init and finish to allow easy reinitialization on error
	void init();
	void finish();
	void audioLoop(); // audio thread main function

	void output(struct SoundIoOutStream*, int, int);
	void error(struct SoundIoOutStream*);

protected:
	AudioFormat format_;
	std::vector<std::unique_ptr<Audio>> audios_;
	std::mutex mutex_;
	std::atomic<bool> error_ {false};
	std::thread audioThread_;

	struct SoundIo* soundio_ {};
	struct SoundIoDevice* device_ {};
	struct SoundIoOutStream* stream_ {};
	struct SoundIoRingBuffer* bufferLeft_ {};
	struct SoundIoRingBuffer* bufferRight_ {};

	// the preferred latency to use when rendering
	float prefLatency_ {};

	// used not allocate vector data every time
	std::vector<ChannelBuffer> bufferCache_ {};
};

/// Interface for playing a sound.
/// Could be implemented e.g. by a fixed audio buffer,
/// an audio stream or a synthesizer.
/// Although not in the interface, implementations should (if possible)
/// provide a volume multiplication factor.
class Audio {
public:
	virtual ~Audio() = default;

	/// Renders its own sound into the described buffer. Must add itself to
	/// the existent values and not just overwrite the already present values.
	/// Note that this function is always called in a separate
	/// audio thread and must synchronize itself internally.
	/// It might use the AudioPlayer.mutex() for this which is guaranteed
	/// to be locked when render is called.
	/// Expected to output with a sample rate of 48 khz.
	/// \param buffers The channel buffers to write.
	///   Each buffer represents one channel.
	///   Must mix itself into the values already present (i.e. add).
	/// \param format The format to write into the given buffers.
	///   Can use kyo::client::convert (see above) to convert samples.
	/// \param samples The number of samples to write
	virtual void render(nytl::Span<const ChannelBuffer> buffers,
		AudioFormat format, unsigned int samples) = 0;
};

/// Interface that can be used to manipulate mixed sound.
class AudioEffect {
public:
	virtual ~AudioEffect() = default;

	/// Called after all audios have been rendered into the given
	/// buffers, can freely manipulate their content.
	virtual void process(nytl::Span<const ChannelBuffer> buffers,
		AudioFormat format, unsigned int samples);
};

} // namespace doi
