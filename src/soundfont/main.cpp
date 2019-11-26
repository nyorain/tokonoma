#include <tkn/audio.hpp>
#include <tkn/ringbuffer.hpp>
#include <dlg/dlg.hpp>
#include <stdexcept>
#include <iostream>
#include <charconv>
#include <chrono>

#define TSF_IMPLEMENTATION
#include <tsf.h>

#define TML_IMPLEMENTATION
#include <tml.h>

class SoundFontAudio : public tkn::Audio {
public:
	static constexpr auto msgBufSize = 10;
	struct Msg { bool on; int key; int preset; float vel; };
	tkn::ring_buffer_base<Msg> msgs_{msgBufSize};
	std::atomic<int> gain_ {0};

public:
	SoundFontAudio() {
		tsf_ = tsf_load_filename("piano.sf2");
		if(!tsf_) {
			std::string err = "Could not load soundfont. "
				"Expected in build dir, path currently hard-coded";
			throw std::runtime_error(err);
		}
	}

	~SoundFontAudio() {
		tsf_close(tsf_);
	}

	void render(const tkn::AudioPlayer& ap, float* buf, unsigned nf) override {
		dlg_assert(ap.channels() == 2);
		tsf_set_output(tsf_, TSF_STEREO_INTERLEAVED, ap.rate(), gain_);

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
		tsf_render_float(tsf_, buf, nf, 1);
	}

private:
	tsf* tsf_;
};

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
	tkn::AudioPlayer ap("tkn/soundfont");
	auto& audio = ap.create<SoundFontAudio>();
	auto& midiAudio = ap.create<MidiAudio>();

	int preset = 0;
	float vel = 1.f;

	std::string line;
	std::cout << ">> " << std::flush;
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

		std::cout << ">> " << std::flush;
	}
}

