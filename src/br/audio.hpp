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

static struct BufCache {
	std::array<std::vector<float>, 2> bufs;
	std::vector<float>& get(unsigned i, std::size_t size) {
		auto& b = bufs[i];
		if(b.size() < size) b.resize(size);
		return b;
	}
} bufCache;

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

static const char* name(IPLerror err) {
	switch(err) {
		case IPL_STATUS_FAILURE: return "failure";
		case IPL_STATUS_OUTOFMEMORY: return "out of memory";
		case IPL_STATUS_INITIALIZATION: return "initialization";
		default: return "<unknown>";
	}
}

#define iplCheck(x) do { \
	auto res = (x); \
	if(res != IPL_STATUS_SUCCESS) { \
		dlg_error("ipl returned {}", name(res)); \
	} \
} while(0)

constexpr IPLAudioFormat stereoFormat() {
	IPLAudioFormat format {};
	format.channelLayoutType = IPL_CHANNELLAYOUTTYPE_SPEAKERS;
	format.channelLayout = IPL_CHANNELLAYOUT_STEREO;
	format.channelOrder = IPL_CHANNELORDER_INTERLEAVED;
	return format;
}

class PhononSetup {
public:
	PhononSetup(const tkn::AudioPlayer& ap) {
		dlg_assert(ap.channels() == 2);
		auto frameRate = ap.rate();

		iplCheck(iplCreateContext(&PhononSetup::log, nullptr,
			nullptr, &context_));

		IPLSimulationSettings envSettings {};
		envSettings.sceneType = IPL_SCENETYPE_PHONON;
		envSettings.numOcclusionSamples = 32;
		envSettings.numDiffuseSamples = 32;
		envSettings.numBounces = 8;
		envSettings.numThreads = 1;
		envSettings.irDuration = 0.5;
		envSettings.maxConvolutionSources = 8u;
		envSettings.irradianceMinDistance = 0.05;

		IPLhandle env;
		iplCheck(iplCreateEnvironment(context_, nullptr, envSettings, nullptr,
			nullptr, &env));

		IPLRenderingSettings settings {};
		settings.samplingRate = frameRate;
		settings.frameSize = tkn::AudioPlayer::blockSize;
		settings.convolutionType = IPL_CONVOLUTIONTYPE_PHONON;
		auto format = stereoFormat();
		iplCheck(iplCreateEnvironmentalRenderer(context_, env, settings,
			format, nullptr, nullptr, &envRenderer_));
	}

	static void log(char* msg) {
		dlg_infot(("phonon"), "phonon: {}", msg);
	}

	IPLhandle context() const { return context_; }
	IPLhandle envRenderer() const { return envRenderer_; }
	IPLhandle environment() const { return iplGetEnvironmentForRenderer(envRenderer_); }

private:
	IPLhandle context_;
	IPLhandle envRenderer_;
};

class DirectSoundEffect {
public:
	DirectSoundEffect(IPLhandle envRenderer) {
		auto format = stereoFormat();
		iplCheck(iplCreateDirectSoundEffect(envRenderer, format, format, &effect_));
	}

	// nb: number blocks
	// in: interleaved stereo in buffer
	// out: interleaved stereo out buffer
	// path: queried direct sound path
	void apply(unsigned nb, float* in, float* out, const IPLDirectSoundPath& path) {
		int bs = tkn::AudioPlayer::blockSize;
		auto format = stereoFormat();
		IPLDirectSoundEffectOptions opts {};
		opts.applyAirAbsorption = IPL_TRUE;
		opts.applyDirectivity = IPL_TRUE;
		opts.applyDistanceAttenuation = IPL_TRUE;
		opts.directOcclusionMode = IPL_DIRECTOCCLUSION_NONE; // TODO
		for(auto i = 0u; i < nb; ++i) {
			IPLAudioBuffer inb{format, bs, ((float*) in + i * bs * 2), nullptr};
			IPLAudioBuffer outb{format, bs, out + i * bs * 2, nullptr};
			iplApplyDirectSoundEffect(effect_, inb, path, opts, outb);
		}
	}

protected:
	IPLhandle effect_;
};

class HRTF {
public:
	HRTF(IPLhandle context, unsigned frameRate) {
		IPLRenderingSettings settings {};
		settings.samplingRate = frameRate;
		settings.frameSize = tkn::AudioPlayer::blockSize;
		settings.convolutionType = IPL_CONVOLUTIONTYPE_PHONON;

		IPLHrtfParams hrtfParams {IPL_HRTFDATABASETYPE_DEFAULT, nullptr, nullptr};
		iplCheck(iplCreateBinauralRenderer(context, settings, hrtfParams, &renderer_));

		auto format = stereoFormat();
    	iplCheck(iplCreateBinauralEffect(renderer_, format, format, &effect_));
	}

	// nb: number blocks
	// in: interleaved stereo in buffer
	// out: interleaved stereo out buffer
	// dir: direction of audio source from listener pov
	void apply(unsigned nb, float* in, float* out, IPLVector3 dir) {
		int bs = tkn::AudioPlayer::blockSize;
		auto format = stereoFormat();
		for(auto i = 0u; i < nb; ++i) {
			IPLAudioBuffer inb{format, bs, ((float*) in + i * bs * 2), nullptr};
			IPLAudioBuffer outb{format, bs, out + i * bs * 2, nullptr};
			iplApplyBinauralEffect(effect_, renderer_, inb, dir,
				IPL_HRTFINTERPOLATION_BILINEAR, outb);
		}
	}

protected:
	IPLhandle renderer_ {};
	IPLhandle effect_ {};
};

