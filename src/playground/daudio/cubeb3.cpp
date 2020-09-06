#include <tkn/app.hpp>
#include <ny/event.hpp>
#include <ny/mouseButton.hpp>
#include <ny/key.hpp>
#include <nytl/vecOps.hpp>
#include <cubeb/cubeb.h>
#include <dlg/dlg.hpp>
#include <atomic>

std::atomic<bool> gdrain {false};
constexpr auto numChannels = 2u;
constexpr auto rate = 44100;

long dataCb(cubeb_stream*, void*, const void* inBuf, void*, long nframes) {
	if(gdrain.load()) {
		gdrain.store(false);
		return 0;
	}


	const float* buf = static_cast<const float*>(inBuf);
	double avg = 0.0;
	for(auto i = 0u; i < nframes; ++i) {
		for(auto c = 0u; c < numChannels; ++c) {
			avg += buf[i * numChannels + c];
		}
	}

	dlg_info("record avg of {} frames: {}", nframes, avg / (nframes * numChannels));
	return nframes;
}

void stateCb(cubeb_stream*, void* user, cubeb_state state) {
	dlg_info("state changed to {}", state);
}

void log(const char* fmt, ...) {
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

class DummyAudioApp : public tkn::App {
public:
	~DummyAudioApp() {
		dlg_trace("~DummyAudioApp");
		if(stream_) {
			dlg_trace("stopping stream...");
			cubeb_stream_stop(stream_);
			dlg_trace("destroying stream...");
			cubeb_stream_destroy(stream_);
		}
		if(cubeb_) {
			dlg_trace("destroying cubeb backend...");
			cubeb_destroy(cubeb_);
		}
	}

	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		int rv = cubeb_set_log_callback(CUBEB_LOG_NORMAL, log);
		rv = cubeb_init(&cubeb_, "tkn: daudio", NULL);
		if(rv) {
			dlg_error("Failed to initialize cubeb");
			return false;
		}

		auto bid = cubeb_get_backend_id(cubeb_);
		dlg_assert(bid);
		dlg_info("cubeb backend: {}", bid);

		cubeb_stream_params input_params;
		input_params.format = CUBEB_SAMPLE_FLOAT32NE;
		input_params.channels = numChannels;
		input_params.layout = CUBEB_LAYOUT_UNDEFINED;
		input_params.prefs = CUBEB_STREAM_PREF_NONE;
		input_params.rate = rate;

		uint32_t latencyFrames = 0;
		rv = cubeb_get_min_latency(cubeb_, &input_params, &latencyFrames);
		if(rv != CUBEB_OK) {
			dlg_warn("Could not get minimum latency");
		} else {
			dlg_info("min latency frames: {}", latencyFrames);
		}

		latencyFrames = std::max<unsigned>(latencyFrames, 1024);
		dlg_info("using latency: {}", latencyFrames);

		rv = cubeb_stream_init(cubeb_, &stream_, "tkn::AudioPlayer",
			NULL, &input_params,
			NULL, NULL,
			latencyFrames, dataCb, stateCb, NULL);
		if(rv != CUBEB_OK) {
			dlg_error("Could not open audio stream");
			return false;
		}

		rv = cubeb_stream_get_latency(stream_, &latencyFrames);
		if(rv == CUBEB_OK) {
			dlg_info("Audio stream latency frames: {}", latencyFrames);
		} else {
			dlg_warn("Could not get audio stream latency");
		}

		rv = cubeb_stream_start(stream_);
		if(rv != CUBEB_OK) {
			dlg_error("Could not start stream");
			return false;
		}

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		((void) cb);
	}

	void update(double dt) override {
		App::update(dt);
		App::scheduleRedraw();
	}

	void clickat(nytl::Vec2f pos) {
		using namespace nytl::vec::cw::operators;
		auto rp = pos / window().size();
		dlg_info("clickat {}", rp);
		if(rp.x > 0.8 && rp.y > 0.9) {
			cubeb_stream_start(stream_);
		} else if(rp.x < 0.2 && rp.y > 0.8) {
			cubeb_stream_stop(stream_);
	 	} else if(rp.x < 0.2 && rp.y < 0.1) {
			gdrain.store(true);
		}
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.pressed && ev.button == ny::MouseButton::left) {
			clickat(nytl::Vec2f(ev.position));
		}
		return true;
	}

	bool touchBegin(const ny::TouchBeginEvent& ev) override {
		if(App::touchBegin(ev)) {
			return true;
		}

		clickat(ev.pos);
		return true;
	}

	const char* name() const override { return "dummy-audio"; }

protected:
	cubeb* cubeb_ {};
	cubeb_stream* stream_ {};
};

int main(int argc, const char** argv) {
	DummyAudioApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
