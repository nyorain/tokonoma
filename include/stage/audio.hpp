#pragma once

#include <cstddef>
#include <mutex>
#include <vector>
#include <memory>
#include <atomic>
#include <nytl/span.hpp>
#include <nytl/callback.hpp>

// soundio fwd
struct SoundIo;
struct SoundIoDevice;
struct SoundIoOutStream;

// TODO: these interfaes leave problems open:
// - not possible to mix (volume) sounds relative to each other
// - not possible to add post-processing/effects to the audio player
// - latency? e.g. music would benefit from huge buffering while
//   short triggered sounds would need low latency
//   (we currently use low (0.05 secs) latency everywhere)
// - currently we lock a mutex while rendering. soundio explicitly
//   says that this is a bad idea

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

// TODO: use std::byte
/// Utility functions that converts a sample between the audio formats.
/// \param add Whether to just add the value instead of overwriting it.
void convert(void* from, AudioFormat fmtFrom, void* to,
	AudioFormat fmtTo, bool add = true);

/// Combines all audio output.
/// Add (and remove) Audio implementations to output them.
/// When an added Audio implementation throws in its render function,
/// the exception is caught (and printed).
/// Only ever uses a fixed sample rate of 48 khz.
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
	/// to render. Must be called from the mainthread (that created this
	/// player).
	Audio& add(std::unique_ptr<Audio>);

	/// Removes the given Audio implementation.
	/// Returns false and has no effect if the given audio
	/// is not known. Must be called from the mainthread (that created this
	/// player).
	bool remove(Audio&);

	/// Should be called from main thread (that created this player)
	/// Used to recover from errors. Might throw an exception
	/// from reinitialization.
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
	/// Make sure that you never lock it for too long (otherwise
	/// realtime audio might fail due to a buffer underflow) and
	/// that you don't have it locked at a call to add/remove.
	auto& mutex() { return mutex_; }

protected:
	static void cbWrite(struct SoundIoOutStream*, int, int);
	static void cbUnderflow(struct SoundIoOutStream*);
	static void cbError(struct SoundIoOutStream*, int);
	static void cbBackendDisconnect(struct SoundIo*, int); // TODO

	void init();
	void finish();

	void output(struct SoundIoOutStream*, int, int);
	void error(struct SoundIoOutStream*);

protected:
	AudioFormat format_;
	std::vector<std::unique_ptr<Audio>> audios_;
	std::mutex mutex_;
	std::atomic<bool> error_ {false};

	struct SoundIo* soundio_ {};
	struct SoundIoDevice* device_ {};
	struct SoundIoOutStream* stream_ {};

	// used not allocate vector data every time
	std::vector<ChannelBuffer> bufferCache_ {};
};

/// Interface for playing a sound.
/// Could be implemented e.g. by a fixed audio buffer,
/// an audio stream or a synthesizer.
class Audio {
public:
	virtual ~Audio() = default;

	/// Renders the own sound into the described buffer.
	/// Note that this function is always called in a separate
	/// audio thread and must synchronize itself internally.
	/// It might use the AudioPlayer.mutex() for this which is guaranteed
	/// to be locked when render is called.
	/// Expected to output with a sample rate of 48 khz.
	/// Should throw a std::exception on error.
	/// Should not block in any way for realtime performance.
	/// \param buffers The channel buffers to write.
	///   Each buffer represents one channel.
	///   Must mix itself into the values already present (i.e. add).
	/// \param format The format to write into the given buffers.
	///   Can use kyo::client::convert (see above) to convert samples.
	/// \param samples The number of samples to write
	virtual void render(nytl::Span<const ChannelBuffer> buffers,
		AudioFormat format, unsigned int samples) = 0;
};

} // namespace doi
