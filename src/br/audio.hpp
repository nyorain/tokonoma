#include <tkn/audio.hpp>
#include <tkn/ringbuffer.hpp>
#include <dlg/dlg.hpp>
#include <nytl/stringParam.hpp>
#include <speex_resampler.h>
#include <phonon.h>

// defines some really shitty macros
// upstream pr to undefine them at the end?
#include <stb_vorbis.h>
#undef R
#undef L
#undef C

#define TSF_IMPLEMENTATION
#include <tsf.h>

#define TML_IMPLEMENTATION
#include <tml.h>

class MidiAudio : public tkn::Audio {
public:
	MidiAudio() {
		tsf_ = tsf_load_filename("piano.sf2");
		if(!tsf_) {
			std::string err = "Could not load soundfont. "
				"Expected in build dir, path currently hard-coded";
			throw std::runtime_error(err);
		}

		messages_ = tml_load_filename(TKN_BASE_DIR "/assets/audio/ibi/Some Sand.mid");
		if(!messages_) {
			throw std::runtime_error("Failed to load midi file");
		}

		current_ = messages_;
	}

	~MidiAudio() {
		if(tsf_) tsf_close(tsf_);
		if(messages_) tml_free(messages_);
	}

	void handle(const tml_message& msg) {
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

	void render(const tkn::AudioPlayer& ap, float* buf, unsigned nf) override {
		while(current_ && current_->time <= time_) {
			handle(*current_);
			current_ = current_->next;

			if(!current_) { // loop
				time_ = 0.f;
				current_ = messages_;
				break;
			}
		}

		dlg_assert(ap.channels() == 2);
		tsf_set_output(tsf_, TSF_STEREO_INTERLEAVED, ap.rate(), 0);
		tsf_render_float(tsf_, buf, nf, 1);

		time_ += 1000 * (double(nf) / ap.rate());
	}

private:
	tsf* tsf_;
	tml_message* messages_;
	tml_message* current_;
	double time_ {}; // relative, in milliseconds
};

struct UniqueSoundBuffer {
	std::size_t frameCount;
	unsigned channelCount;
	unsigned rate; // in Hz
	std::unique_ptr<float[]> data; // interleaved
};

struct SoundBufferView {
	std::size_t frameCount;
	unsigned channelCount;
	unsigned rate; // in Hz
	float* data; // interleaved

	SoundBufferView() = default;
	SoundBufferView(UniqueSoundBuffer& rhs) :
		frameCount(rhs.frameCount),
		channelCount(rhs.channelCount),
		rate(rhs.rate),
		data(rhs.data.get()) {}
};


void resample(SoundBufferView dst, SoundBufferView src) {
	if(dst.rate == src.rate) {
		dlg_info("resampling not needed");
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

		return;
	}

	int err;
	auto speex = speex_resampler_init(src.channelCount,
		src.rate, dst.rate, SPEEX_RESAMPLER_QUALITY_DESKTOP, &err);
	dlg_assert(speex);

	speex_resampler_set_input_stride(speex, src.channelCount);
	speex_resampler_set_output_stride(speex, dst.channelCount);
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
		dlg_assertm(in_len == src.frameCount, "{} vs {}",
			in_len, src.frameCount);
		dlg_assertm(out_len == dst.frameCount, "{} vs {}",
			out_len, dst.frameCount);
	}

	dlg_info("resampling done");
	speex_resampler_destroy(speex);
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


/// Streams audio from a vorbis file.
class StreamedVorbisAudio : public tkn::Audio {
public:
	StreamedVorbisAudio(nytl::StringParam file) {
		int error = 0;
		vorbis_ = stb_vorbis_open_filename(file.c_str(), &error, nullptr);
		if(!vorbis_) {
			auto msg = std::string("StreamVorbisAudio: failed to load ");
			msg += file.c_str();
			throw std::runtime_error(msg);
		}
	}

	~StreamedVorbisAudio() {
		if(vorbis_) {
			stb_vorbis_close(vorbis_);
		}
	}

	// TODO: cache data buffers, don't allocate every time
	void update(const tkn::AudioPlayer& ap) override {
		if(!play_) {
			return;
		}

		auto info = stb_vorbis_get_info(vorbis_);
		auto cap = buffer_.available_write();
		if(cap < 4096) { // not worth it, still full enough
			dlg_info("Skipping update");
			return;
		}

		std::unique_ptr<float[]> data = std::make_unique<float[]>(cap);
		auto ns = stb_vorbis_get_samples_float_interleaved(vorbis_,
			info.channels, data.get(), cap);
		if(ns == 0) {
			dlg_info("finished stream");
			play_ = false;
			return;
		}

		unsigned c = info.channels;
		if(c != ap.channels() || info.sample_rate != ap.rate()) {
			SoundBufferView view;
			view.channelCount = info.channels;
			view.frameCount = ns;
			view.rate = info.sample_rate;
			view.data = data.get();
			auto dst = resample(view, ap.rate(), ap.channels());
			buffer_.enqueue(dst.data.get(), dst.channelCount * dst.frameCount);
		} else {
			buffer_.enqueue(data.get(), ns * c);
		}

#ifndef NDEBUG
		// workaround for the threadid debug-check in our ring buffer.
		// update is called by the main thread first when the audio is
		// added (making this the producer) and (strictly) afterwards
		// only by the update thread. This is valid, the AudioPlayer
		// manages the switch of the producer switch but it defies
		// the debug check logic in the threadbuffer
		if(firstUpdate_) {
			firstUpdate_ = false;
			buffer_.reset_thread_ids();
		}
#endif
	}

