#include <tkn/sound.hpp>
#include <speex_resampler.h>
#include <dlg/dlg.hpp>
#include <cmath>

namespace tkn {

// resampling
unsigned resample(SoundBufferView dst, SoundBufferView src) {
	if(dst.rate == src.rate) {
		dlg_assert(dst.frameCount == src.frameCount);
		if(dst.channelCount == src.channelCount) {
			auto nbytes = dst.frameCount * src.channelCount * sizeof(float);
			memcpy(dst.data, src.data, nbytes);
		} else {
			auto nbytes = dst.frameCount * sizeof(float);
			for(auto i = 0u; i < dst.channelCount; ++i) {
				// TODO: not sure if that is a good idea, see below
				auto srcc = i % src.channelCount;
				memcpy(dst.data + i, src.data + srcc, nbytes);
			}
		}

		return dst.frameCount;
	}

	int err {};
	auto speex = speex_resampler_init(src.channelCount,
		src.rate, dst.rate, SPEEX_RESAMPLER_QUALITY_DESKTOP, &err);
	dlg_assert(speex && !err);
	auto ret = resample(speex, dst, src);
	speex_resampler_destroy(speex);
	return ret;
}

unsigned resample(SpeexResamplerState* speex,
		SoundBufferView dst, SoundBufferView src) {
	int err;
	speex_resampler_set_input_stride(speex, src.channelCount);
	speex_resampler_set_output_stride(speex, dst.channelCount);
	unsigned ret = 0xFFFFFFFFu;
	for(auto i = 0u; i < dst.channelCount; ++i) {
		spx_uint32_t in_len = src.frameCount;
		spx_uint32_t out_len = dst.frameCount;
		// TODO: not sure if that is a good idea.
		// could be optimized in that case anyways, we don't
		// have to resample the same channel multiple times.
		// Lookup surround sound mixing.
		auto srcc = i % src.channelCount;
		err = speex_resampler_process_float(speex, srcc,
			src.data + srcc, &in_len, dst.data + i, &out_len);

		dlg_assertm(!err, "{}", err);
		dlg_assertm(in_len == src.frameCount, "{} vs {}",
			in_len, src.frameCount);
		dlg_assertm(out_len == dst.frameCount || out_len == dst.frameCount - 1,
			"{} vs {}", out_len, dst.frameCount);
		ret = out_len;
	}

	return ret;
}

UniqueSoundBuffer resample(SoundBufferView view, unsigned rate,
		unsigned channels) {
	UniqueSoundBuffer ret;
	ret.channelCount = channels;
	ret.rate = rate;
	ret.frameCount = std::ceil(view.frameCount * ((double)rate / view.rate));
	auto count = ret.frameCount * ret.channelCount;
	ret.data = std::make_unique<float[]>(count);
	resample(ret, view);
	return ret;
}

UniqueSoundBuffer loadVorbis(nytl::StringParam file) {
	int error = 0;
	auto vorbis = stb_vorbis_open_filename(file.c_str(), &error, nullptr);
	if(!vorbis) {
		auto msg = std::string("loadVorbis: failed to load");
		msg += file.c_str();
		throw std::runtime_error(msg);
	}

	auto info = stb_vorbis_get_info(vorbis);

	UniqueSoundBuffer ret;
	ret.channelCount = info.channels;
	ret.rate = info.sample_rate;
	ret.frameCount = stb_vorbis_stream_length_in_samples(vorbis);

	std::size_t count = ret.channelCount * ret.frameCount;
	ret.data = std::make_unique<float[]>(count);

	auto ns = stb_vorbis_get_samples_float_interleaved(vorbis, info.channels,
		ret.data.get(), count);
	dlg_assert((std::size_t) ns == ret.frameCount);
	stb_vorbis_close(vorbis);
	return ret;
}

// MidiAudio
MidiAudio::MidiAudio(tsf* tsf, nytl::StringParam midiPath, unsigned rate)
		: tsf_(tsf), rate_(rate) {
	messages_ = tml_load_filename(midiPath.c_str());
	if(!messages_) {
		std::string err = "Could not load midi file ";
		err += midiPath;
		throw std::runtime_error(err);
	}

	current_ = messages_;
	tsf_set_output(tsf_, TSF_STEREO_INTERLEAVED, rate_, 0);
}

MidiAudio::MidiAudio(nytl::StringParam tsfPath, nytl::StringParam midiPath,
		unsigned rate) : rate_(rate) {
	tsf_ = tsf_load_filename(tsfPath.c_str());
	if(!tsf_) {
		std::string err = "Could not load soundfont file ";
		err += midiPath;
		throw std::runtime_error(err);
	}

	messages_ = tml_load_filename(midiPath.c_str());
	if(!messages_) {
		std::string err = "Could not load midi file ";
		err += midiPath;
		throw std::runtime_error(err);
	}

	current_ = messages_;
}

MidiAudio::~MidiAudio() {
	if(messages_) {
		tml_free(const_cast<tml_message*>(messages_));
	}
	if(tsf_) {
		tsf_close(tsf_);
	}
}

void MidiAudio::handle(const tml_message& msg) {
	switch(msg.type) {
	case TML_PROGRAM_CHANGE:
		// TODO: drum channel rules?
		tsf_channel_set_presetnumber(tsf_, msg.channel, msg.program, 0);
		break;
	case TML_PITCH_BEND: // pitch wheel modification
		tsf_channel_set_pitchwheel(tsf_, msg.channel, msg.pitch_bend);
		break;
	case TML_CONTROL_CHANGE: // MIDI controller messages
		tsf_channel_midi_control(tsf_, msg.channel, msg.control, msg.control_value);
		break;
	case TML_NOTE_ON:
		tsf_note_on(tsf_, 0, msg.key, (float) msg.velocity / 255);
		break;
	case TML_NOTE_OFF:
		tsf_note_off(tsf_, 0, msg.key);
		break;
	default:
		break;
	}
}

void MidiAudio::render(unsigned nb, float* buf, bool mix) {
	auto v = volume_.load();
	if(v <= 0.f) {
		return;
	}

	// conversion from multiplicative factor to db gain, acoording to
	// http://www.sengpielaudio.com/calculator-FactorRatioLevelDecibel.htm
	// We could probably just store this in volume_ with a special value
	// for disabled to not calculate this in ever render iteration?
	auto dbgain = 20 * std::log10(v);
	tsf_set_output(tsf_, TSF_STEREO_INTERLEAVED, rate_, dbgain);

	// handle messages
	while(current_ && current_->time <= time_) {
		handle(*current_);
		current_ = current_->next;
		if(!current_) { // finished
			time_ = 0.f;
			volume_.store(0.f);
		}

		break;
	}

	auto nf = tkn::AudioPlayer::blockSize * nb;
	tsf_render_float(tsf_, buf, nf, mix);

	time_ += 1000 * (double(nf) / rate_);
}

// SoundBufferAudio
void SoundBufferAudio::render(unsigned nb, float* buf, bool mix) {
	auto v = volume_.load();
	if(v <= 0.f) {
		return;
	}

	auto tf = nb * AudioPlayer::blockSize;
	auto nf = tf;
	if(frame_ + nf > buffer_.frameCount) {
		nf = buffer_.frameCount - frame_;
	}

	auto cc = buffer_.channelCount;
	auto ns = nf * cc;

	// when not mixing, make sure to set the remaining samples to 0
	// otherwise we get undefined data in the end
	if(mix) {
		for(auto i = 0u; i < ns; ++i) {
			buf[i] += v * buffer_.data[frame_ * cc + i];
		}
	} else if(volume() != 1.f) {
		auto i = 0u;
		for(; i < ns; ++i) {
			buf[i] = v * buffer_.data[frame_ * cc + i];
		}
		for(; i < tf * cc; ++i) {
			buf[i] = 0.f;
		}
	} else {
		memcpy(buf, buffer_.data + frame_ * cc, ns * sizeof(float));
		memset(buf + ns, 0x0, (tf - nf) * sizeof(float));
	}

	frame_ += nf;
	if(frame_ == buffer_.frameCount) {
		volume_.store(0.f);
		frame_ = 0;
	}
}

// StreamedVorbisAudio
StreamedVorbisAudio::StreamedVorbisAudio(nytl::StringParam file, unsigned rate,
		unsigned channels) {
	int error = 0;
	vorbis_ = stb_vorbis_open_filename(file.c_str(), &error, nullptr);
	if(!vorbis_) {
		auto msg = std::string("StreamVorbisAudio: failed to load ");
		msg += file.c_str();
		msg += ": ";
		msg += std::to_string(error);
		throw std::runtime_error(msg);
	}

	rate_ = rate;
	channels_ = channels;

	auto info = stb_vorbis_get_info(vorbis_);
	if(info.sample_rate != rate_) {
		int err {};
		speex_ = speex_resampler_init(info.channels,
			info.sample_rate, rate_, SPEEX_RESAMPLER_QUALITY_DESKTOP, &err);
		dlg_assert(speex_ && !err);
	}
}

StreamedVorbisAudio::StreamedVorbisAudio(nytl::Span<const std::byte> buf,
		unsigned rate, unsigned channels) {
	int error = 0;
	auto data = (const unsigned char*) buf.data();
	vorbis_ = stb_vorbis_open_memory(data, buf.size(), &error, nullptr);
	if(!vorbis_) {
		auto msg = std::string("StreamVorbisAudio: failed to load from memory");
		throw std::runtime_error(msg);
	}

	rate_ = rate;
	channels_ = channels;

	auto info = stb_vorbis_get_info(vorbis_);
	if(info.sample_rate != rate_) {
		int err {};
		speex_ = speex_resampler_init(info.channels,
			info.sample_rate, rate_, SPEEX_RESAMPLER_QUALITY_DESKTOP, &err);
		dlg_assert(speex_ && !err);
	}
}

StreamedVorbisAudio::~StreamedVorbisAudio() {
	if(speex_) {
		speex_resampler_destroy(speex_);
	}
	if(vorbis_) {
		stb_vorbis_close(vorbis_);
	}
}

void StreamedVorbisAudio::update() {
	// workaround for the threadid debug-check in our ring buffer.
	// update is called by the main thread first when the audio is
	// added (making this the producer) and (strictly) afterwards
	// only by the update thread. This is valid, the AudioPlayer
	// manages the switch of the producer switch but it defies
	// the debug check logic in the threadbuffer
#ifndef NDEBUG
	auto resetThreads = (buffer_.producer_id == std::thread::id{});
#endif

	auto cap = buffer_.available_write();
	if(cap < minWrite) { // not worth it, still full enough
		return;
	}

	auto ns = cap;
	auto info = stb_vorbis_get_info(vorbis_);
	if(info.sample_rate != rate_) {
		// in this case we need less original samples since they
		// will be upsampled below. Taking all samples here would
		// result in more upsampled samples than there is capacity.
		// We choose exactly so many source samples that the upsampled
		// result will fill the remaining capacity
		ns = std::floor(ns * ((double) info.sample_rate / rate_));
	}

	auto b0 = bufCacheU.get(0, ns).data();
	auto read = stb_vorbis_get_samples_float_interleaved(vorbis_,
		info.channels, b0, ns);
	if(read == 0) { // stream finished, no samples left
		stb_vorbis_seek_start(vorbis_);
		volume_.store(0.f);
		return;
	}

	unsigned c = info.channels;
	if(rate_ != info.sample_rate || channels_ != c) {
		SoundBufferView src;
		src.channelCount = info.channels;
		src.frameCount = read;
		src.rate = info.sample_rate;
		src.data = b0;

		SoundBufferView dst;
		dst.channelCount = channels_;
		dst.frameCount = std::ceil(read * ((double)rate_ / src.rate));
		dst.rate = rate_;
		dst.data = bufCacheU.get(1, dst.frameCount * channels_).data();

		if(speex_) {
			dst.frameCount = resample(speex_, dst, src);
		} else { // we only "resample" channels
			resample(dst, src);
		}

		auto count = dst.channelCount * dst.frameCount;
		auto written = buffer_.enqueue(dst.data, count);
		dlg_assertm(written == count, "{} {}", written, count);
	} else {
		auto count = read * channels_;
		auto written = buffer_.enqueue(b0, count);
		dlg_assert(written == count && written == cap);
	}

#ifndef NDEBUG
	if(resetThreads) {
		buffer_.reset_thread_ids();
	}
#endif // NDEBUG
}

void StreamedVorbisAudio::render(unsigned nb, float* buf, bool mix) {
	auto v = volume_.load();
	if(v <= 0.f) {
		return;
	}

	auto nf = AudioPlayer::blockSize * nb;
	auto ns = nf * channels_;

	if(mix) {
		auto b0 = bufCacheR.get(0, ns).data();
		auto count = buffer_.dequeue(b0, ns);
		dlg_assertl(dlg_level_warn, count == ns);
		for(auto i = 0u; i < count; ++i) {
			buf[i] += v * b0[i];
		}

	} else if(v != 1.f) {
		auto b0 = bufCacheR.get(0, ns).data();
		auto count = buffer_.dequeue(b0, ns);
		dlg_assertl(dlg_level_warn, count == ns);
		auto i = 0u;
		for(; i < count; ++i) {
			buf[i] = v * b0[i];
		}
		for(; i < ns; ++i) {
			buf[i] = 0.f;
		}
	} else {
		auto count = buffer_.dequeue(buf, ns);
		dlg_assertl(dlg_level_warn, count == ns);
		memset(buf + count, 0x0, ns - count);
	}
}

} // namespace tkn
