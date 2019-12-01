#include <tkn/sound.hpp>
#include <speex_resampler.h>
#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>
#include <cmath>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.h>
#include <minimp3.h>

namespace tkn {
namespace {

// Returns 0 for no error, 1 for eof and -e for error e.
// Outputs warning on error
int check(std::FILE* f) {
	int e = std::ferror(f);
	if(e) {
		dlg_warn("File error: {} ({})", std::strerror(e), e);
		return -e;
	}

	return std::feof(f) != 0;
}

} // anon namespace

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
		auto msg = std::string("loadVorbis: failed to load ");
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

// mp3
UniqueSoundBuffer loadMP3(nytl::StringParam file) {
	// TODO: not tested
	dlg_warn("NOT TESTED");

	auto f = std::fopen(file.c_str(), "r");
	if(!f) {
		auto msg = std::string("loadMP3: failed to load ");
		msg += file.c_str();
		throw std::runtime_error(msg);
	}

	mp3dec_t decoder;
	mp3dec_init(&decoder);

	UniqueSoundBuffer ret {};
	mp3dec_frame_info_t fi;
	std::vector<std::uint8_t> rbuf(16 * 1024);
	auto rbufSize = 0u;

	size_t dataSamples = 10 * MINIMP3_MAX_SAMPLES_PER_FRAME;
	ret.data = std::make_unique<float[]>(dataSamples);

	while(true) {
		auto off = ret.frameCount * ret.channelCount;
		if(off + MINIMP3_MAX_SAMPLES_PER_FRAME > dataSamples) {
			dataSamples *= 4;
			auto nd = std::make_unique<float[]>(dataSamples);
			memcpy(nd.get(), ret.data.get(), off * sizeof(float));
			ret.data = std::move(nd);
		}

		// TODO: could be done more efficiently with mmap
		// decode first frame
		auto size = rbuf.size() - rbufSize;
		int num = std::fread(rbuf.data() + rbufSize, 1, size, f);
		if(num == 0) {
			dlg_assert(!ferror(f));
			break;
		}

		rbufSize += num;
		int nf = mp3dec_decode_frame(&decoder, rbuf.data(), rbufSize,
			ret.data.get() + off, &fi);

		// TODO: increase probably not needed. Just fail when
		// not possible
		auto maxs = 1024u * 1024u;
		while(nf == 0 && fi.frame_bytes == 0 && rbuf.size() < maxs) {
			rbuf.resize(2 * rbuf.size());
			dlg_debug("increasing read buffer size to {}", rbuf.size());
			auto size = 2 * rbuf.size() - rbufSize;
			num = std::fread(rbuf.data() + rbufSize, 1, size, f);
			if(num == 0) {
				dlg_assert(!ferror(f));
				dlg_warn("MP3 file incomplete");
				return {};
			}

			rbufSize += num;
			nf = mp3dec_decode_frame(&decoder, rbuf.data(), rbufSize,
				ret.data.get() + off, &fi);
		}

		if(rbuf.size() >= maxs) {
			dlg_warn("MP3 file has too long blocks of invalid data");
			return {};
		}

		if(nf > 0) {
			if(!ret.rate) {
				ret.channelCount = fi.channels;
				ret.rate = fi.hz;
			}

			dlg_assert(ret.channelCount == (unsigned) fi.channels);
			dlg_assert(ret.rate == (unsigned) fi.hz);
			ret.frameCount += nf;
			rbufSize -= fi.frame_bytes;

			auto size = rbufSize - fi.frame_bytes;
			std::memmove(rbuf.data(), rbuf.data() + fi.frame_bytes, size);
		}
	}

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
	auto nf = tkn::AudioPlayer::blockSize * nb;
	auto ns = nf * 2u;
	if(v <= 0.f) {
		if(!mix) {
			std::memset(buf, 0x0, ns * sizeof(float));
		}

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

	tsf_render_float(tsf_, buf, nf, mix);

	time_ += 1000 * (double(nf) / rate_);
}

// SoundBufferAudio
void SoundBufferAudio::render(unsigned nb, float* buf, bool mix) {
	auto v = volume_.load();
	if(v <= 0.f) {
		if(!mix) {
			auto ns = nb * AudioPlayer::blockSize * buffer_.channelCount;
			std::memset(buf, 0x0, ns * sizeof(float));
		}

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
		std::memset(buf + ns, 0x0, (tf - nf) * sizeof(float));
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
	auto resetThreads = (buffer_.consumer_id == std::thread::id{});
	auto sg = nytl::ScopeGuard {[&]{ if(resetThreads) buffer_.reset_thread_ids(); }};
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
}

void StreamedVorbisAudio::render(unsigned nb, float* buf, bool mix) {
	auto nf = AudioPlayer::blockSize * nb;
	auto ns = nf * channels_;
	auto v = volume_.load();
	if(v <= 0.f) {
		if(!mix) {
			std::memset(buf, 0x0, ns * sizeof(float));
		}

		return;
	}

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
		std::memset(buf + count, 0x0, (ns - count) * sizeof(float));
	}
}

// StreamedMP3Audio
StreamedMP3Audio::StreamedMP3Audio(nytl::StringParam file) {
	mp3dec_init(&mp3_);
	file_ = std::fopen(file.c_str(), "r");
	if(!file_) {
		auto msg = std::string("failed to open streamed mp3 file ");
		msg += file.c_str();
		throw std::runtime_error(msg);
	}

	// parse frames until we get for the first time.
	// only this way we can know the files channel count and rate
	rbuf_.resize(16 * 1024);
	mp3dec_frame_info_t fi {};
	int nf = 0u;
	auto maxs = MINIMP3_MAX_SAMPLES_PER_FRAME;
	auto samples = bufCacheU.get(0, maxs).data();

	while(nf == 0) {
		auto size = rbuf_.size() - rbufSize_;
		auto n = std::fread(rbuf_.data() + rbufSize_, 1, size, file_);
		if(n == 0) {
			check(file_);
			auto msg = std::string("invalid mp3 file (reading failed) ");
			msg += file.c_str();
			throw std::runtime_error(msg);
		}

		rbufSize_ += n;
		nf = mp3dec_decode_frame(&mp3_, rbuf_.data(), rbufSize_, samples, &fi);
		if(fi.frame_bytes == 0) {
			auto msg = std::string("invalid mp3 file (large invalid chunk) ");
			msg += file.c_str();
			throw msg;
		}

		rbufSize_ -= fi.frame_bytes;
		std::memmove(rbuf_.data(), rbuf_.data() + fi.frame_bytes, rbufSize_);
	}

	channels_ = fi.channels;
	rate_ = fi.hz;
	buffer_.enque(samples, channels_ * nf);

#ifndef NDEBUG
	buffer_.reset_thread_ids();
#endif
}

StreamedMP3Audio::~StreamedMP3Audio() {
	if(file_) {
		std::fclose(file_);
	}
}

void StreamedMP3Audio::update() {
#ifndef NDEBUG
	auto resetThreads = (buffer_.consumer_id == std::thread::id{});
	auto sg = nytl::ScopeGuard {[&]{ if(resetThreads) buffer_.reset_thread_ids(); }};
#endif

	if(buffer_.available_write() < minWrite) {
		return;
	}

	unsigned maxs = MINIMP3_MAX_SAMPLES_PER_FRAME;
	auto samples = bufCacheU.get(0, maxs).data();
	mp3dec_frame_info_t fi {};
	int nf = 0u;

	while(buffer_.available_write() >= maxs) {
		// fill read buffer
		auto size = rbuf_.size() - rbufSize_;
		if(size > 0) {
			int n = std::fread(rbuf_.data() + rbufSize_, 1, size, file_);
			if(n == 0) {
				check(file_);
				dlg_trace("stream done");
				std::fseek(file_, 0, SEEK_SET);
				volume_.store(0.f);
				break;
			}
			rbufSize_ += n;
		}

		// decode and enqueue frames, if any
		nf = mp3dec_decode_frame(&mp3_, rbuf_.data(), rbufSize_, samples, &fi);

		if(nf > 0) {
			dlg_assert((unsigned) fi.channels == channels_);
			dlg_assert((unsigned) fi.hz == rate_);
			buffer_.enque(samples, channels_ * nf);
		}

		rbufSize_ -= fi.frame_bytes;
		std::memmove(rbuf_.data(), rbuf_.data() + fi.frame_bytes, rbufSize_);
	}
}

void StreamedMP3Audio::render(unsigned nb, float* buf, bool mix) {
	auto v = volume_.load();
	auto ns = nb * AudioPlayer::blockSize * channels_;
	if(v <= 0.f) {
		if(!mix) {
			std::memset(buf, 0x0, ns * sizeof(float));
		}

		return;
	}

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
		std::memset(buf + count, 0x0, (ns - count) * sizeof(float));
	}
}


} // namespace tkn
