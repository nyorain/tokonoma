#include <tkn/audio-cubeb.hpp>
#include <cubeb/cubeb.h>
#include <dlg/dlg.hpp>
#include <stdexcept>
#include <thread>

// TODO: remove atomic_unique_ptr.
// Sticking to an RAII destruction smart pointer is nice and
// all but we do all the manging ourselves (using the added
// release_exchange method) so i guess we should just use plain old
// pointers and delete at this point. It's a couple of highly
// specialized lines of code anyways.

namespace tkn::acb {

struct AudioPlayer::Util {
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

	channels_ = output_params.channels;

	uint32_t rate;
	rv = cubeb_get_preferred_sample_rate(cubeb_, &rate);
	if(rv != CUBEB_OK) {
		throw std::runtime_error("Could not get preferred sample rate");
	}

	output_params.rate = rate;
	rate_ = output_params.rate;
	dlg_info("Using rate: {}", rate);

	rv = cubeb_get_min_latency(cubeb_, &output_params, &latencyFrames);
	if(rv != CUBEB_OK) {
		throw std::runtime_error("Could not get minimum latency");
	}

	dlg_info("min latency frames: {}", latencyFrames);

	rv = cubeb_stream_init(cubeb_, &stream_, "tkn::AudioPlayer",
		NULL, NULL, NULL, &output_params,
		latencyFrames, Util::dataCb, Util::stateCb, this);
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

	// start update thread
	run_.store(true);
	updateThread_ = std::thread{[this]{ this->updateThread(); }};
}

AudioPlayer::~AudioPlayer() {
	cubeb_stream_destroy(stream_);
	cubeb_destroy(cubeb_);

	run_.store(false);
	// TODO: this might take a while when the thread is sleeping.
	// We could instead make it wait on a cv everytime it goes
	// to sleep and signal that here
	updateThread_.join();
}

Audio& AudioPlayer::add(std::unique_ptr<Audio> audio) {
	dlg_assert(audio);
	dlg_assert(!audio->next_);
	dlg_assert(!audio->destroyed_.load());

	// Call update the first time since the update thread
	// may only pick it up the next iteration and update
	// is expected to be called before render.
	// Important that we do this *before* the audio is in the list
	// since otherwise it might be called here and by the update
	// thread.
	audio->update(*this);

	auto& ret = *audio;
	audio->next_ = std::move(list_);
	list_.reset(audio.release());
	return ret;
}

std::unique_ptr<Audio> AudioPlayer::unlink(Audio& link, Audio* prev,
		atomic_unique_ptr<Audio>& head) {
	// via release_exchange we prevent that 'link' is destroyed
	// but make sure it's not in a unique ptr anymore
	if(&link == head.get()) {
		head.release_exchange(head->next_.release());
	} else {
		dlg_assert(prev);
		prev->next_.release_exchange(link.next_.release());
	}

	// Then we construct a new unique pointer from the unlinked link
	return std::unique_ptr<Audio>(&link);
}

bool AudioPlayer::remove(Audio& audio) {
	// find 'audio' is 'list_', unlink it there and move
	// it to 'destroyed_'. We furthermore set the 'destroyed_'
	// field of audio to the current render iteration, so we can
	// keep it alive until that iteration ends.
	auto it = list_.get();
	auto prev = decltype(it) {nullptr};
	while(it) {
		if(it == &audio) {
			auto owned = unlink(*it, prev, list_);
			owned->next_.reset(destroyed_.get());
			owned->destroyed_.store(renderIteration_.load());
			destroyed_.release_exchange(owned.release());
			return true;
		}

		prev = it;
		it = it->next_;
	}

	return false;
}

void AudioPlayer::updateThread() {
	using namespace std::chrono;

	constexpr static auto iterationTime =
		duration_cast<Clock::duration>(milliseconds(50));

	while(run_.load()) {
		auto nextIteration = Clock::now() + iterationTime;

		// destroy all audios that we can destroy
		auto c = renderIteration_.load();
		auto it = destroyed_.get();
		auto prev = decltype(it) {nullptr};
		while(it) {
			dlg_assert(it->destroyed_.load());
			if(it->destroyed_ != c) {
				auto nit = it->next_.get();
				unlink(*it, prev, destroyed_); // ignore -> destroyed
				it = nit;
				// prev remains the same
			} else {
				prev = it;
				it = it->next_;
			}
		}

		// upate all audios
		for(auto it = list_.get(); it; it = it->next_.get()) {
			it->update(*this);
		}

		// sleep
		std::this_thread::sleep_until(nextIteration);
	}
}

long AudioPlayer::dataCb(void* vbuffer, long nframes) {
	if(++renderIteration_ == 0) {
		++renderIteration_;
	}

	// clear the buffer first, initial values undefined
	auto buffer = static_cast<float*>(vbuffer);
	std::memset(buffer, 0x0, nframes * channels() * sizeof(float));

	for(auto it = list_.get(); it; it = it->next_.get()) {
		it->render(*this, buffer, nframes);
	}

	return nframes;
}

void AudioPlayer::stateCb(unsigned state) {
	dlg_info("audio player state: {}", state);
}

} // namespace tkn
