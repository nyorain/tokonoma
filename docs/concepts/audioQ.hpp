// // Size: AudioPlayer::blockSize
using AudioBlock = std::unique_ptr<float[]>;

AudioBlock createAudioBlock() {
	return std::make_unique<float[]>(tkn::AudioPlayer::blockSize);
}

class AudioBufferQueue {
public:
	static constexpr auto blockCount = 64;

public:
	AudioBufferQueue() {
		AudioBlock blocks[blockCount];
		for(auto& block : blocks) {
			block = createAudioBlock();
		}

		qback_.enqueue(blocks, blockCount);
	}

	// update thread
	bool get(AudioBlock& out) {
		return qback_.dequeue(&out, 1) == 1;
	}

	bool submit(AudioBlock&& in) {
		return queue_.enqueue(&in, 1) == 1;
	}

	// render thread
	bool read(AudioBlock& out) {
		return queue_.dequeue(&out, 1) == 1;
	}

	bool release(AudioBlock&& in) {
		return qback_.dequeue(&in, 1) == 1;
	}

private:
	/// Queue sending ownership from update to render thread.
	tkn::RingBuffer<AudioBlock> queue_ {blockCount};
	/// Queue sending ownership from render to update thread.
	tkn::RingBuffer<AudioBlock> qback_ {blockCount};
};

// class AudioBufferQueue {
// public:
// 	/// Reads the next queues audio block.
// 	/// Must only be called from the update thread.
// 	AudioBlock read();
//
// 	/// Submits the given audio block for rendering.
// 	/// Must only be called from the update thread.
// 	void submit(AudioBlock&&);
//
// 	/// Retrieves a new AudioBlock that can be filled.
// 	/// The buffer has undefined contents.
// 	/// Must only be called in the audio update thread.
// 	AudioBlock getUpdate();
//
// 	/// Retrieves a new AudioBlock that can be filled.
// 	/// The buffer has undefined contents.
// 	/// Must only be called in the audio render thread.
// 	AudioBlock getRender();
//
// 	/// Releases the audio block after it was read by the rendering
// 	/// thread. Must only be called from the rendering thread.
// 	void releaseRender(AudioBlock&& block);
//
// private:
// 	/// Rendered buffers that were submitted.
// 	tkn::RingBuffer<AudioBlock> queue_ {512};
//
// 	/// Buffers for which owneship is being transferred back
// 	/// to the update thread.
// 	tkn::RingBuffer<AudioBlock> qback_ {512};
//
// 	/// Unused buffers.
// 	std::vector<AudioBlock> freeUpdate_;
// 	std::vector<AudioBlock> freeRender_;
// };

// template<typename Source>
// class BufferedAudioSource : public tkn::AudioSource {
// public:
// 	static constexpr auto bufferSize = 64;
// 	static constexpr auto min
//
// public:
// 	template<typename... Args>
// 	BufferedAudioSource(Args&&... args) :
// 		source_(std::forward<Args>(args)...) {}
//
// 	void update() override {
// 		source_.update();
//
// 		auto cap = buffer_.available_write();
// 		cap -= (cap % tkn::AudioPlayer::blockSize);
// 		if(cap < 4096) { // not worth it, still full enough
// 			dlg_info("Skipping update");
// 			return;
// 		}
// 	}
//
// protected:
// 	Source source_;
// 	tkn::RingBuffer<AudioBlock> buffer_;
// };
