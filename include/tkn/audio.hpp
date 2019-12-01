#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <array>
#include <cassert>

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

// TODO: specify what happens with exceptions from AudioSource and
// AudioEffect. Just catch them in the audio player? destroy/disable
// sources/effects after they've thrown? or mark the interfaces noexcept?

namespace tkn {

class AudioSource;
class AudioEffect;

/// Buffer cache with exactly 2 non-owned dynamic-sized buffers.
/// Useful for ping-pong audio manipulation.
struct BufCache {
	std::array<std::vector<float>, 2>* bufs;

	BufCache(std::array<std::vector<float>, 2>& ref) : bufs(&ref) {}
	BufCache(const BufCache& rhs) : bufs(rhs.bufs) {}

	template<unsigned I = 0>
	std::vector<float>& get(std::size_t size) {
		static_assert(I < 2);
		auto& b = (*bufs)[I];
		if(b.size() < size) b.resize(size);
		return b;
	}

	std::vector<float>& get(unsigned i, std::size_t size) {
		assert(i < 2);
		auto& b = (*bufs)[i];
		if(b.size() < size) b.resize(size);
		return b;
	}
};

/// Holds two separate BufCache objects to be used by render and
/// update thread, respectively.
struct BufCaches {
	BufCache render;
	BufCache update;
};

struct OwnedBufCaches : public BufCaches {
	std::array<std::vector<float>, 2> orender;
	std::array<std::vector<float>, 2> oupdate;

	OwnedBufCaches() : BufCaches{orender, oupdate} {}
	OwnedBufCaches(OwnedBufCaches&&) = delete;
	OwnedBufCaches& operator=(OwnedBufCaches&&) = delete;
};

/// Combines audio output.
class AudioPlayer {
public:
	// How many frames are packed together in a block.
	// The player will always render/process audio blocks of this size.
	static constexpr auto blockSize = 1024u;

	// Building block for our lock-free linked list data structure.
	struct Audio {
		// The AudioSource implementation
		std::unique_ptr<AudioSource> source;

		// Lock-free owned linked list used by AudioPlayer for update and render
		// iterations. If AudioSource wasn't destroyed yet, next_ can be thought
		// of as owned pointer.
		std::atomic<Audio*> next {nullptr};

		// 0 when the audio was not destroyed.
		// Otherwise the AudioPlayer::renderIteration_ in which it was destroyed.
		// The AudioPlayer keeps it alive for this iteration.
		std::atomic<std::uint64_t> destroyed {0};
	};

public:
	/// Throws std::runtime_error if something could not be initialized.
	AudioPlayer(const char* name = "tkn", // stream name sent to backend
		unsigned rate = 0, // rate to use. 0 for default/preferred rate
		unsigned channels = 2, // num channels
		unsigned latencyBlocks = 1); // minimum render latency in blocks
	virtual ~AudioPlayer();

	/// Starts playback.
	/// Must only be called once from the main thread after the
	/// AudioPlayer was created to start it.
	void start();

	/// Adds the given Audio implementation to the list of audios
	/// to render. Must be called from the thread that created this player.
	AudioSource& add(std::unique_ptr<AudioSource>);

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
	bool remove(AudioSource&);

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

	BufCaches bufCaches() const { return bufCaches_; }

protected:
	void updateThread();
	void unlink(Audio& link, Audio* prev, std::atomic<Audio*>& head);
	long dataCb(void* buffer, long nframes);
	void stateCb(unsigned);

	// makes sure renderBuf_ contains at least nf frames
	// assumes that renderBuf_ is empty
	void fill(unsigned nf);

	virtual void renderUpdate() {}

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

	// how many frames are left in renderBuf_ (at leftOff_)
	unsigned left_ {0};
	unsigned leftOff_ {0};

	OwnedBufCaches bufCaches_;
};

class AudioSource {
public:
	virtual ~AudioSource() = default;

	/// Renders a given number of blocks of its sound into the provided buffer.
	/// The sample/frame rate, channel count and block size must have
	/// been previously configured via implementation-specific means.
	/// This is done so they can't be changed in every single call
	/// to render, i.e. allow audio sources to preconfigure complex
	/// computations based on the setup.
	/// Note that this function is always called in a separate
	/// audio thread and must synchronize itself internally.
	/// It must not block at all and should therefore use mechanisms
	/// like (lock-free) ring buffers.
	/// - nb: Number of blocks to render into `buf`.
	/// - buf: Buffer of interleaved samples
	/// - mix: Whether to mix or overwrite the audio in `buf`.
	///   When this is true, the AudioSource must just add itself
	///   to the values in the buffer, otherwise it must overwrite them.
	virtual void render(unsigned nb, float* buf, bool mix) = 0;

	/// This function is called regularly from a thread in which
	/// more expensive operations are permitted.
	/// No guarantees about the thread are made, except that this function
	/// isn't called by multiple threads at once.
	/// It might be called simultaneously with `render`.
	/// It should prepare audio data to be written in render, if needed.
	/// Implementations can depend on this function being called
	/// a couple of times per second, i.e. an appropriate amount of
	/// data to prepare for rendering would be the samples for one second.
	/// In turn, implementations must not take extreme amounts
	/// of time (more than a couple of milliseconds is definitely too
	/// much).
	virtual void update() {}
};

/// Class for a general audio effect that modifies audio buffers.
class AudioEffect {
public:
	virtual ~AudioEffect() = default;

	/// Applies the effect to the input and renders it into an output buffer.
	/// - rate: sampling rate in Hz
	/// - nc: number channels in input and output buffer
	/// - nf: number of frames per input and output buffer
	/// - in: input buffer to read
	/// - out: output buffer for writing.
	///   Initial contents are undefined, they must be overriden.
	/// The buffers are interleaved. Input and output buffers must not
	/// overlap.
	virtual void apply(unsigned rate, unsigned nc, unsigned nf,
		const float* in, float* out) = 0;
};

} // namespace tkn
