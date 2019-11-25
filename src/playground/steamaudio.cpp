#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>
#include <phonon.h>

const auto samplingrate = 44100;
const auto framesize = 1024;

int main(int argc, char** argv) {
    IPLhandle context = nullptr;
    IPLhandle renderer = nullptr;
    IPLhandle effect = nullptr;

    iplCreateContext(nullptr, nullptr, nullptr, &context);
    IPLRenderingSettings settings { samplingrate, framesize };
    IPLHrtfParams hrtfParams{ IPL_HRTFDATABASETYPE_DEFAULT, nullptr, nullptr };
    iplCreateBinauralRenderer(context, settings, hrtfParams, &renderer);

    IPLAudioFormat mono;
    mono.channelLayoutType  = IPL_CHANNELLAYOUTTYPE_SPEAKERS;
    mono.channelLayout      = IPL_CHANNELLAYOUT_MONO;
    mono.channelOrder       = IPL_CHANNELORDER_INTERLEAVED;

    IPLAudioFormat stereo;
    stereo.channelLayoutType  = IPL_CHANNELLAYOUTTYPE_SPEAKERS;
    stereo.channelLayout      = IPL_CHANNELLAYOUT_STEREO;
    stereo.channelOrder       = IPL_CHANNELORDER_INTERLEAVED;

    iplCreateBinauralEffect(renderer, mono, stereo, &effect);
    std::vector<float> outputaudioframe(2 * framesize);
    std::vector<float> outputaudio;

	// rendering here

    iplDestroyBinauralEffect(&effect);
    iplDestroyBinauralRenderer(&renderer);
    iplDestroyContext(&context);
    iplCleanup();

    return 0;
}
