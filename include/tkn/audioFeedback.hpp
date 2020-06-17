#pragma once

#include <tkn/audio.hpp>
#include <tkn/ringbuffer.hpp>

namespace tkn {

// Allows to read back the audio rendered by a AudioSource implementation
// in another thread than the render thread. Useful for appplications
// like visualizers.
// TODO: would be cleaner to do all feedback stuff in update if possible
// (e.g. for streamed sources)
template<typename T>
class FeedbackAudioSource : public tkn::AudioSource {
public:
	template<typename... Args>
	FeedbackAudioSource(Args&&... args) : impl_(std::forward<Args>(args)...) {}

	void update() override {
		impl_.update();
	}

	void render(unsigned nb, float* buf, bool mix) override {
		auto ns = 2 * nb * tkn::AudioPlayer::blockSize; // TODO: don't assume stereo
		float* feedback;
		if(!mix) {
			feedback = buf;
			impl_.render(nb, buf, mix);
		} else {
			tmpBuf_.resize(ns);
			feedback = tmpBuf_.data();
			impl_.render(nb, feedback, false);
			for(auto i = 0u; i < ns; ++i) {
				buf[i] += feedback[i];
			}
		}

		feedback_.enque(feedback, ns);
	}

	unsigned available() const {
		return feedback_.available_read();
	}

	unsigned dequeFeedback(float* buf, unsigned ns) {
		return feedback_.deque(buf, ns);
	}

	auto& inner() { return impl_; }
	auto& inner() const { return impl_; }

protected:
	T impl_;
	std::vector<float> tmpBuf_; // TODO: use buf cache

	static constexpr auto bufSize = 48000 * 2 * 10;
	tkn::RingBuffer<float> feedback_{bufSize};
};

} // namespace tkn
