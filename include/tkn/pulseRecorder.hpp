#pragma once

#include <tkn/config.hpp>
#ifndef TKN_WITH_PULSE_SIMPLE
	#error "pulse simple required to use this header"
#endif

#include <tkn/ringbuffer.hpp>
#include <dlg/dlg.hpp>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <stdexcept>
#include <atomic>

namespace tkn {

// simple utility that gets audio from system
class PulseAudioRecorder {
public:
	static constexpr auto bufferSize = 1024 * 8;

	PulseAudioRecorder() = default;

	void init() {
		exit_.store(false);

		pa_channel_map map;
		std::memset(&map, 0, sizeof(map));
		pa_channel_map_init_stereo(&map);

		pa_sample_spec spec {};
		spec.format = PA_SAMPLE_FLOAT32LE;
		spec.rate = 44100;
		spec.channels = 2;

		pa_buffer_attr attr {};
		attr.maxlength = bufferSize;
		attr.fragsize = bufferSize;

		int perr;
		pa_ = pa_simple_new(NULL, "tkn-recorder", PA_STREAM_RECORD,
			NULL, "tkn-recorder", &spec, &map, &attr, &perr);
		if(!pa_) {
			dlg_error("pa_simple_new: {}", pa_strerror(perr));
			throw std::runtime_error("pa_simple_new failed");
		}

		thread_ = std::thread{[this]{ readerThread(); }};
	}

	~PulseAudioRecorder() {
		exit_.store(true);
		if(thread_.joinable()) {
			thread_.join();
		}
		if(pa_) {
			pa_simple_free(pa_);
		}
	}

	void readerThread() {
		std::vector<float> buf(bufferSize);
		int perr;

		while(!exit_.load()) {
			if(pa_simple_read(pa_, buf.data(), buf.size(), &perr) < 0) {
				dlg_error("pa_simple_read: {}", pa_strerror(perr));
				return;
			}

			recorded_.enqueue(buf.data(), buf.size());
		}
	}

	unsigned available() const {
		return recorded_.available_read();
	}

	unsigned deque(float* buf, unsigned ns) {
		return recorded_.deque(buf, ns);
	}

protected:
	static constexpr auto bufSize = 48000 * 2;
	pa_simple* pa_ {};
	std::atomic<bool> exit_;
	std::thread thread_;
	tkn::RingBuffer<float> recorded_ {bufSize};
};


} // namespace tkn
