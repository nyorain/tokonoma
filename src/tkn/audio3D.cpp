#include <tkn/audio3D.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

static const char* name(IPLerror err) {
	switch(err) {
		case IPL_STATUS_FAILURE: return "failure";
		case IPL_STATUS_OUTOFMEMORY: return "out of memory";
		case IPL_STATUS_INITIALIZATION: return "initialization";
		default: return "<unknown>";
	}
}

#define iplCheck(x) do { \
	auto res = (x); \
	if(res != IPL_STATUS_SUCCESS) { \
		dlg_error("ipl returned {}", name(res)); \
	} \
} while(0)

constexpr IPLAudioFormat stereoFormat() {
	IPLAudioFormat format {};
	format.channelLayoutType = IPL_CHANNELLAYOUTTYPE_SPEAKERS;
	format.channelLayout = IPL_CHANNELLAYOUT_STEREO;
	format.channelOrder = IPL_CHANNELORDER_INTERLEAVED;
	return format;
}

// Audio3D
Audio3D::Audio3D(unsigned frameRate) {
	iplCheck(iplCreateContext(&log, nullptr, nullptr, &context_));

	// environment
	IPLSimulationSettings envSettings {};
	envSettings.sceneType = IPL_SCENETYPE_PHONON;
	envSettings.numOcclusionSamples = 32;
	envSettings.numDiffuseSamples = 32;
	envSettings.numBounces = 8;
	envSettings.numThreads = 1;
	envSettings.irDuration = 0.5;
	envSettings.maxConvolutionSources = 8u;
	envSettings.irradianceMinDistance = 0.05;

	iplCheck(iplCreateEnvironment(context_, nullptr, envSettings, nullptr,
		nullptr, &env_));

	IPLRenderingSettings rsettings {};
	rsettings.samplingRate = frameRate;
	rsettings.frameSize = tkn::AudioPlayer::blockSize;
	rsettings.convolutionType = IPL_CONVOLUTIONTYPE_PHONON;
	auto format = stereoFormat();
	iplCheck(iplCreateEnvironmentalRenderer(context_, env_, rsettings,
		format, nullptr, nullptr, &envRenderer_));

	// binaural
	IPLHrtfParams hrtfParams {IPL_HRTFDATABASETYPE_DEFAULT, nullptr, nullptr};
	iplCheck(iplCreateBinauralRenderer(context_, rsettings, hrtfParams,
		&binaural_.renderer));
	iplCheck(iplCreateBinauralEffect(binaural_.renderer, format, format,
		&binaural_.effect));

	// direct
	iplCheck(iplCreateDirectSoundEffect(envRenderer_, format, format,
		&directEffect_));
}

Audio3D::~Audio3D() {
	if(directEffect_) iplDestroyDirectSoundEffect(&directEffect_);
	if(envRenderer_) iplDestroyEnvironmentalRenderer(&envRenderer_);
	if(env_) iplDestroyEnvironment(&env_);
	if(binaural_.effect) iplDestroyBinauralEffect(&binaural_.effect);
	if(binaural_.renderer) iplDestroyBinauralRenderer(&binaural_.renderer);
	if(context_) iplDestroyContext(&context_);
	iplCleanup();
}

void Audio3D::log(char* msg) {
	dlg_infot(("phonon"), "phonon: {}", msg);
}

// all methods only for render thread
IPLDirectSoundPath Audio3D::path(const IPLSource& source, float radius) const {
	return iplGetDirectSoundPath(environment(),
		listenerPos_, listenerDir_, listenerUp_,
		source, radius,
		IPL_DIRECTOCCLUSION_NONE,
		IPL_DIRECTOCCLUSION_RAYCAST);
}

void Audio3D::update() {
	std::array<nytl::Vec3f, 3> l;
	if(updateListener_.dequeue(l)) {
		listenerPos_ = {l[0].x, l[0].y, l[0].z};
		listenerDir_ = {l[1].x, l[1].y, l[1].z};
		listenerUp_ = {l[2].x, l[2].y, l[2].z};
	}
}

void Audio3D::applyDirectEffect(unsigned nb, float* in, float* out,
		const IPLDirectSoundPath& path) {
	int bs = tkn::AudioPlayer::blockSize;
	auto format = stereoFormat();
	IPLDirectSoundEffectOptions opts {};
	opts.applyAirAbsorption = IPL_TRUE;
	opts.applyDirectivity = IPL_TRUE;
	opts.applyDistanceAttenuation = IPL_TRUE;
	opts.directOcclusionMode = IPL_DIRECTOCCLUSION_NONE; // TODO
	for(auto i = 0u; i < nb; ++i) {
		IPLAudioBuffer inb{format, bs, ((float*) in + i * bs * 2), nullptr};
		IPLAudioBuffer outb{format, bs, out + i * bs * 2, nullptr};
		iplApplyDirectSoundEffect(directEffect_, inb, path, opts, outb);
	}
}

void Audio3D::applyHRTF(unsigned nb, float* in, float* out, IPLVector3 dir) {
	dlg_info("dir: {} {} {}", dir.x, dir.y, dir.z);
	int bs = tkn::AudioPlayer::blockSize;
	auto format = stereoFormat();
	for(auto i = 0u; i < nb; ++i) {
		IPLAudioBuffer inb{format, bs, ((float*) in + i * bs * 2), nullptr};
		IPLAudioBuffer outb{format, bs, out + i * bs * 2, nullptr};
		iplApplyBinauralEffect(binaural_.effect, binaural_.renderer, inb, dir,
			IPL_HRTFINTERPOLATION_BILINEAR, outb);
	}
}

} // namespace tkn
