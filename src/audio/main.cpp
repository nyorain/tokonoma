#include <stage/audio.hpp>
#include <nytl/math.hpp>
#include <dlg/dlg.hpp>

#include <thread>
#include <chrono>
#include <cmath>

class DummySound : public doi::Audio {
public:
	void render(float& buf, unsigned samples) override {
        auto pitch = 440.0;
        auto radsPerSec = pitch * 2.0 * nytl::constants::pi;
		auto secondsPerFrame = 1.0 / 48000;

		auto it = &buf;
		for(auto i = 0u; i < samples; ++i) {
			float val = 0.05 * std::sin((soffset + i * secondsPerFrame) * radsPerSec);
			*(it++) += val;
			*(it++) += val;
		}

        soffset += secondsPerFrame * samples;
	}

	double soffset {};
};

int main() {
	doi::AudioPlayer ap;
	ap.add(std::make_unique<DummySound>());
	std::this_thread::sleep_for(std::chrono::seconds(10));
}
