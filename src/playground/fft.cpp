#include <tkn/kiss_fft.h>
#include <cmath>
#include <vector>
#include <cstdio>

int main() {
	auto pi = 3.141f; // close enough
	auto count = 1024u;
	auto freq = 200;
	float freqf = 2 * pi * float(freq) / (count);

	std::vector<kiss_fft_cpx> time;
	time.reserve(count);
	for(auto i = 0u; i < count; ++i) {
		auto r = 5 + std::cos(freqf * i);
		time.push_back({r, 0});
	}

	auto fft = kiss_fft_alloc(count, 0, nullptr, nullptr);

	std::vector<kiss_fft_cpx> freqd;
	freqd.resize(count);
	kiss_fft(fft, time.data(), freqd.data());

	// we disregard second half, we have no frequencies that high
	for(auto i = 0u; i < freqd.size() / 2; ++i) {
		auto& val = freqd[i];
		auto l = std::sqrt(val.r * val.r + val.i * val.i) / count;
		auto phase = std::atan2(val.i, val.r);
		if(l > 0.1) {
			std::printf("%d: %f, %f\n", i, l, phase);
		}
	}
}
