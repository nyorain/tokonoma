#define DLG_DEFAULT_TAGS "audio",
#include <stage/audio.hpp>
#include <soundio/soundio.h>
#include <dlg/dlg.hpp>

#include <stdexcept>

namespace doi {
namespace {

template<typename T>
void check(T* ptr, const char* msg)
{
	if(ptr == nullptr) {
		throw std::runtime_error(msg);
	}
}

void checkSoundIo(int err, const char* msg)
{
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
AudioPlayer::AudioPlayer()
{
	init();
}

AudioPlayer::~AudioPlayer()
{
	finish();
}

void AudioPlayer::init()
{
	// TODO: currently hardcoded settings
	// TODO: clamp softwareLatency to supported device range
	constexpr auto sampleRate = 48000;
	constexpr auto softwareLatency = 0.1;
	constexpr auto channelCount = 2u;

	// TODO: on_device_change callback? additional callbacks?
	// at least on_backend_disconnect

	// TODO: wrap debug loops in dlg_check blocks

	// soundio
	soundio_ = soundio_create();
	check(soundio_, "soundio_create failed");
	soundio_->userdata = this;

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

	// TODO: debug output channel layouts?
	for(auto i = 0; i < count; ++i) {
		auto dev = soundio_get_output_device(soundio_, i);
		dlg_debug("soundio device {}: {}", i, dev->name);
		dlg_debug("\tdevice is raw: {}", dev->is_raw);
		dlg_debug("\tdevice current sample rate: {}", dev->sample_rate_current);
		dlg_debug("\tdevice latency min: {}", dev->software_latency_min);
		dlg_debug("\tdevice latency max: {}", dev->software_latency_max);
		dlg_debug("\tdevice current format: {}",
			soundio_format_string(dev->current_format));

		for(auto i = 0; i < dev->format_count; ++i) {
			auto fmt = soundio_format_string(dev->formats[i]);
			dlg_debug("\tdevice supports format {}", fmt);
		}

		for(auto i = 0; i < dev->sample_rate_count; ++i) {
			auto& range = dev->sample_rates[i];
			dlg_debug("\tdevice sample range ({}, {})", range.min, range.max);
		}

		soundio_device_unref(dev);
	}

	auto defaultID = soundio_default_output_device_index(soundio_);
	if(defaultID == -1) {
		throw std::runtime_error("soundio_default_output_device_index: -1");
	}

	device_ = soundio_get_output_device(soundio_, defaultID);
	check(device_, "soundio_get_output_device failed");
	dlg_info("using soundio device {} ({})", defaultID, device_->name);

	// sample rate
	// TODO: maybe also support 44100 if needed?
	// how to resample stuff? we would have to expose it somehow
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
	int rate = 0;
	for(auto i = 0; i < device_->format_count; ++i) {
		auto f = device_->formats[i];
		if(f == SoundIoFormatFloat32NE) {
			format = f;
			format_ = AudioFormat::f32;
			break;
		} else if(f == SoundIoFormatFloat64NE) {
			format = f;
			format_ = AudioFormat::f64;
			rate = 3;
		} else if(f == SoundIoFormatS16NE && rate < 2) {
			format = f;
			format_ = AudioFormat::s16;
			rate = 2;
		} else if(f == SoundIoFormatS32NE && rate < 1) {
			format = f;
			format_ = AudioFormat::s32;
			rate = 1;
		}
	}

	if(rate == 0) {
		throw std::runtime_error("sound io device has no supported format");
	}

	// layout
	auto layout = soundio_channel_layout_get_default(channelCount);
	check(layout, "soundio_channel_layout_get_default failed");

	// stream
	stream_ = soundio_outstream_create(device_);
	check(stream_, "soundio_outstream_create failed");

	stream_->write_callback = &AudioPlayer::cbWrite;
	stream_->error_callback = &AudioPlayer::cbError;
	stream_->underflow_callback = &AudioPlayer::cbUnderflow;
	stream_->format = format;
	stream_->name = "doi";
	stream_->sample_rate = sampleRate;
	stream_->layout = *layout;
	stream_->userdata = this;
	stream_->software_latency = softwareLatency;

	checkSoundIo(soundio_outstream_open(stream_), "soundio_outstream_open");
	checkSoundIo(soundio_outstream_start(stream_), "soundio_outstream_start");

	dlg_info("using software latency {}", stream_->software_latency);
}

void AudioPlayer::finish()
{
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

Audio& AudioPlayer::add(std::unique_ptr<Audio> audio)
{
	dlg_assert(audio);
	std::lock_guard lock(mutex());
	audios_.push_back(std::move(audio));
	return *audios_.back();
}

bool AudioPlayer::remove(Audio& audio)
{
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

void AudioPlayer::update()
{
	if(error_.load()) {
		dlg_debug("Trying to reinitialize audio player");
		finish();
		error_.store(false);
		auto oldformat = format_;
		init();
		onReinit(*this, oldformat == format_);
	}
}

void AudioPlayer::cbWrite(struct SoundIoOutStream* stream, int min, int max)
{
	dlg_assert(stream && stream->userdata);
	reinterpret_cast<AudioPlayer*>(stream->userdata)->output(stream, min, max);
}

void AudioPlayer::cbUnderflow(struct SoundIoOutStream*)
{
	// TODO: can we fill the buffer here?
	dlg_warn("audio buffer underflow");
}

void AudioPlayer::cbError(struct SoundIoOutStream* stream, int)
{
	dlg_assert(stream && stream->userdata);
	reinterpret_cast<AudioPlayer*>(stream->userdata)->error(stream);
}

void AudioPlayer::output(struct SoundIoOutStream* stream, int min, int max)
{
	dlg_assert(stream == stream_);
	if(error_.load()) {
		dlg_error("AudioPlayer output called with error flag set");
		return;
	}

	// TODO: don't just use a random clamped value
	auto remaining = std::clamp(4096, min, max);

	while(remaining > 0) {
		int frames = remaining;
		struct SoundIoChannelArea* areas;
		auto err = soundio_outstream_begin_write(stream_, &areas, &frames);
		if(err) {
			dlg_error("soundio_begin_write: {} ({})", soundio_strerror(err), err);
			error_.store(true);
			return;
		}

		if(frames == 0) {
			break;
		}

		remaining -= frames;

		// bufferCache_ must never be accessed in main thread
		bufferCache_.clear();
		for(auto i = 0u; i < 2u; ++i) { // TODO: hardcoded channel count
			auto ptr = reinterpret_cast<std::byte*>(areas[i].ptr);
			auto step = static_cast<unsigned int>(areas[i].step);
			bufferCache_.push_back({ptr, step});
			std::memset(ptr, 0, step * frames); // clear buffers
		}

		{
			// NOTE: this function must be realtime so this mutex
			// lock *might* be problematic. Just make sure
			// to keep this in mind when getting audio issues
			// at some point
			std::lock_guard lock(mutex_);
			for(auto& audio : audios_) {
				try {
					audio->render(bufferCache_, format_, frames);
				} catch(const std::exception& err) {
					dlg_error("audio->render: {}", err.what());
				}
			}
		}

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

void AudioPlayer::error(struct SoundIoOutStream* stream)
{
	dlg_assert(stream == stream_);
	dlg_error("critical audio error, trying to reinitialize in next frame");
	error_.store(true);
}

// util
void convert(void* from, AudioFormat fmtFrom, void* to, AudioFormat fmtTo, bool add)
{
	constexpr static auto fac16 = 32767.0; // 2^15 (16 - 1 sign bit)
	constexpr static auto fac32 = 2147483648.0; // 2^31 (32 - 1 sign bit)

	// TODO: optimize case where from == to

	double val; // normalized
	switch(fmtFrom) {
		case AudioFormat::f32:
			val = *reinterpret_cast<float*>(from);
			break;
		case AudioFormat::f64:
			val = *reinterpret_cast<double*>(from);
			break;
		case AudioFormat::s16:
			val = *reinterpret_cast<std::int16_t*>(from) / fac16;
			break;
		case AudioFormat::s32:
			val = *reinterpret_cast<std::int32_t*>(from) / fac32;
			break;
		default:
			dlg_error("convert: invalid fmtFrom");
			return;
	}

	if(add) {
		switch(fmtTo) {
			case AudioFormat::f32:
				*reinterpret_cast<float*>(to) += val;
				break;
			case AudioFormat::f64:
				*reinterpret_cast<double*>(to) += val;
				break;
			case AudioFormat::s16:
				*reinterpret_cast<std::int16_t*>(to) += val * fac16;
				break;
			case AudioFormat::s32:
				*reinterpret_cast<std::int32_t*>(to) += val * fac32;
				break;
			default:
				dlg_error("convert: invalid fmtTo");
				return;
		}
	} else {
		switch(fmtTo) {
			case AudioFormat::f32:
				*reinterpret_cast<float*>(to) = val;
				break;
			case AudioFormat::f64:
				*reinterpret_cast<double*>(to) = val;
				break;
			case AudioFormat::s16:
				*reinterpret_cast<std::int16_t*>(to) = val * fac16;
				break;
			case AudioFormat::s32:
				*reinterpret_cast<std::int32_t*>(to) = val * fac32;
				break;
			default:
				dlg_error("convert: invalid fmtTo");
				return;
		}
	}
}

} // namespace doi
