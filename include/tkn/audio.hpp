#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

// cubeb fwd decls
typedef struct cubeb cubeb;
typedef struct cubeb_stream cubeb_stream;

// Terminology:
// - sample: a number representing the audio signal of one channel
//   at a point in time.
// - frame: the samples for all channels at a point in time.
//   E.g. for a stereo setup, a frame contains 2 samples.
// - block: a set of frames with a fixed size. Usually frames are
//   not rendered/processed one-by-one but rather in blocks of fixed
//   sizes, allowing for optimizied processing algorithms.

namespace tkn {

class Audio;
class AudioEffect;

/// Combines audio output.
class AudioPlayer {
public:
	// How many frames are packed together in a block.
	// Will always render/process audio blocks of this size.
	static constexpr auto blockSize = 1024u;

public:
	/// Throws std::runtime_error if something could not be initialized.
	AudioPlayer(const char* name = "tkn");
	~AudioPlayer();

	/// Adds the given Audio implementation to the list of audios
	/// to render. Must be called from the thread that created this player.
	Audio& add(std::unique_ptr<Audio>);

	template<typename AudioImpl, typename... Args>
	AudioImpl& create(Args&&... args) {
		auto impl = std::make_unique<AudioImpl>(std::forward<Args>(args)...);
		auto& ret = *impl;
		add(std::move(impl));
		return ret;
	}

	/// Removes the given Audio implementation object.
	/// Returns false and has no effect if the given audio
	/// is not known. Must be called from the thread that created this player.
	/// Note that removing the audio implementation will destroy it
	/// eventually (as soon as possible) but due to the multi threaded nature
	/// of the AudioPlayer, audio.update and audio.render might still be called
	/// after this.
	bool remove(Audio&);

	/// Changes the active effect. Will destroy the previous effect, if any.
	void effect(std::unique_ptr<AudioEffect> effect);

	/// Returns the number of channels.
	/// Guaranteed to stay constant and not change randomly.
	unsigned channels() const { return channels_; }

	/// Returns the frame rate (i.e. the number of frames per second)
	/// of the output stream in Hz, e.g.  44100 or 48000.
	/// Independent from the number of channels.
	/// Guaranteed to stay constant and not change randomly.
	unsigned rate() const { return rate_; };

	/// Must only be accessed by render thread, i.e. by Audios in
	/// their render function (or effects used by them).

protected:
	void updateThread();
	long dataCb(void* buffer, long nsamples);
	void stateCb(unsigned);

	void unlink(Audio& link, Audio* prev, std::atomic<Audio*>& head);

	struct Util; // static c callbacks

protected:
	// Active audios.
	// Iterated over by update and render thread.
	// Elements added and unlinked by main thread (add/destroy).
	// Owned list, we don't use unique_ptrs since it's atomic
	// and we do too much ownership moving anyways.
	std::atomic<Audio*> audios_ {};

	// List of Audios that were destroyed but needed to be kept
	// alive until this update and render iteration finish.
	// Access synchronized by dmutex_. Doesn't have to be lock-free
	// since this is never accessed by the audio thread.
	std::vector<std::unique_ptr<Audio>> destroyed_ {};
	std::mutex dmutex_ {};

	// Owned
	std::atomic<AudioEffect*> effect_ {};

	std::atomic<std::uint64_t> renderIteration_ {1};
	std::atomic<bool> run_;

	cubeb* cubeb_;
	cubeb_stream* stream_;
	unsigned rate_;
	unsigned channels_;
	std::thread updateThread_;

	// owned by render thread
	std::vector<float> renderBuf_;
	std::vector<float> renderBufTmp_;

	// in samples
	unsigned left_ {0};
	unsigned leftOff_ {0};

	// temporary buffers cache to minimize allocations while rendering
	// and not force every source to have it's own temporary buffer.
	// should only be accessed by render thread
	std::array<std::vector<float>, 2> buf1_;
};

/// Interface for playing a sound.
/// Could be implemented e.g. by a fixed audio buffer,
/// an audio stream or a synthesizer.
class Audio {
public:
	/// When an Audio implementation is destroyed in response to
	/// AudioPlayer::remove, destruction may be deferred and happen
	/// in a different thread.
	virtual ~Audio() = default;

	/// This function is called regularly from a thread in which
	/// more expensive operations are permitted by the AudioPlayer
	/// it is associated with. No guarantees about the thread are
	/// made, except that this function isn't called more than
	/// once at a time. It might be called simultaneously with `render`.
	/// It should prepare audio data to be written in render, if needed.
	/// Implementations can depend on this function being called
	/// a couple of times per second, i.e. an appropriate amount of
	/// data to prepare for rendering would be the samples for one second.
	/// In turn, implementations must not take extreme amounts
	/// of time (more than a couple of milliseconds is definitely too
	/// much).
	virtual void update(const AudioPlayer&) {};

	/// Renders its own sound into the described buffer. Must add itself to
	/// the existent values and not just overwrite the already present values.
	/// Note that this function is always called in a separate
	/// audio thread and must synchronize itself internally.
	/// It must not block at all and should therefore use mechanisms
	/// like (lock-free) ring buffers. The output buffer will always
	/// be interleaved with stereo format.
	/// - ap: The associated audio player from which the sampling
	///   rate and channel count can be recevied.
	/// - buf: Tightly packed interleaved stereo audio data.
	///   References at least 'ap.channels() * sizeof(float) * nf' bytes.
	/// - ns: The number of samples to write (per channel).
	virtual void render(const AudioPlayer& pl, float* buf, unsigned ns) = 0;

private:
	friend class AudioPlayer;

	/// Lock-free owned linked list used by AudioPlayer for update and render
	/// iterations. If Audio wasn't destroyed yet, next_ can be thought
	/// of as owned pointer.
	std::atomic<Audio*> next_ {};

	/// 0 when the audio was not destroyed.
	/// Otherwise the AudioPlayer::renderIteration_ in which it was destroyed.
	/// The AudioPlayer keeps it alive for this iteration.
	std::atomic<std::uint64_t> destroyed_ {};
};

/// Class for a general audio effect that modifies audio buffers.
class AudioEffect {
public:
	virtual ~AudioEffect() = default;

	/// Applies the effect.
	/// - rate: sampling rate in Hz
	/// - nc: number channels in input and output buffer
	/// - ns: number of samples per input and output buffer
	/// - in: input buffer to read
	/// - out: output buffer with undefined contents that should be overriden
	/// The buffers are interleaved. Input and output buffers must not
	/// overlap.
	virtual void apply(unsigned rate, unsigned nc, unsigned ns,
		const float* in, float* out) = 0;
};

} // namespace tkn