	void render(const tkn::AudioPlayer& ap, float* buf, unsigned nf) override {
		if(!play_) {
			return;
		}

		if(drain_.load()) {
			// empty it
			buffer_.dequeue(nullptr, buffer_.available_read());
			drain_.store(false);
			return;
		}

		// there are multiple cases where this might not be the
		// case and it's alright. When draining (restarting)
		// or when the stream has finished.
		// dlg_assert(nf <= (unsigned) buffer_.available_read());

		unsigned count = buffer_.dequeue(rbuf_.get(), nf * ap.channels());
		for(auto i = 0u; i < count; ++i) {
			buf[i] += rbuf_[i];
		}
	}

	void toggle() {
		play_ = !play_;
	}

	void restart() {
		stb_vorbis_seek_start(vorbis_);
		drain_.store(true);
	}

protected:
	stb_vorbis* vorbis_ {};

	static constexpr auto bufSize = 48000 * 2;
	tkn::ring_buffer_base<float> buffer_{bufSize};
	std::unique_ptr<float[]> rbuf_ =
		std::make_unique<float[]>(bufSize);
	bool play_ {true};
	std::atomic<bool> drain_ {}; // when restarting

#ifndef NDEBUG
	bool firstUpdate_ {true};
#endif
};

class AudioEffectHTRF : public tkn::AudioEffect {
public:
	static constexpr auto framesize = 1024u;
	tkn::ring_buffer_base<IPLVector3> updateDir{4};

public:
	AudioEffectHTRF(const tkn::AudioPlayer& ap) {
		dlg_assert(ap.channels() == 2);

		auto rate = ap.rate();

		iplCreateContext(&AudioEffectHTRF::log, nullptr, nullptr, &context_);

		IPLRenderingSettings settings {};
		settings.samplingRate = rate;
		settings.frameSize = framesize;
		settings.convolutionType = IPL_CONVOLUTIONTYPE_PHONON;

		IPLHrtfParams hrtfParams {IPL_HRTFDATABASETYPE_DEFAULT, nullptr, nullptr};
		iplCreateBinauralRenderer(context_, settings, hrtfParams, &renderer_);

		format_ = {};
		format_.channelLayoutType = IPL_CHANNELLAYOUTTYPE_SPEAKERS;
		format_.channelLayout = IPL_CHANNELLAYOUT_STEREO;
		format_.channelOrder = IPL_CHANNELORDER_INTERLEAVED;

    	iplCreateBinauralEffect(renderer_, format_, format_, &effect_);
	}

	~AudioEffectHTRF() {
		if(effect_) iplDestroyBinauralEffect(&effect_);
		if(renderer_) iplDestroyBinauralRenderer(&renderer_);
		if(context_) iplDestroyContext(&context_);
		iplCleanup();
	}

	void apply(unsigned, unsigned, unsigned ns,
			const float* in, float* out) override {
		// TODO
		// auto size = ns * 2 * sizeof(float);
		// memcpy(out, in, size);
		while(updateDir.dequeue(&direction_, 1));

		dlg_assert(ns % framesize == 0);
		auto count = ns / framesize;
		for(auto i = 0u; i < count; ++i) {
			IPLAudioBuffer inb{format_, int(framesize), ((float*) in + i * framesize * 2), nullptr};
			IPLAudioBuffer outb{format_, int(framesize), out + i * framesize * 2, nullptr};
			iplApplyBinauralEffect(effect_, renderer_, inb, direction_,
					IPL_HRTFINTERPOLATION_BILINEAR, outb);
		}

		// IPLAudioBuffer inb{format_, int(ns), (float*) in, nullptr};
    	// IPLAudioBuffer outb{format_, int(ns), (float*) out, nullptr};
		// iplApplyBinauralEffect(effect_, renderer_, inb, direction_,
		// 		IPL_HRTFINTERPOLATION_NEAREST, outb);
	}

	static void log(char* msg) {
		dlg_infot(("phonon"), "phonon: {}", msg);
	}

private:
	IPLhandle context_ {};
	IPLhandle renderer_ {};
	IPLhandle effect_ {};
	IPLAudioFormat format_ {};
	IPLVector3 direction_ {0.f, 0.f, 1.f};
};

