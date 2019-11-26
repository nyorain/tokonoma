#include <tkn/audio.hpp>
#include <cubeb/cubeb.h>
#include <dlg/dlg.hpp>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace tkn {

struct AudioPlayer::Util {
	static long dataCb(cubeb_stream*, void* user, const void* inBuf,
			void* outBuf, long nsamples) {
		(void) inBuf;
		AudioPlayer* player = static_cast<AudioPlayer*>(user);
		return player->dataCb(outBuf, nsamples);
	}

	static void stateCb(cubeb_stream*, void* user, cubeb_state state) {
		AudioPlayer* player = static_cast<AudioPlayer*>(user);
		player->stateCb(state);
	}
};

AudioPlayer::AudioPlayer(const char* name) {
	cubeb_init(&cubeb_, name, NULL);
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
	latencyFrames = std::max<unsigned>(latencyFrames, 4096u);

	rv = cubeb_stream_init(cubeb_, &stream_, "tkn::AudioPlayer",
		NULL, NULL, NULL, &output_params,
		latencyFrames, Util::dataCb, Util::stateCb, this);
	if(rv != CUBEB_OK) {
		throw std::runtime_error("Could not open audio stream");
	}

	// NOTE(optimization): we could only start the stream when
	// audios are added and stop it when there are no audios.
	// Not so easy to implement though, we should still destroy
	// audios in destroyed_.
	// And then we should probably also add `bool Audio::active() const`
	// that returns whether the audio is currently active or e.g.
	// paused/stopped and only have the stream playing if we have
	// and active audio.
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

	// destroy remaning active audios
	// no thread is accessing them anymore
	auto it = audios_.load();
	while(it) {
		auto next = it->next_.load();
		delete it;
		it = next;
	}
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
	audio->next_ = audios_.load();
	audios_.store(audio.release());

	return ret;
}

void AudioPlayer::effect(std::unique_ptr<AudioEffect> effect) {
	auto old = effect_.load();
	auto next = effect.release(); // we take manual ownership
	while(!std::atomic_compare_exchange_weak(&effect_, &old, next));
	if(old) delete old; // owned pointer
}

void AudioPlayer::unlink(Audio& link, Audio* prev, std::atomic<Audio*>& head) {
	if(&link == head.load()) {
		head.store(link.next_.load());
	} else {
		dlg_assert(prev);
		prev->next_.store(link.next_.load());
	}
}

bool AudioPlayer::remove(Audio& audio) {
	// find 'audio' is 'audios_', unlink it there and move
	// it to 'destroyed_'. We furthermore set the 'destroyed_'
	// field of audio to the current render iteration, so we can
	// keep it alive until that iteration ends.
	//
	// If the update or render thread are currently processing 'audio',
	// they will still be able to normally continue iteration since
	// - the object is kept alive until no one needs it anymore
	// - audio.next_ will stil point into the list of active audios,
	//   unless audio.next_ is removed as well. But then audio.next_->next_
	//   will point into the list of active audios again (and so on).
	// This means, even after remove(audio) was called, audio.update()
	// and audio.render() might still be called from other threads.
	// But in the next iteration of the update thread or the
	// next render callback, it won't be for certain.
	// And then it can be destroyed.
	auto it = audios_.load();
	auto prev = decltype(it) {nullptr};
	while(it) {
		if(it == &audio) {
			unlink(*it, prev, audios_);
			it->destroyed_.store(renderIteration_.load());

			std::lock_guard lock(dmutex_);
			destroyed_.push_back(std::unique_ptr<Audio>(it));
			return true;
		}

		prev = it;
		it = it->next_;
	}

	return false;
}

