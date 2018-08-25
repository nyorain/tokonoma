#define DLG_DEFAULT_TAGS "audio",
#include <stage/audio.hpp>
#include <soundio/soundio.h>
#include <dlg/dlg.hpp>

#include <chrono>
#include <stdexcept>

// NOTES: we assume:
//  - 48 kHz sampling is available
//  - stereo audio output is available

// TODO(complicated/may be not possible): function to clear local buffer?
// using soundio_ring_buffer_clear. But then we might have nothing to
// render in the output function.
// But would be nice e.g. when an Audio impl is added/removed
// NOTE: pretty sure using the soundio_ring_buffer_clear will always
// have the potential for a data race (ub) or advance beyond filled.
// maybe modify soundio/pull request upstream? e.g. the advance_read_ptr
// function could return bool or sth. Would require more atomic magics.

namespace doi {

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

/// Utility functions that converts a sample between the audio formats.
/// \param from The buffer which holds 'count' audio samples in format 'fmtFrom'
/// \param to The out buffer which holds space for 'count' audio samples
///        in format 'fmtTo'
/// \param count The number of audio samples to convert
/// \param add Whether to add the values instead of overwriting them.
void convert(const std::byte* from, AudioFormat fmtFrom, std::byte* to,
	AudioFormat fmtTo, std::size_t count, bool add);

namespace {

constexpr static auto cfac16 = 32767.0; // 2^15 (16 - 1 sign bit)
constexpr static auto cfac32 = 2147483648.0; // 2^31 (32 - 1 sign bit)


template<typename T>
void assertValid(T* ptr, const char* msg) {
	if(ptr == nullptr) {
		throw std::runtime_error(msg);
	}
}

void checkSoundIo(int err, const char* msg) {
	if(err == SoundIoErrorNone) {
		return;
	}

	std::string m = msg;
	m += ": ";
	m += soundio_strerror(err);
	throw std::runtime_error(m);
}

} // anonymous namespace

// AudioPlayer
AudioPlayer::AudioPlayer() {
	init();
}

AudioPlayer::~AudioPlayer() {
	finish();
}

void AudioPlayer::init() {

	// NOTE: currently hardcoded settings
	constexpr auto sampleRate = 48000;
	constexpr auto channelCount = 2u;
	constexpr auto prefLatency = 1 / 60.f;

	// soundio
	soundio_ = soundio_create();
	assertValid(soundio_, "soundio_create failed");
	soundio_->userdata = this;
	soundio_->on_backend_disconnect = cbBackendDisconnect;

	// backend
	auto count = soundio_backend_count(soundio_);
	dlg_assert(count >= 0);
	for(auto i = 0; i < count; ++i) {
		auto name = soundio_backend_name(soundio_get_backend(soundio_, i));
		dlg_debug("Available soundio backend: {}", name);
	}

	checkSoundIo(soundio_connect(soundio_), "soundio_connect failed");
	auto bname = soundio_backend_name(soundio_->current_backend);
	dlg_info("Using soundio backend {}", bname);

	// device
	soundio_flush_events(soundio_);
	count = soundio_output_device_count(soundio_);
	dlg_assert(count >= 0);

	dlg_debug("Found {} soundio devices", count);
	for(auto i = 0; i < count; ++i) {
		auto dev = soundio_get_output_device(soundio_, i);
		dlg_debug("<< soundio device {}: {} >>", i, dev->name);
		dlg_debug("  raw: {}{}", std::boolalpha, dev->is_raw);
		dlg_debug("  device latency min: {}", dev->software_latency_min);
		dlg_debug("  device latency max: {}", dev->software_latency_max);

		dlg_debug("  device current format: {}",
			soundio_format_string(dev->current_format));
		for(auto i = 0; i < dev->format_count; ++i) {
			auto fmt = soundio_format_string(dev->formats[i]);
			dlg_debug("    supported format: {}", fmt);
		}

		dlg_debug("  current sample rate: {}", dev->sample_rate_current);
		for(auto i = 0; i < dev->sample_rate_count; ++i) {
			auto& range = dev->sample_rates[i];
			dlg_debug("    supported sample range: ({}, {})",
				range.min, range.max);
		}

		auto cl = dev->current_layout.name;
		dlg_debug("  current channel layout: {}", cl ? cl : "<none>");
		for(auto i = 0; i < dev->layout_count; ++i) {
			auto& layout = dev->layouts[i];
			if(layout.name) {
				dlg_debug("    available layout: {}", layout.name);
			}
		}

		soundio_device_unref(dev);
	}

	auto defaultID = soundio_default_output_device_index(soundio_);
	if(defaultID == -1) {
		throw std::runtime_error("soundio_default_output_device_index: -1");
	}

	device_ = soundio_get_output_device(soundio_, defaultID);
	assertValid(device_, "soundio_get_output_device failed");
	dlg_info("using soundio device {} ({})", defaultID, device_->name);

	// sample rate
	bool foundSampleRate = false;
	for(auto i = 0; i < device_->sample_rate_count; ++i) {
		auto& range = device_->sample_rates[i];
		if(range.min <= sampleRate && range.max >= sampleRate) {
			foundSampleRate = true;
			break;
		}
	}

	if(!foundSampleRate) {
		std::string msg = "soundio device does not support sample rate ";
		msg += std::to_string(sampleRate);
		throw std::runtime_error(msg);
	}

	// format
	enum SoundIoFormat format;
	int rating = 0;
	for(auto i = 0; i < device_->format_count; ++i) {
		auto f = device_->formats[i];
		if(f == SoundIoFormatFloat32NE) {
			format = f;
			format_ = AudioFormat::f32;
			break;
		} else if(f == SoundIoFormatFloat64NE) {
			format = f;
			format_ = AudioFormat::f64;
			rating = 3;
		} else if(f == SoundIoFormatS16NE && rating < 2) {
			format = f;
			format_ = AudioFormat::s16;
			rating = 2;
		} else if(f == SoundIoFormatS32NE && rating < 1) {
			format = f;
			format_ = AudioFormat::s32;
			rating = 1;
		}
	}

	if(rating == 0) {
		throw std::runtime_error("sound io device has no supported format");
	}

	// layout
	auto layout = soundio_channel_layout_get_default(channelCount);
	assertValid(layout, "soundio_channel_layout_get_default failed");

	// stream
	stream_ = soundio_outstream_create(device_);
	assertValid(stream_, "soundio_outstream_create failed");

	stream_->write_callback = &AudioPlayer::cbWrite;
	stream_->error_callback = &AudioPlayer::cbError;
	stream_->underflow_callback = &AudioPlayer::cbUnderflow;
	stream_->format = format;
	stream_->name = "doi";
	stream_->sample_rate = sampleRate;
	stream_->layout = *layout;
	stream_->userdata = this;
	stream_->software_latency = prefLatency;

	checkSoundIo(soundio_outstream_open(stream_), "soundio_outstream_open");
	dlg_info("using software latency {}", stream_->software_latency);

	// create ring buffer
	auto frameCount = 2 * bufferTime_ * stream_->sample_rate + 1024u;
	auto byteSize = frameCount * stream_->bytes_per_frame;
	buffer_ = soundio_ring_buffer_create(soundio_, byteSize);
	assertValid(buffer_, "Failed to create soundio ring buffer");

	// start audio thread
	audioThread_ = std::thread {[=]{ audioLoop(); }};

	checkSoundIo(soundio_outstream_start(stream_), "soundio_outstream_start");
}

void AudioPlayer::finish() {
	// we don't have to lock a mutex here, soundio has to care
	// of this internally and locking mutex_ might trigger
	// a deadlock (since sounio probably joins the audio thread)
	if(stream_) {
		soundio_outstream_destroy(stream_);
	}

	if(device_) {
		soundio_device_unref(device_);
	}

	if(soundio_) {
		soundio_destroy(soundio_);
	}
}

void AudioPlayer::audioLoop() {
	using namespace std::literals::chrono_literals;
	auto fillLimit = stream_->sample_rate * bufferTime_;
	auto lastTime = std::chrono::high_resolution_clock::duration {};

	while(true) {
		// we subtract a the time we needed to render the audio last time,
		// otherwise when rendering takes really long and then we sleep
		// normally, we might run out of frames.
		auto waitTime = std::chrono::duration<float>(bufferTime_ / 2);
		if(lastTime >= waitTime) {
			dlg_warn("Audio thread too slow/too much work");
		} else {
			std::this_thread::sleep_for(waitTime - lastTime);
		}

		auto count = soundio_ring_buffer_fill_count(buffer_);
		count /= stream_->bytes_per_frame;
		if(count >= fillLimit) {
			lastTime = {};
			continue;
		}

		auto now = std::chrono::high_resolution_clock::now();
		auto frames = std::max<unsigned>(fillLimit - count, 128u);
		dlg_info("filling {} frames", frames);
		dlg_assert(int(frames) < soundio_ring_buffer_free_count(buffer_));

		auto bytes = stream_->bytes_per_frame * frames;
		auto ptr = reinterpret_cast<float*>(
			soundio_ring_buffer_write_ptr(buffer_));
		std::memset(ptr, 0, bytes);

		{
			std::lock_guard lock(mutex_);
			for(auto& audio : audios_) {
				try {
					audio->render(*ptr, frames);
				} catch(const std::exception& err) {
					dlg_error("audio->render: {}", err.what());
				}
			}
		}

		soundio_ring_buffer_advance_write_ptr(buffer_, bytes);
		lastTime = std::chrono::high_resolution_clock::now() - now;
	}
}

Audio& AudioPlayer::add(std::unique_ptr<Audio> audio) {
	dlg_assert(audio);
	std::lock_guard lock(mutex());
	audios_.push_back(std::move(audio));
	return *audios_.back();
}

bool AudioPlayer::remove(Audio& audio) {
	// if we only read we don't need to lock the mutex
	auto it = std::find_if(audios_.begin(), audios_.end(), [&](auto& s) {
		return s.get() == &audio;
	});

	if(it == audios_.end()) {
		return false;
	}

	std::lock_guard lock(mutex());
	audios_.erase(it);
	return true;
}

void AudioPlayer::update() {
	soundio_flush_events(soundio_);
	if(error_.load()) {
		dlg_debug("Trying to reinitialize audio player");
		finish();
		error_.store(false);
		init();
	}
}

void AudioPlayer::cbWrite(struct SoundIoOutStream* stream, int min, int max) {
	dlg_assert(stream && stream->userdata);
	reinterpret_cast<AudioPlayer*>(stream->userdata)->output(stream, min, max);
}

void AudioPlayer::cbUnderflow(struct SoundIoOutStream*) {
	dlg_warn("audio buffer underflow");
}

void AudioPlayer::cbError(struct SoundIoOutStream* stream, int err) {
	dlg_warn("soundio error: {}", soundio_strerror(err));
	dlg_assert(stream && stream->userdata);
	auto ap = reinterpret_cast<AudioPlayer*>(stream->userdata);
	dlg_assert(ap->stream_ == stream);
	ap->error_.store(true);
}

void AudioPlayer::cbBackendDisconnect(struct SoundIo* soundio, int err) {
	dlg_warn("soundio backend disconnected: {}", soundio_strerror(err));
	dlg_assert(soundio && soundio->userdata);
	auto ap = reinterpret_cast<AudioPlayer*>(soundio->userdata);
	dlg_assert(ap->soundio_ == soundio);
	ap->error_.store(true);
}

void AudioPlayer::output(struct SoundIoOutStream* stream, int min, int max) {
	dlg_assert(stream == stream_);
	if(error_.load()) {
		dlg_error("AudioPlayer output called with error flag set");
		return;
	}

	auto avail = soundio_ring_buffer_fill_count(buffer_) / (2 * sizeof(float));
	auto pref = std::min<unsigned>(avail, bufferTime_ * stream_->sample_rate);
	auto remaining = std::clamp<unsigned>(pref, min, max);

	dlg_info("{} {} {} {}", avail, pref, min, max);

	if(remaining > avail) {
		dlg_warn("AudioPlayer::output: not enough data available: "
			"needed {}, available: {})", remaining, avail);
		// TODO: we could set bufferTime here
		// bufferTime_ = 2 * remaining / stream_->sample_rate;
	}

	dlg_info("writing {}", remaining);
	while(remaining > 0) {
		int frames = remaining;
		struct SoundIoChannelArea* areas;
		auto err = soundio_outstream_begin_write(stream_, &areas, &frames);
		dlg_info("frames: {}", frames);
		if(err) {
			dlg_error("soundio_begin_write: {} ({})", soundio_strerror(err), err);
			error_.store(true);
			return;
		}

		if(frames == 0) {
			break;
		}

		// we assume that the ring buffer is never cleared
		auto available = std::min<int>(avail, frames);
		auto readPtr = reinterpret_cast<const std::byte*>(
			soundio_ring_buffer_read_ptr(buffer_));

		// TODO: detect special (memcpy/loop convert) cases for performance
		for(auto i = 0u; i < unsigned(available); ++i) {
			auto sz = sizeof(float);
			if(format_ == AudioFormat::f32) {
				std::memcpy(areas[0].ptr, readPtr, sz);
				std::memcpy(areas[1].ptr, readPtr + sz, sz);
			} else {
				convert(readPtr, AudioFormat::f32,
					reinterpret_cast<std::byte*>(areas[0].ptr),
					format_, 1u, false);
				convert(readPtr + sz, AudioFormat::f32,
					reinterpret_cast<std::byte*>(areas[1].ptr),
					format_, 1u, false);
			}

			areas[0].ptr += areas[0].step;
			areas[1].ptr += areas[1].step;
			readPtr += 2 * sz;
		}

		auto byteSize = stream_->bytes_per_frame * available;
		soundio_ring_buffer_advance_read_ptr(buffer_, byteSize);

		// fill rest with zeroes. Should not happend in normal case
		if(frames > available) {
			dlg_info("internal underflow");
		}

		for(auto i = available; i < frames; ++i) {
			std::memset(areas[0].ptr, 0, stream_->bytes_per_sample);
			std::memset(areas[1].ptr, 0, stream_->bytes_per_sample);
			areas[0].ptr += areas[0].step;
			areas[1].ptr += areas[1].step;
		}

		avail -= frames;
		remaining -= frames;

		err = soundio_outstream_end_write(stream_);
		if(err) {
			if(err != SoundIoErrorUnderflow) {
				dlg_error("soundio_end_write: {} ({})", soundio_strerror(err), err);
				error_.store(true);
			}

			return;
		}
	}
}

// util
template<typename T>
void writeAdd(T val, std::byte* to, bool add) {
	auto* ptr = *reinterpret_cast<T*>(to);
	*ptr = add * (*ptr) + val;
}

template<typename To, typename From>
void doConvert(const std::byte* from, std::byte* to, std::size_t count,
		bool add, double fac1, double fac2) {

	// the real conversion loop. As compact as possible
	// reinterpret_cast instead of memcpy since its allowed for
	// std::byte (does not break strict aliasing) and should/could be faster
	for(auto i = 0u; i < count; ++i) {
		double val = *reinterpret_cast<const From*>(from) / fac1;
		auto ptr = reinterpret_cast<To*>(to);
		if(add) {
			*ptr += val * fac2;
		} else {
			*ptr = val * fac2;
		}

		from += sizeof(From);
		to += sizeof(To);
	}
}

template<typename From>
void toSwitch(const std::byte* from, std::byte* to, std::size_t count,
		bool add, double fac1, AudioFormat fmtTo) {
	switch(fmtTo) {
		case AudioFormat::f32:
			doConvert<From, float>(from, to, count, add, fac1, 1.0);
			break;
		case AudioFormat::f64:
			doConvert<From, double>(from, to, count, add, fac1, 1.0);
			break;
		case AudioFormat::s16:
			doConvert<From, std::int16_t>(from, to, count, add, fac1, cfac16);
			break;
		case AudioFormat::s32:
			doConvert<From, std::int32_t>(from, to, count, add, fac1, cfac32);
			break;
		default:
			dlg_error("convert: invalid fmtTo");
			return;
	}
}

void convert(const std::byte* from, AudioFormat fmtFrom, std::byte* to,
		AudioFormat fmtTo, std::size_t count, bool add) {

	switch(fmtFrom) {
		case AudioFormat::f32:
			toSwitch<float>(from, to, count, add, 1.0, fmtTo);
			break;
		case AudioFormat::f64:
			toSwitch<double>(from, to, count, add, 1.0, fmtTo);
			break;
		case AudioFormat::s16:
			toSwitch<std::int16_t>(from, to, count, add, cfac16, fmtTo);
			break;
		case AudioFormat::s32:
			toSwitch<std::int32_t>(from, to, count, add, cfac32, fmtTo);
			break;
		default:
			dlg_error("convert: invalid fmtFrom");
			return;
	}
}

} // namespace doi
