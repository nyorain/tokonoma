#include <tkn/audio.hpp>
#include <cubeb/cubeb.h>
#include <dlg/dlg.hpp>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace tkn {

struct AudioPlayer::Util {
	static long dataCb(cubeb_stream*, void* user, const void* inBuf,
			void* outBuf, long nframes) {
		(void) inBuf;
		try {
			AudioPlayer* player = static_cast<AudioPlayer*>(user);
			return player->dataCb(outBuf, nframes);
		} catch(const std::exception& err) {
			dlg_error("caught exception in audio callback: {}", err.what());
		} catch(...) {
			dlg_error("caught exception in audio callback");
		}

		std::fflush(stdout);
		return 0; // drain
	}

	static void stateCb(cubeb_stream*, void* user, cubeb_state state) {
		AudioPlayer* player = static_cast<AudioPlayer*>(user);
		player->stateCb(state);
	}

	static void log(const char* fmt, ...) {
		va_list vlist;
		va_start(vlist, fmt);

		va_list vlistcopy;
		va_copy(vlistcopy, vlist);
		int needed = vsnprintf(NULL, 0, fmt, vlist);
		if(needed < 0) {
			dlg_error("cubeb log: invalid format given\n");
			va_end(vlist);
			va_end(vlistcopy);
			return;
		}

		auto buf = std::make_unique<char[]>(needed + 1);
		std::vsnprintf(buf.get(), needed + 1, fmt, vlistcopy);
		va_end(vlistcopy);

		// strip newline if there is one
		auto nl = std::strchr(buf.get(), '\n');
		auto len = needed;
		if(nl) {
			len = nl - buf.get();
		}

		std::string_view msg(buf.get(), len);
		dlg_debugt(("cubeb"), "{}", msg);
	}
};

AudioPlayer::AudioPlayer(const char* name, unsigned rate, unsigned channels,
		unsigned latencyBlocks) {
	int rv = cubeb_set_log_callback(CUBEB_LOG_VERBOSE, Util::log);

	rv = cubeb_init(&cubeb_, name, NULL);
	if(rv) {
		dlg_error("Failed to initialize cubeb");
		throw std::runtime_error("Failed to initialize cubeb");
	}

	auto bid = cubeb_get_backend_id(cubeb_);
	dlg_assert(bid);
	dlg_info("cubeb backend: {}", bid);

	cubeb_stream_params output_params;
	output_params.format = CUBEB_SAMPLE_FLOAT32NE;
	output_params.channels = channels;
	output_params.layout = CUBEB_LAYOUT_UNDEFINED;
	output_params.prefs = CUBEB_STREAM_PREF_NONE;

	channels_ = output_params.channels;

	if(rate == 0) {
		uint32_t prate;
		rv = cubeb_get_preferred_sample_rate(cubeb_, &prate);
		if(rv != CUBEB_OK) {
			dlg_warn("Could not get preferred sample rate");
			rate = 44100;
		} else {
			rate = prate;
		}
	}

	output_params.rate = rate;
	rate_ = output_params.rate;
	dlg_info("Using rate: {}", rate);

	uint32_t latencyFrames = 0;
	rv = cubeb_get_min_latency(cubeb_, &output_params, &latencyFrames);
	if(rv != CUBEB_OK) {
		dlg_warn("Could not get minimum latency");
	} else {
		dlg_info("min latency frames: {}", latencyFrames);
	}

	latencyFrames = std::max<unsigned>(latencyFrames, latencyBlocks * blockSize);
	dlg_info("using latency: {}", latencyFrames);

	rv = cubeb_stream_init(cubeb_, &stream_, "tkn::AudioPlayer",
		NULL, NULL, NULL, &output_params,
		latencyFrames, Util::dataCb, Util::stateCb, this);
	if(rv != CUBEB_OK) {
		dlg_error("Could not open audio stream");
		throw std::runtime_error("Could not open audio stream");
	}

	rv = cubeb_stream_get_latency(stream_, &latencyFrames);
	if(rv == CUBEB_OK) {
		dlg_info("Audio stream latency frames: {}", latencyFrames);
	} else {
		dlg_warn("Could not get audio stream latency");
	}
}

AudioPlayer::~AudioPlayer() {
	cubeb_stream_stop(stream_);
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
		auto next = it->next.load();
		delete it;
		it = next;
	}
}

void AudioPlayer::start() {
	// since the first iteration of rendering can sometimes take
	// longer, we could already execute it here, i.e. fill renderBuf_
	// fill(blockSize * 2);

	// NOTE(optimization): we could only start the stream when
	// audios are added and stop it when there are no audios.
	// Not so easy to implement though, we should still destroy
	// audios in destroyed_.
	// And then we should probably also add `bool Audio::active() const`
	// that returns whether the audio is currently active or e.g.
	// paused/stopped and only have the stream playing if we have
	// and active audio.
	int rv = cubeb_stream_start(stream_);
	if(rv != CUBEB_OK) {
		dlg_error("Could not start stream");
		throw std::runtime_error("Could not start stream");
	}

	// start update thread
	run_.store(true);
	updateThread_ = std::thread{[this]{
		try {
			this->updateThread();
		} catch(const std::exception& err) {
			dlg_error("caught exception in update thread: {}", err.what());
		} catch(...) {
			dlg_error("caught exception in update thread");
		}
	}};

	// cubeb requires to first explicitly unset the logger
	cubeb_set_log_callback(CUBEB_LOG_DISABLED, nullptr);
	cubeb_set_log_callback(CUBEB_LOG_NORMAL, Util::log);
}

