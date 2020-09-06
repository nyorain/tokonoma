#include <swa/swa.h>
#include <cubeb/cubeb.h>
#include <dlg/dlg.hpp>
#include <atomic>
#include <memory>
#include <vector>

constexpr auto numChannels = 2u;
constexpr auto rate = 48000;

bool run = true;
cubeb* cubeb_ = nullptr;
cubeb_stream* stream_ = nullptr;
std::atomic<bool> gdrain {false};
unsigned winWidth;
unsigned winHeight;

struct AudioData {
	std::atomic<bool> swap {false};
	std::atomic<bool> drain {false};
	bool record {false};

	std::vector<float> lastBuf;
	unsigned pos {0};

	std::vector<float> newBuf;
} audioData;

void windowClose(struct swa_window*) {
	dlg_info("window closed");
	run = false;
}

void windowDraw(struct swa_window* w) {
	swa_image img;
	swa_window_get_buffer(w, &img);

	auto size = img.height * img.stride;
	memset(img.data, 0x0, size);

	swa_window_apply_buffer(w);
}

void clickat(float x, float y) {
	dlg_info("clickat {} {}", x, y);
	if(x > 0.8 && y > 0.9) {
		dlg_info("starting stream");
		cubeb_stream_start(stream_);
	} else if(x < 0.2 && y > 0.8) {
		dlg_info("stopping stream");
		cubeb_stream_stop(stream_);
	} else if(x < 0.2 && y < 0.1) {
		gdrain.store(true);
		dlg_info("drain: {}", gdrain.load());
	} else {
		audioData.swap.store(true);
		dlg_info("swapping");
	}
}

void mouseButton(struct swa_window*, const swa_mouse_button_event* ev) {
	if(ev->button != swa_mouse_button_left || !ev->pressed) {
		return;
	}

	float x = ev->x / float(winWidth);
	float y = ev->y / float(winHeight);
	clickat(x, y);
}

void touchBegin(struct swa_window*, const swa_touch_event* ev) {
	float x = ev->x / float(winWidth);
	float y = ev->y / float(winHeight);
	clickat(x, y);
}

void touchEnd(struct swa_window* w, unsigned id) {
}

void touchUpdate(struct swa_window* w, const swa_touch_event* ev) {
}

void touchCancel(struct swa_window* w) {
}

void windowResize(struct swa_window*, unsigned w, unsigned h) {
	winWidth = w;
	winHeight = h;
}

void cubebLog(const char* fmt, ...) {
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

long dataCb(cubeb_stream*, void*, const void* inBuf, void* outBuf, long nframes) {
	if(gdrain.load()) {
		gdrain.store(false);
		return 0;
	}

	auto nc = numChannels;
	auto& data = audioData;
	if(data.swap.load()) {
		data.swap.store(false);
		data.record ^= true;
		data.pos = 0u;
		if(!data.record) {
			data.lastBuf = std::move(data.newBuf);
		}
		dlg_info("recording: {}", data.record);
	}

	if(data.record) {
		auto ib = (float*) inBuf;
		data.newBuf.insert(data.newBuf.end(), ib, ib + nc * nframes);

		float avg = 0.0;
		for(auto i = 0u; i < nframes; ++i) {
			avg += ib[i];
		}
		dlg_info("record avg: {}", avg / nframes);
	}

	auto rem = nframes * nc;
	auto ob = (float*) outBuf;
	if(data.lastBuf.empty()) {
		auto frameSize = sizeof(float) * numChannels;
		std::memset(ob, 0x0, nframes * frameSize);
	} else {
		while(rem > 0) {
			auto count = std::min<std::size_t>(rem, data.lastBuf.size() - data.pos);
			std::memcpy(ob, data.lastBuf.data() + data.pos, count * sizeof(float));
			data.pos = (data.pos + count) % data.lastBuf.size();
			rem -= count;
			ob += count;
		}
	}

	return nframes;
}

void stateCb(cubeb_stream*, void* user, cubeb_state state) {
	dlg_info("state changed to {}", state);
}

bool setupAudio() {
	int rv = cubeb_set_log_callback(CUBEB_LOG_NORMAL, cubebLog);
	rv = cubeb_init(&cubeb_, "daudiotest", NULL);
	if(rv) {
		dlg_error("Failed to initialize cubeb");
		return false;
	}

	auto bid = cubeb_get_backend_id(cubeb_);
	dlg_assert(bid);
	dlg_info("cubeb backend: {}", bid);

	cubeb_stream_params output_params;
	output_params.format = CUBEB_SAMPLE_FLOAT32NE;
	output_params.channels = numChannels;
	output_params.layout = CUBEB_LAYOUT_UNDEFINED;
	output_params.prefs = CUBEB_STREAM_PREF_NONE;
	output_params.rate = 44100;

	cubeb_stream_params input_params;
	input_params.format = CUBEB_SAMPLE_FLOAT32NE;
	input_params.channels = numChannels;
	input_params.layout = CUBEB_LAYOUT_UNDEFINED;
	input_params.prefs = CUBEB_STREAM_PREF_NONE;
	input_params.rate = 44100;

	uint32_t latencyFrames = 0;
	rv = cubeb_get_min_latency(cubeb_, &output_params, &latencyFrames);
	if(rv != CUBEB_OK) {
		dlg_warn("Could not get minimum latency");
	} else {
		dlg_info("min latency frames: {}", latencyFrames);
	}

	latencyFrames = std::max<unsigned>(latencyFrames, 1024);
	dlg_info("using latency: {}", latencyFrames);

	rv = cubeb_stream_init(cubeb_, &stream_, "cubeb-test",
		NULL, &input_params,
		NULL, &output_params,
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

int main() {
	// setup display and window
	auto* dpy = swa_display_autocreate("cubebtest");
	if(!dpy) {
		throw std::runtime_error("Failed to create display");

	}

	static swa_window_listener wl {};
	wl.close = windowClose;
	wl.draw = windowDraw;
	wl.touch_begin = touchBegin;
	wl.mouse_button = mouseButton;
	wl.touch_end = touchEnd;
	wl.touch_update = touchUpdate;
	wl.touch_cancel = touchCancel;
	wl.resize = windowResize;

	swa_window_settings ws;
	swa_window_settings_default(&ws);
	ws.surface = swa_surface_buffer;
	ws.transparent = false;
	ws.listener = &wl;

	auto* win = swa_display_create_window(dpy, &ws);
	if(!win) {
		throw std::runtime_error("Failed to create window");
	}

	if(!setupAudio()) {
		throw std::runtime_error("Failed to setup audio");
	}

	// main loop
	while(run && swa_display_dispatch(dpy, true)) {
		// blank
	}

	// destroy audio
	dlg_trace("stopping stream...");
	cubeb_stream_stop(stream_);
	dlg_trace("destroying stream...");
	cubeb_stream_destroy(stream_);
	dlg_trace("destroying cubeb backend...");
	cubeb_destroy(cubeb_);
	dlg_trace("done");

	// destroy display
	dlg_trace("destroy display");
	swa_window_destroy(win);
	swa_display_destroy(dpy);

	dlg_trace("Success! Exiting.");
}
