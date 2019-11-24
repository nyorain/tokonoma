#pragma once

#include <chrono>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

// cubeb fwd decls
typedef struct cubeb cubeb;
typedef struct cubeb_stream cubeb_stream;

namespace tkn {

using Clock = std::chrono::high_resolution_clock;
class Audio;

/// Combines audio output.
class AudioPlayer {
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

	/// Returns the number of channels.
	unsigned channels() const { return channels_; }

	/// Returns the sampling rate of the output stream in Hz, e.g.
	/// 44100 or 48000.
	unsigned rate() const { return rate_; };

protected:
	void updateThread();
	long dataCb(void* buffer, long nframes);
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

	std::atomic<std::uint64_t> renderIteration_ {1};
	std::atomic<bool> run_;

	cubeb* cubeb_;
	cubeb_stream* stream_;
	unsigned rate_;
	unsigned channels_;
	std::thread updateThread_;
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
	/// data to prepare for rendering would be the frames for one second.
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
	/// - nf: The number of frames to write.
	virtual void render(const AudioPlayer& pl, float* buf, unsigned nf) = 0;

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

} // namespace tkn
