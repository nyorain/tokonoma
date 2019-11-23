#pragma once

#include <chrono>
#include <memory>
#include <vector>
#include <shared_mutex>

// cubeb fwd decls
typedef struct cubeb cubeb;
typedef struct cubeb_stream cubeb_stream;

namespace tkn::acb {

using Clock = std::chrono::high_resolution_clock;
class Audio;

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

protected:
	void updateThread();
	long dataCb(void* buffer, long nframes);
	void stateCb(unsigned);

	struct CCB; // static c callbacks

protected:
	// TODO: nvm, just use vector!
	std::unique_ptr<Audio> audios_;
	Audio* updateCurrent_ {};
	Audio* renderCurrent_ {};
	std::unique_ptr<Audio> keepRender_;
	std::unique_ptr<Audio> keepUpdate_;

	std::shared_mutex mutex_;
	cubeb* cubeb_;
	cubeb_stream* stream_;
	unsigned rate_;
	unsigned state_;
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
	virtual void update() {};

	/// Renders its own sound into the described buffer. Must add itself to
	/// the existent values and not just overwrite the already present values.
	/// Note that this function is always called in a separate
	/// audio thread and must synchronize itself internally.
	/// It must not block at all and should therefore use mechanisms
	/// like (lock-free) ring buffers. The output buffer will always
	/// be interleaved with stereo format.
	/// \param buffers Tightly packed interleaved stereo audio data.
	///   References at least 2 * sizeof(float) * frames bytes.
	/// \param samples The number of frames to write.
	/// \param rate The sample rate in hz, e.g. 44100.
	virtual void render(float& buffer, unsigned frames,
		unsigned rate) = 0;

private:
	// Needed by AudioPlayer
	friend class AudioPlayer;
	std::unique_ptr<Audio> next {};
	Audio* prev {};
};

} // namespace tkn
