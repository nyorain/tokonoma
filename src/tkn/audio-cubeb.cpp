#include <tkn/audio-cubeb.hpp>
#include <cubeb/cubeb.h>
#include <dlg/dlg.hpp>
#include <stdexcept>

namespace tkn::acb {

struct AudioPlayer::CCB {
	static long dataCb(cubeb_stream*, void* user, const void* inBuf,
			void* outBuf, long nframes) {
		(void) inBuf;
		AudioPlayer* player = static_cast<AudioPlayer*>(user);
		return player->dataCb(outBuf, nframes);
	}

	static void stateCb(cubeb_stream*, void* user, cubeb_state state) {
		AudioPlayer* player = static_cast<AudioPlayer*>(user);
		player->stateCb(state);
	}
};

struct AudioPlayer::AudioElem {
	std::unique_ptr<Audio> audio;
	std::unique_ptr<AudioElem> next;
	AudioElem* prev;
};

AudioPlayer::AudioPlayer() {
	cubeb_init(&cubeb_, "Example Application", NULL);
	int rv;
	uint32_t latencyFrames;

	dlg_info("cubeb backend: {}", cubeb_get_backend_id(cubeb_));

	cubeb_stream_params output_params;
	output_params.format = CUBEB_SAMPLE_FLOAT32NE;
	output_params.channels = 2;
	output_params.layout = CUBEB_LAYOUT_UNDEFINED;
	output_params.prefs = CUBEB_STREAM_PREF_NONE;

	uint32_t rate;
	rv = cubeb_get_preferred_sample_rate(cubeb_, &rate);
	if(rv != CUBEB_OK) {
		throw std::runtime_error("Could not get preferred sample rate");
	}

	output_params.rate = rate;
	printf("reate: %d\n", rate);

	rv = cubeb_get_min_latency(cubeb_, &output_params, &latencyFrames);
	if(rv != CUBEB_OK) {
		throw std::runtime_error("Could not get minimum latency");
	}

	dlg_info("min latency frames: {}", latencyFrames);

	rv = cubeb_stream_init(cubeb_, &stream_, "tkn::AudioPlayer",
		NULL, NULL, NULL, &output_params,
		latencyFrames, CCB::dataCb, CCB::stateCb, this);
	if(rv != CUBEB_OK) {
		throw std::runtime_error("Could not open audio stream");
	}

	rv = cubeb_stream_start(stream_);
	if(rv != CUBEB_OK) {
		throw std::runtime_error("Could not start stream");
	}

	rv = cubeb_stream_get_latency(stream_, &latencyFrames);
	if(rv == CUBEB_OK) {
		dlg_info("Audio stream latency frames: {}", latencyFrames);
	} else {
		dlg_warn("Could not get audio stream latency");
	}
}

AudioPlayer::~AudioPlayer() {
	cubeb_stream_destroy(stream_);
	cubeb_destroy(cubeb_);
}

Audio& AudioPlayer::add(std::unique_ptr<Audio> audio) {
	dlg_assert(audio);

	std::unique_lock lock(mutex_);
	dlg_assert(!audio->next && !audio->prev);
	if(audios_) {
		dlg_assert(!audios_->prev);
		audios_->prev = audio.get();
	}

	audio->next = std::move(audios_);
	audios_ = std::move(audio);
	return *audios_;
}

bool AudioPlayer::remove(Audio& audio) {
	// we don't need to lock the mutex here since we only read
	// and no other thread writes to audios_ anyways
	auto ita = audios_.get();
	while(ita) {
		if(ita == &audio) {
			break;
		}
		ita = ita->next.get();
	}

	if(!ita) { // not found
		return false;
	}

	// remove from linked list
	std::unique_lock lock(mutex_);
	if(ita->next) {
		ita->next->prev = ita->prev;
	}

	auto up = ita->prev ? std::move(ita->prev->next) : std::move(audios_);

	// advance iterator if active
	if(renderCurrent_ == ita) {
		renderCurrent_ = ita->next.get();
		keepRender_ = std::move(up);
	}
	if(updateCurrent_ == ita) {
		updateCurrent_ = ita->next.get();
		keepUpdate_ = std::move(up);
	}

	// this will implicitly delete it
	if(ita->prev) {
		ita->prev->next = std::move(ita->next);
	} else {
		dlg_assert(audios_.get() == ita);
		audios_ = std::move(ita->next);
	}

	return true;
}

void AudioPlayer::updateThread() {
	std::shared_lock lock(mutex_);
}

} // namespace tkn
