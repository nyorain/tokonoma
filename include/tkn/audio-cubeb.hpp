#pragma once

#include <chrono>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>

// cubeb fwd decls
typedef struct cubeb cubeb;
typedef struct cubeb_stream cubeb_stream;

namespace tkn::acb {

using Clock = std::chrono::high_resolution_clock;
class Audio;

template<class T>
class atomic_unique_ptr {
public:
	constexpr atomic_unique_ptr() noexcept : ptr() {}
	explicit atomic_unique_ptr(T* p) noexcept : ptr(p) {}
	atomic_unique_ptr(atomic_unique_ptr&& p) noexcept : ptr(p.release()) {}
	atomic_unique_ptr& operator=(atomic_unique_ptr&& p) noexcept {
		reset(p.release());
		return *this;
	}

	void reset(T* p = nullptr) {
		auto old = ptr.exchange(p);
		if (old) delete old;
	}
	operator T*() const { return ptr.load(); }
	T* operator->() const { return ptr.load(); }
	T* get() const { return ptr.load(); }
	explicit operator bool() const { return ptr.load() != nullptr; }
	T* release() { return ptr.exchange(nullptr); }
	T* release_exchange(T* n) { return ptr.exchange(n); }
	~atomic_unique_ptr() { reset(); }

private:
	std::atomic<T*> ptr;
};

/// Combines audio output.
class AudioPlayer {
public:
	/// Throws std::runtime_error if something could not be initialized.
	AudioPlayer();
	~AudioPlayer();

	/// Adds the given Audio implementation to the list of audios
	/// to render. Must be called from the thread that created this player.
	Audio& add(std::unique_ptr<Audio>);

	/// Removes the given Audio implementation object.
	/// Returns false and has no effect if the given audio
	/// is not known. Must be called from the thread that created this player.
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

	std::unique_ptr<Audio> unlink(Audio& link, Audio* prev,
		atomic_unique_ptr<Audio>& head);

	struct Util; // static c callbacks

protected:
	// std::vector<std::unique_ptr<Audio>> audios_;
	// std::vector<std::unique_ptr<Audio>> destroyed_;

	// Active audios.
	// Iterated over by update and render thread.
	// Elements added and unlinked by main thread (add/destroy).
	atomic_unique_ptr<Audio> list_ {};

	// List of Audios that were destroyed but needed to be kept
	// alive until this update and render iteration finish.
	// Iterated over and elements removed by update thread.
	// Elements added by main thread.
	// NOTE: We could probably implement this as vector and
	// mutex as well, lock-free isn't really important here.
	atomic_unique_ptr<Audio> destroyed_ {};

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
	/// \param pl The associated audio player from which the sampling
	///   rate and channel count can be recevied.
	/// \param buffers Tightly packed interleaved stereo audio data.
	///   References at least 2 * sizeof(float) * frames bytes.
	/// \param numFrames The number of frames to write.
	virtual void render(const AudioPlayer& pl, float* buffer,
		unsigned numFrames) = 0;

protected:
	friend class AudioPlayer;
	atomic_unique_ptr<Audio> next_ {};
	std::atomic<std::uint64_t> destroyed_ {};
};

} // namespace tkn
