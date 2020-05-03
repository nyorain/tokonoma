#include <tkn/audio.hpp>
#include <tkn/sound.hpp>
#include <tkn/ringbuffer.hpp>
#include <dlg/dlg.hpp>
#include <stdexcept>
#include <iostream>
#include <charconv>
#include <chrono>
#include <tsf.h>
#include <tml.h>

class SoundFontAudio : public tkn::AudioSource {
public:
	static constexpr auto msgBufSize = 10;
	struct Msg { bool on; int key; int preset; float vel; };
	tkn::RingBuffer<Msg> msgs_{msgBufSize};
	std::atomic<int> gain_ {0};

public:
	SoundFontAudio(unsigned rate) : rate_(rate) {
		tsf_ = tsf_load_filename("violin.sf2");
		if(!tsf_) {
			std::string err = "Could not load soundfont. "
				"Expected in build dir, path currently hard-coded";
			throw std::runtime_error(err);
		}
	}

	~SoundFontAudio() {
		tsf_close(tsf_);
	}

	void render(unsigned nb, float* buf, bool mix) override {
		tsf_set_output(tsf_, TSF_STEREO_INTERLEAVED, rate_, gain_);

		// process messages
		Msg mbuf[msgBufSize];
		unsigned count = msgs_.dequeue(mbuf, msgBufSize);
		for(auto i = 0u; i < count; ++i) {
			auto& msg = mbuf[i];
			if(msg.on) {
				dlg_info("starting note {}", msg.key);
				tsf_note_on(tsf_, msg.preset, msg.key, msg.vel);
			} else {
				dlg_info("stopping note {}", msg.key);
				tsf_note_off(tsf_, msg.preset, msg.key);
			}
		}

		// render
		auto nf = nb * tkn::AudioPlayer::blockSize;
		tsf_render_float(tsf_, buf, nf, mix);
	}

private:
	tsf* tsf_;
	unsigned rate_ {};
};

std::vector<std::string_view> split(std::string_view s,
		std::string_view delim = " ") {
	size_t pos;
	size_t current = 0;
	std::vector<std::string_view> ret;
	while ((pos = s.find(delim, current)) != std::string::npos) {
		ret.push_back(s.substr(current, pos));
		current = pos + 1;
	}

	if(current < s.size()) {
		ret.push_back(s.substr(current));
	}

	return ret;
}

// nvm, not supported for float in gcc yet (end of 2019)
// template<typename T>
// bool from_chars(std::string_view view, T& val) {
// 	auto ptr = std::from_chars(view.begin(), view.end(), val).ptr;
// 	return ptr != view.begin();
// }

bool from_chars(std::string_view view, int& val) {
	auto ptr = std::from_chars(view.begin(), view.end(), val).ptr;
	return ptr != view.begin();
}

int main() {
	tkn::AudioPlayer ap("tkn/soundfont", 0, 2);
	auto& audio = ap.create<SoundFontAudio>(ap.rate());
	// auto& midiAudio = ap.create<tkn::MidiAudio>("violin.sf2",
	// 	TKN_BASE_DIR "/assets/audio/ibi/Some Sand.mid", ap.rate());

	int preset = 0;
	float vel = 1.f;

	ap.start();

	std::string line;
	while(std::getline(std::cin, line)) {
		if(line.empty()) {
			continue;
		}

		auto toks = split(line);
		if(toks[0] == "quit" || toks[0] == "q") {
			break;
		} if(toks[0] == "volume") {
			if(toks.size() != 2) {
				std::cout << "Usage: volume <number>\n";
				continue;
			}

			int gain;
			if(!from_chars(toks[1], gain)) {
				std::cout << "Invalid integer: " << toks[1] << "\n";
				continue;
			}

			audio.gain_ += gain;
		} else if(toks[0] == "play") {
			if(toks.size() != 2) {
				std::cout << " Usage: play <key>\n";
				continue;
			}

			int key;
			if(!from_chars(toks[1], key)) {
				std::cout << "Invalid integer: " << toks[1] << "\n";
				continue;
			}

			SoundFontAudio::Msg msg;
			msg.key = key;
			msg.on = true;
			msg.preset = preset;
			msg.vel = vel;
			audio.msgs_.enqueue(msg);
		} else if(toks[0] == "stop") {
			if(toks.size() != 2) {
				std::cout << " Usage: stop <key>\n";
				continue;
			}

			int key;
			if(!from_chars(toks[1], key)) {
				std::cout << "Invalid integer: " << toks[1] << "\n";
				continue;
			}

			SoundFontAudio::Msg msg;
			msg.key = key;
			msg.on = false;
			msg.preset = preset;
			audio.msgs_.enqueue(msg);
		} else if(toks[0] == "preset") {
			if(toks.size() != 2) {
				std::cout << " Usage: preset <preset>\n";
				continue;
			}

			if(!from_chars(toks[1], preset)) {
				std::cout << "Invalid integer: " << toks[1] << "\n";
				continue;
			}
		} else if(toks[0] == "vel") {
			if(toks.size() != 2) {
				std::cout << " Usage: vel <velocity>\n";
				continue;
			}

			std::string str(toks[1]);
			float nv = std::strtof(str.c_str(), nullptr);
			if(nv <= 0.f) {
				std::cout << "Invalid velocity: " << toks[1] << "\n";
				continue;
			}

			vel = nv;
		}
	}
}

