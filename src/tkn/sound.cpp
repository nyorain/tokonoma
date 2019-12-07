#include <tkn/sound.hpp>
#include <tkn/sampling.hpp>
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
	auto f = File(file, "rb");
	if(!f) {
		auto msg = std::string("loadMP3: failed to load ");
		msg += file.c_str();
		throw std::runtime_error(msg);
	}

	mp3dec_t decoder;
	mp3dec_init(&decoder);

	UniqueSoundBuffer ret {};
	mp3dec_frame_info_t fi;

	constexpr auto readSize = 16 * 1024;
	auto rbuf = std::make_unique<std::uint8_t[]>(readSize);
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
		auto size = readSize - rbufSize;
		int num = std::fread(rbuf.get() + rbufSize, 1, size, f);
		if(num == 0) {
			dlg_assert(!ferror(f));
			if(!ret.rate) {
				auto msg = std::string("loadMP3: failed to load ");
				msg += file.c_str();
				msg += ": Unexpected eof";
				throw std::runtime_error(msg);
			}
			break;
		}

		rbufSize += num;
		int nf = mp3dec_decode_frame(&decoder, rbuf.get(), rbufSize,
			ret.data.get() + off, &fi);

		if(nf == 0 && fi.frame_bytes == 0) {
			auto msg = std::string("loadMP3: failed to load ");
			msg += file.c_str();
			msg += ": Too much invalid data";
			throw std::runtime_error(msg);
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
			std::memmove(rbuf.get(), rbuf.get() + fi.frame_bytes, rbufSize);
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

// MP3Decoder
MP3Decoder::MP3Decoder(nytl::StringParam file) {
	mp3dec_init(&mp3_);
	file_ = File(file, "rb");
	if(!file_) {
		auto msg = std::string("failed to open streamed mp3 file ");
		msg += file.c_str();
		throw std::runtime_error(msg);
	}

	init();
}

MP3Decoder::MP3Decoder(File&& file) : file_(std::move(file)) {
	dlg_assert(file);
	init();
}

void MP3Decoder::init() {
	// parse frames until we get for the first time.
	// only this way we can know the files channel count and rate
	rbuf_ = std::make_unique<std::uint8_t[]>(readSize);
	mp3dec_frame_info_t fi {};
	int nf = 0u;
	auto maxs = MINIMP3_MAX_SAMPLES_PER_FRAME;
	samples_.resize(maxs);

	while(nf == 0) {
		auto size = readSize - rbufSize_;
		auto n = std::fread(rbuf_.get() + rbufSize_, 1, size, file_);
		if(n == 0) {
			check(file_);
			auto msg = std::string("invalid mp3 file (reading failed)");
			throw std::runtime_error(msg);
		}

		rbufSize_ += n;
		nf = mp3dec_decode_frame(&mp3_, rbuf_.get(), rbufSize_,
			samples_.data(), &fi);
		if(fi.frame_bytes == 0) {
			auto msg = std::string("invalid mp3 file (large invalid chunk)");
			throw msg;
		}

		rbufSize_ -= fi.frame_bytes;
		std::memmove(rbuf_.get(), rbuf_.get() + fi.frame_bytes, rbufSize_);
	}

	channels_ = fi.channels;
	rate_ = fi.hz;
	samplesCount_ = nf * channels_;
}

int MP3Decoder::get(float* buf, unsigned nf) {
	auto ns = nf * channels_;
	auto rem = ns;

	// copy remaining
	auto count = std::min<unsigned>(rem, samplesCount_);
	if(count) {
		std::memcpy(buf, samples_.data() + samplesOff_, count * sizeof(float));
		buf += count;
		samplesOff_ += count;
		samplesCount_ -= count;
		rem -= count;
	}

	mp3dec_frame_info_t fi {};
	while(rem) {
		// fill read buffer
		auto size = readSize - rbufSize_;
		if(size > 0) {
			int n = std::fread(rbuf_.get() + rbufSize_, 1, size, file_);
			if(n == 0) {
				auto c = check(file_);
				if(c < 0) {
					return c;
				}

				auto ret = (ns - rem) / channels_;
				if(ret == 0) {
					std::fseek(file_, 0, SEEK_SET);
				}
				return ret;
			}

			rbufSize_ += n;
		}

		// decode and enqueue frames, if any
		auto nf = mp3dec_decode_frame(&mp3_, rbuf_.get(), rbufSize_,
			samples_.data(), &fi);

		if(nf > 0) {
			dlg_assert((unsigned) fi.channels == channels_);
			dlg_assert((unsigned) fi.hz == rate_);
			auto rns = nf * channels_;
			auto count = std::min<unsigned>(rem, rns);
			std::memcpy(buf, samples_.data(), count * sizeof(float));
			rem -= count;
			buf += count;

			samplesOff_ = count;
			samplesCount_ = rns - count;
		}

		rbufSize_ -= fi.frame_bytes;

		// NOTE: this could hurt performance. We move *all* the remaining
		// data every frame. We could instead make the read buffer 32kb
		// and read-append/only move once the offset is over 16kb
		std::memmove(rbuf_.get(), rbuf_.get() + fi.frame_bytes, rbufSize_);
	}

	return nf;
}

// VorbisDecoder
VorbisDecoder::VorbisDecoder(nytl::StringParam file) {
	int error = 0;
	vorbis_ = stb_vorbis_open_filename(file.c_str(), &error, nullptr);
	if(!vorbis_) {
		auto msg = std::string("StreamVorbisAudio: failed to load ");
		msg += file.c_str();
		msg += ": ";
		msg += std::to_string(error);
		throw std::runtime_error(msg);
	}
}

VorbisDecoder::VorbisDecoder(nytl::Span<const std::byte> buf) {
	int error = 0;
	auto data = (const unsigned char*) buf.data();
	vorbis_ = stb_vorbis_open_memory(data, buf.size(), &error, nullptr);
	if(!vorbis_) {
		auto msg = std::string("StreamVorbisAudio: failed to load from memory");
		throw std::runtime_error(msg);
	}
}

VorbisDecoder::VorbisDecoder(File&& file) {
	int error = 0;
	vorbis_ = stb_vorbis_open_file(file.release(), true, &error, nullptr);
	if(!vorbis_) {
		auto msg = std::string("StreamVorbisAudio: failed to load from memory");
		throw std::runtime_error(msg);
	}
}

VorbisDecoder::~VorbisDecoder() {
	if(vorbis_) {
		stb_vorbis_close(vorbis_);
	}
}

int VorbisDecoder::get(float* buf, unsigned nf) {
	auto info = stb_vorbis_get_info(vorbis_);
	auto read = stb_vorbis_get_samples_float_interleaved(vorbis_,
		info.channels, buf, nf * info.channels);
	return read;
}

unsigned VorbisDecoder::rate() const {
	auto info = stb_vorbis_get_info(vorbis_);
	return info.sample_rate;
}

unsigned VorbisDecoder::channels() const {
	auto info = stb_vorbis_get_info(vorbis_);
	return info.channels;
}

} // namespace tkn