void AudioPlayer::updateThread() {
	using namespace std::chrono;
	using Clock = high_resolution_clock;

	// NOTE: we could allow Audio implementations to return when
	// they need it sooner or later.
	constexpr static auto iterationTime =
		duration_cast<Clock::duration>(milliseconds(50));

	while(run_.load()) {
		auto nextIteration = Clock::now() + iterationTime;

		// destroy all audios that we can destroy
		{
			auto c = renderIteration_.load();
			std::lock_guard lock(dmutex_);
			for(auto it = destroyed_.begin(); it < destroyed_.end();) {
				auto& audio = **it;
				dlg_assert(audio.destroyed_.load());
				// Using '<' as comparison makes more sense logically but
				// like this, we can also work with iteration count wrapping.
				// And otherwise, the render iteration count is monotonically
				// increasing anyways.
				if(audio.destroyed_ != c) {
					it = destroyed_.erase(it);
				} else {
					++it;
				}
			}
		}

		// upate all active audios
		for(auto it = audios_.load(); it; it = it->next_.load()) {
			it->update(*this);
		}

		// sleep
		std::this_thread::sleep_until(nextIteration);
	}
}

template<typename A, typename B>
constexpr auto align(A offset, B alignment) {
	if(offset == 0 || alignment == 0) {
		return offset;
	}

	auto rest = offset % alignment;
	return rest ? A(offset + (alignment - rest)) : A(offset);
}

long AudioPlayer::dataCb(void* vbuffer, long nsamples) {
	auto buffer = static_cast<float*>(vbuffer);
	if(++renderIteration_ == 0) {
		++renderIteration_;
	}

	// check if there is an active effect
	// if so, we move it to this thread, so it won't be deleted.
	// We will check later if it was changed in the meantime
	AudioEffect* effect = effect_.load();
	while(!std::atomic_compare_exchange_weak(&effect_, &effect, nullptr));

	if(!effect) {
		// clear the render buf first, initial values undefined
		// if we use renderBuf_ they still contain the old values.
		// memset 0 works for float
		std::memset(buffer, 0x0, nsamples * channels() * sizeof(float));
		for(auto it = audios_.load(); it; it = it->next_.load()) {
			it->render(*this, buffer, nsamples);
		}

		return nsamples;
	}

	// first, copy the samples we already have
	if(left_ > nsamples) { // may be enough in certain cases
		memcpy(buffer, renderBufPost_.data() + leftOff_ * channels(),
			nsamples * channels() * sizeof(float));
		leftOff_ += nsamples;
		left_ -= nsamples;
	} else {
		if(left_ > 0) {
			memcpy(buffer, renderBufPost_.data() + leftOff_ * channels(),
				left_ * channels() * sizeof(float));
		}

		// create new samples
		auto fs = 1024u; // frame size
		auto neededSamples = nsamples - left_;
		auto renderSamples = align(neededSamples, fs);

		std::size_t needed = channels() * renderSamples;
		if(renderBuf_.size() < needed) renderBuf_.resize(needed);
		if(renderBufPost_.size() < needed) renderBufPost_.resize(needed);

		std::memset(renderBuf_.data(), 0x0,
			renderSamples * channels() * sizeof(float));
		for(auto it = audios_.load(); it; it = it->next_.load()) {
			it->render(*this, renderBuf_.data(), renderSamples);
		}

		// apply effect
		effect->apply(rate(), channels(), renderSamples,
			renderBuf_.data(), renderBufPost_.data());

		// output remaining samples
		memcpy(buffer + left_ * channels(),
			renderBufPost_.data(),
			sizeof(float) * channels() * neededSamples);
		leftOff_ = neededSamples;
		left_ = renderSamples - neededSamples;
	}

	// write back the effect
	// but if it was changed in the meantime, we have to delete the old
	// effect instead of writing it back, it's not needed anymore.
	// It's important that we use a strong compare_exchange here
	AudioEffect* neffect = nullptr;
	if(!std::atomic_compare_exchange_strong(&effect_, &neffect, effect)) {
		delete effect;
	}

	return nsamples;
}

void AudioPlayer::stateCb(unsigned state) {
	dlg_info("audio player state: {}", state);
}

} // namespace tkn
