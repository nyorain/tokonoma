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
enum class AudioFormat;

/// Combines audio output.
/// Add (and remove) Audio implementations to output them.
/// Always uses 48 kHz sample rate.
class AudioPlayer {
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

	/// Returns the internal synchronization mutex.
	/// Can be locked before accessing resources that might
	/// be accessed from the audio thread.
	/// Make sure that you never lock it for short time (otherwise
	/// realtime audio might fail due to a buffer underflow) and
	/// that you don't have it locked while calling a function of this object.
	auto& mutex() { return mutex_; }

protected:
	static void cbWrite(struct SoundIoOutStream*, int, int);
	static void cbUnderflow(struct SoundIoOutStream*);
	static void cbError(struct SoundIoOutStream*, int);
	static void cbBackendDisconnect(struct SoundIo*, int);

	// init and finish to allow easy reinitialization on error
	void init();
	void finish();
	void audioLoop(); // audio thread main function

	void output(struct SoundIoOutStream*, int, int);

protected:
	AudioFormat format_;
	std::vector<std::unique_ptr<Audio>> audios_;
	std::mutex mutex_;
	std::atomic<bool> error_ {false};
	std::thread audioThread_;

	struct SoundIo* soundio_ {};
	struct SoundIoDevice* device_ {};
	struct SoundIoOutStream* stream_ {};
	struct SoundIoRingBuffer* buffer_ {};

	// TODO: should be related to software_latency (?) (and backend)
	// how much to internally buffer ahead (in seconds).
	// Should be at least 0.001 (1ms). Additional latency.
	float bufferTime_ {0.005};
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
	/// \param buffers Tightly packed interleaved stereo audio data.
	///   References at least 2 * sizeof(float) * samples bytes.
	/// \param samples The number of samples to write
	virtual void render(float& buffer, unsigned samples) = 0;
};

} // namespace doi
