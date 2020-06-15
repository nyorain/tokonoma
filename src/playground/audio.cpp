#include <tkn/audio.hpp>
#include <tkn/sound.hpp>
#include <tkn/sampling.hpp>
#include <nytl/math.hpp>
#include <nytl/stringParam.hpp>
#include <dlg/dlg.hpp>
#include <stb_vorbis.h>

#include <thread>
#include <chrono>
#include <cmath>

class SinAudio : public tkn::AudioSource {
public:
	SinAudio(unsigned rate, unsigned nc) : spf_(1.0 / rate), channels_(nc) {}

    static constexpr auto pitch = 440.0;
    static constexpr auto radsPerSec = pitch * 2.0 * nytl::constants::pi;

	void render(unsigned nb, float* buf, bool mix) override {
		auto nf = tkn::AudioPlayer::blockSize * nb;
		for(auto i = 0u; i < nf; ++i) {
			float val = 0.25 * std::sin((soffset_ + i * spf_) * radsPerSec);
			for(auto c = 0u; c < channels_; ++c) {
				auto& b = buf[i * channels_ + c];
				b = (mix ? b : 0) + val;
			}
		}

        soffset_ += spf_ * nf;
	}

	double soffset_ {};
	double spf_ {}; // second per frame
	unsigned channels_ {};
};

int main() {
	tkn::UniqueSoundBuffer buf;
	tkn::SoundBufferAudio* sba = nullptr;
	tkn::StreamedVorbisAudio* streamAudio = nullptr;
	SinAudio* sina = nullptr;

	tkn::AudioPlayer ap("tkn/audio", 44100, 2);

	/*
	auto stream = true;
	auto samplePath = TKN_BASE_DIR "/assets/punch.ogg";
	if(stream) {
		auto a = std::make_unique<tkn::StreamedVorbisAudio>(samplePath,
			ap.rate(), ap.channels());
		streamAudio = a.get();
		ap.add(std::move(a));
	} else {
		auto sample = tkn::loadVorbis(samplePath);
		dlg_assert(sample.data);
		if(ap.rate() != sample.rate || ap.channels() != sample.channelCount) {
			buf = resample(sample, ap.rate(), ap.channels());
		} else {
			buf = std::move(sample);
		}

		auto usba = std::make_unique<tkn::SoundBufferAudio>(buf);
		sba = usba.get();
		ap.add(std::move(usba));
	}
	*/

	// tkn::Buffers bufs;
	// auto& a = ap.create<tkn::StreamedResampler<tkn::MP3Decoder>>(bufs,
	// 	ap.rate(), ap.channels(), "test.mp3");
	// auto& a = ap.create<tkn::StreamedResampler<tkn::VorbisDecoder>>(bufs,
		// ap.rate(), ap.channels(), "test48khz.ogg");
	// auto& a = ap.create<tkn::StreamedMP3Audio>("test.mp3");

	ap.start();

	auto run = true;
	while(run) {
		char c;
		int ret = std::fread(&c, 1, 1, stdin);
		if(ret != 1) {
			if(ret != 0) {
				dlg_error("fread failed: {}", ret);
			}
			break;
		}

		switch(c) {
			case 'a':
				if(!sina) {
					sina = &ap.create<SinAudio>(ap.rate(), ap.channels());
				}
				break;
			case 'r':
				if(sina) {
					ap.remove(*sina);
					sina = nullptr;
				}
				break;
			case 'q':
				run = false;
				break;
			case 'p':
				if(sba) {
					sba->volume(sba->playing() ? 0.f : 1.f);
				} else if(streamAudio) {
					streamAudio->volume(streamAudio->playing() ? 0.f : 1.f);
				}
				break;
			default:
				break;
		}
	}
}