AudioSource& AudioPlayer::add(std::unique_ptr<AudioSource> audio) {
	dlg_assert(audio);
	// dlg_assert(!audio->next_);
	// dlg_assert(!audio->destroyed_.load());

	// Call update the first time since the update thread
	// may only pick it up the next iteration and update
	// is expected to be called before render.
	// Important that we do this *before* the audio is in the list
	// since otherwise it might be called here and by the update
	// thread.
	audio->update();

	auto& ret = *audio;
	auto na = new Audio;
	na->source = std::move(audio);
	na->next.store(audios_.load());
	audios_.store(na);

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
		head.store(link.next.load());
	} else {
		dlg_assert(prev);
		prev->next.store(link.next.load());
	}
}

bool AudioPlayer::remove(AudioSource& audio) {
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
		if(it->source.get() == &audio) {
			unlink(*it, prev, audios_);
			it->destroyed.store(renderIteration_.load());

			std::lock_guard lock(dmutex_);
			destroyed_.push_back(std::unique_ptr<Audio>(it));
			return true;
		}

		prev = it;
		it = it->next;
	}

	return false;
}

void AudioPlayer::updateThread() {
	using namespace std::chrono;
	using Clock = high_resolution_clock;

	// NOTE: we could allow Audio implementations to return when
	// they need it sooner or later. Not sure if good design though
	// since they can't depend on it anyways
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
				dlg_assert(audio.destroyed.load());
				// Using '<' as comparison makes more sense logically but
				// like this, we can also work with iteration count wrapping.
				// And otherwise, the render iteration count is monotonically
				// increasing anyways.
				if(audio.destroyed != c) {
					it = destroyed_.erase(it);
				} else {
					++it;
				}
			}
		}

		// upate all active audios
		for(auto it = audios_.load(); it; it = it->next.load()) {
			it->source->update();
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

void AudioPlayer::fill(unsigned nf) {
	renderUpdate();

	// check if there is an active effect
	// if so, we move it to this thread, so it won't be deleted.
	// We will check later if it was changed in the meantime
	AudioEffect* effect = effect_.load();
	AudioEffect* en = nullptr;
	while(!std::atomic_compare_exchange_weak(&effect_, &effect, en));

	auto renderFrames = align(nf, blockSize);
	auto nb = renderFrames / blockSize;

	std::size_t needed = channels() * renderFrames;
	if(renderBuf_.size() < needed) {
		renderBuf_.resize(needed);
	}

	auto* renderBuf = renderBuf_.data();
	if(effect) {
		if(renderBufTmp_.size() < needed) {
			renderBufTmp_.resize(needed);
		}
		renderBuf = renderBufTmp_.data();
	}

	std::memset(renderBuf, 0x0, renderFrames * channels() * sizeof(float));
	for(auto it = audios_.load(); it; it = it->next.load()) {
		it->source->render(nb, renderBuf, true);
	}

	// apply effect
	if(effect) {
		effect->apply(rate(), channels(), renderFrames,
			renderBufTmp_.data(), renderBuf_.data());

		// write back the effect
		// but if it was changed in the meantime, we have to delete the old
		// effect instead of writing it back, it's not needed anymore.
		// It's important that we use a strong compare_exchange here
		AudioEffect* neffect = nullptr;
		if(!std::atomic_compare_exchange_strong(&effect_, &neffect, effect)) {
			delete effect;
		}
	}

	leftOff_ = 0;
	left_ = renderFrames;
}

long AudioPlayer::dataCb(void* vbuffer, long nframes) {
	// using namespace std::chrono;
	// using Clock = high_resolution_clock;
	// auto start = Clock::now();

	// artifical delay for testing slow cpus
	// std::this_thread::sleep_for(milliseconds(5));

	auto buffer = static_cast<float*>(vbuffer);
	if(++renderIteration_ == 0) {
		++renderIteration_;
	}

	// first, copy the samples we already have
	if(left_ >= nframes) { // may be enough in certain cases
		memcpy(buffer, renderBuf_.data() + leftOff_ * channels(),
			nframes * channels() * sizeof(float));
		leftOff_ += nframes;
		left_ -= nframes;
	} else {
		memcpy(buffer, renderBuf_.data() + leftOff_ * channels(),
			left_ * channels() * sizeof(float));

		// in this case we need additional frames.
		// Make sure we fill renderBuf_ with enough samples
		auto neededFrames = nframes - left_;
		auto left = left_;
		fill(neededFrames);

		// output remaining samples
		memcpy(buffer + left * channels(), renderBuf_.data(),
			sizeof(float) * channels() * neededFrames);
		leftOff_ += neededFrames;
		left_ -= neededFrames;
	}

	// we do this here again so audios that were removed while we were
	// rendering can immediately be removed and don't have to wait for
	// our next iteration
	if(++renderIteration_ == 0) {
		++renderIteration_;
	}

	// TODO: buffer and log from another thread!
	// cubeb already implements that
	// auto dur = Clock::now() - start;
	// dlg_info("Needed {} ms for audio rendering",
	// 	duration_cast<microseconds>(dur).count() / 1000.f);

	// NOTE: debugging: output number of non-zero samples
	// useful in certain scenarios to find buffers with invalid data
	// auto c = 0u;
	// for(auto i = 0u; i < channels() * nframes; ++i) {
	// 	if(buffer[i] != 0.f) {
	// 		++c;
	// 	}
	// }
	// dlg_debug("{} c non-zero samples", c);

	return nframes;
}

void AudioPlayer::stateCb(unsigned state) {
	dlg_info("audio player state: {}", state);
}

} // namespace tkn