/// Streams audio from a vorbis file.
class StreamedVorbisAudio : public tkn::Audio {
public:
	static constexpr IPLVector3 position {0.f, 0.f, 0.f};

	// TODO: shouldn't be stored here
	// and changing it will result in data race.
	IPLVector3 listenerPos {0.f, 0.f, 0.f};
	IPLVector3 listenerDir {0.f, 0.f, 1.f};
	IPLVector3 listenerUp {0.f, 1.f, 0.f};

public:
	StreamedVorbisAudio(nytl::StringParam file, HRTF& hrtf,
			DirectSoundEffect& directEffect, IPLhandle env) {
		env_ = env;
		hrtf_ = &hrtf;
		directEffect_ = &directEffect;

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
		cap -= (cap % tkn::AudioPlayer::blockSize);
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

		auto ns = nf * ap.channels();
		auto& buf1 = bufCache.get(0, ns);

		// read into buf1
		unsigned count = buffer_.dequeue(buf1.data(), ns);
		// TODO: this means we could miss the overhanging streamed samples
		// instead fill up with 0 and then process?
		auto frames = count / 2;
		frames -= frames % tkn::AudioPlayer::blockSize;
		auto nb = frames / tkn::AudioPlayer::blockSize;

		// query the direct sound path
		auto radius = 0.1f;
		IPLSource source {};
		source.position = position;

		// shouldn't matter since it's a monopole, right?
		source.ahead = {0.f, 0.f, 1.f};
		source.up = {0.f, 1.f, 0.f};
		source.right = {1.f, 0.f, 0.f};
		source.directivity.dipoleWeight = 0.0;

		auto path = iplGetDirectSoundPath(env_,
			listenerPos, listenerDir, listenerUp,
			source, radius,
			IPL_DIRECTOCCLUSION_NONE,
			IPL_DIRECTOCCLUSION_RAYCAST);

		// apply first effect: direct sound
		// reads buf1, writes buf2
		auto& buf2 = bufCache.get(1, count);
		directEffect_->apply(nb, buf1.data(), buf2.data(), path);

		// apply second effect: hrtf
		// reads buf2, writes buf1
		hrtf_->apply(nb, buf2.data(), buf1.data(), path.direction);

		// then, mix in with existing audio, reading buf1
		for(auto i = 0u; i < count; ++i) {
			buf[i] += buf1[i];
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

	IPLhandle env_;
	HRTF* hrtf_;
	DirectSoundEffect* directEffect_;

	// TODO: we could probably instead use a ring buffer of owned
	// data blocks, resulting in way less copying. But we then probably
	// need a second (potentially global though) ring buffer to move
	// back ownership of those block buffers to the update thread.
	static constexpr auto bufSize = 48000 * 2;
	tkn::ring_buffer_base<float> buffer_{bufSize};
	bool play_ {true};
	std::atomic<bool> drain_ {}; // when restarting

#ifndef NDEBUG
	bool firstUpdate_ {true};
#endif
};


/*
class AudioEffectHTRF : public tkn::AudioEffect {
public:
	static constexpr auto framesize = 1024u;
	tkn::ring_buffer_base<IPLVector3> updateDir{4};

public:
	AudioEffectHTRF(const tkn::AudioPlayer& ap) {
		dlg_assert(ap.channels() == 2);

		auto rate = ap.rate();

		iplCheck(iplCreateContext(&AudioEffectHTRF::log, nullptr,
			nullptr, &context_));

		IPLRenderingSettings settings {};
		settings.samplingRate = rate;
		settings.frameSize = framesize;
		settings.convolutionType = IPL_CONVOLUTIONTYPE_PHONON;

		IPLHrtfParams hrtfParams {IPL_HRTFDATABASETYPE_DEFAULT, nullptr, nullptr};
		iplCheck(iplCreateBinauralRenderer(context_, settings, hrtfParams, &renderer_));

		format_ = {};
		format_.channelLayoutType = IPL_CHANNELLAYOUTTYPE_SPEAKERS;
		format_.channelLayout = IPL_CHANNELLAYOUT_STEREO;
		format_.channelOrder = IPL_CHANNELORDER_INTERLEAVED;

    	iplCheck(iplCreateBinauralEffect(renderer_, format_, format_, &effect_));

		IPLSimulationSettings envSettings {};
		envSettings.sceneType = IPL_SCENETYPE_PHONON;
		envSettings.numOcclusionSamples = 32;
		envSettings.numDiffuseSamples = 32;
		envSettings.numBounces = 8;
		envSettings.numThreads = 1;
		envSettings.irDuration = 0.5;
		envSettings.maxConvolutionSources = 8u;
		envSettings.irradianceMinDistance = 0.05;
		iplCheck(iplCreateEnvironment(context_, nullptr, envSettings, nullptr,
			nullptr, &environment_));
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

	IPLhandle environment_ {};
};
*/

