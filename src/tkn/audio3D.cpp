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

IPLAudioFormat stereoFormat() {
	IPLAudioFormat format {};
	format.channelLayoutType = IPL_CHANNELLAYOUTTYPE_SPEAKERS;
	format.channelLayout = IPL_CHANNELLAYOUT_STEREO;
	format.channelOrder = IPL_CHANNELORDER_INTERLEAVED;
	return format;
}

IPLAudioFormat ambisonicsFormat() {
	IPLAudioFormat format {};
	format.channelLayoutType = IPL_CHANNELLAYOUTTYPE_AMBISONICS;
	format.ambisonicsOrder = 2;
	format.ambisonicsOrdering = IPL_AMBISONICSORDERING_ACN;
	format.ambisonicsNormalization = IPL_AMBISONICSNORMALIZATION_SN3D;
	format.channelOrder = IPL_CHANNELORDER_INTERLEAVED;
	return format;
}

// Audio3D
Audio3D::Audio3D(unsigned frameRate,
			std::vector<IPLVector3> positions,
			std::vector<IPLTriangle> tris,
			std::vector<IPLint32> mats) {
	iplCheck(iplCreateContext(&log, nullptr, nullptr, &context_));

	// scene
	IPLSimulationSettings ssettings {};
	ssettings.sceneType = IPL_SCENETYPE_PHONON;
	ssettings.numOcclusionSamples = 32;
	ssettings.numRays = 1 * 4096;
	ssettings.numDiffuseSamples = 256;
	ssettings.numBounces = 4;
	ssettings.numThreads = 2;
	ssettings.irDuration = 3.0;
	ssettings.ambisonicsOrder = 2;
	ssettings.maxConvolutionSources = 4u;
	ssettings.irradianceMinDistance = 0.01;

	// generic material
	IPLMaterial material {0.01f,0.002f,0.003f,0.05f,0.100f,0.050f,0.030f};

	iplCheck(iplCreateScene(context_, nullptr, ssettings, 1, &material,
		nullptr, nullptr, nullptr, nullptr, this, &scene_));

	IPLhandle mesh;
	iplCheck(iplCreateStaticMesh(scene_, positions.size(),
		tris.size(), positions.data(), tris.data(), mats.data(),
		&mesh));

	// environment
	iplCheck(iplCreateEnvironment(context_, nullptr, ssettings, scene_,
		nullptr, &env_));

	IPLRenderingSettings rsettings {};
	rsettings.samplingRate = frameRate;
	rsettings.frameSize = tkn::AudioPlayer::blockSize;
	rsettings.convolutionType = IPL_CONVOLUTIONTYPE_PHONON;

	// auto format = stereoFormat();
	auto aformat = ambisonicsFormat();
	iplCheck(iplCreateEnvironmentalRenderer(context_, env_, rsettings,
		aformat, nullptr, nullptr, &envRenderer_));

	// binaural
	IPLHrtfParams hrtfParams {IPL_HRTFDATABASETYPE_DEFAULT, nullptr, nullptr};
	iplCheck(iplCreateBinauralRenderer(context_, rsettings, hrtfParams,
		&binauralRenderer_));
}

Audio3D::~Audio3D() {
	if(envRenderer_) iplDestroyEnvironmentalRenderer(&envRenderer_);
	if(env_) iplDestroyEnvironment(&env_);
	if(binauralRenderer_) iplDestroyBinauralRenderer(&binauralRenderer_);
	if(context_) iplDestroyContext(&context_);
	iplCleanup();
}

void Audio3D::log(char* msg) {
	auto nl = std::strchr(msg, '\n');
	auto len = nl ? nl - msg : std::strlen(msg);
	dlg_infot(("phonon"), "phonon: {}", std::string_view(msg, len));
}

void Audio3D::closestHit(const IPLfloat32* origin, const IPLfloat32* direction,
		const IPLfloat32 minDistance, const IPLfloat32 maxDistance,
		IPLfloat32* hitDistance, IPLfloat32* hitNormal,
		IPLMaterial** hitMaterial, IPLvoid* userData) {
	dlg_info("closestHit");
	*hitDistance = -1.f;
}

void Audio3D::anyHit(const IPLfloat32* origin, const IPLfloat32* direction,
		const IPLfloat32 minDistance, const IPLfloat32 maxDistance,
		IPLint32* hitExists, IPLvoid* userData) {
	dlg_info("anyHit");
	*hitExists = 0;
}

// all methods only for render thread
IPLDirectSoundPath Audio3D::path(const IPLSource& source, float radius) const {
	return iplGetDirectSoundPath(environment(),
		listenerPos_, listenerDir_, listenerUp_,
		source, radius,
		IPL_DIRECTOCCLUSION_TRANSMISSIONBYFREQUENCY,
		IPL_DIRECTOCCLUSION_VOLUMETRIC);
}

void Audio3D::update() {
	std::array<nytl::Vec3f, 3> l;
	if(updateListener_.dequeue(l)) {
		listenerPos_ = {l[0].x, l[0].y, l[0].z};
		listenerDir_ = {l[1].x, l[1].y, l[1].z};
		listenerUp_ = {l[2].x, l[2].y, l[2].z};
	}
}

void Audio3D::applyDirectEffect(IPLhandle directEffect,
		unsigned nb, float* in, float* out,
		const IPLDirectSoundPath& path) {
	int bs = tkn::AudioPlayer::blockSize;
	auto format = stereoFormat();
	IPLDirectSoundEffectOptions opts {};
	opts.applyAirAbsorption = IPL_TRUE;
	opts.applyDirectivity = IPL_TRUE;
	opts.applyDistanceAttenuation = IPL_TRUE;
	opts.directOcclusionMode = IPL_DIRECTOCCLUSION_TRANSMISSIONBYFREQUENCY;

	for(auto i = 0u; i < nb; ++i) {
		IPLAudioBuffer inb{format, bs, ((float*) in + i * bs * 2), nullptr};
		IPLAudioBuffer outb{format, bs, out + i * bs * 2, nullptr};
		iplApplyDirectSoundEffect(directEffect, inb, path, opts, outb);
	}
}

void Audio3D::applyHRTF(IPLhandle effect, unsigned nb, float* in, float* out,
		IPLVector3 dir) {
	int bs = tkn::AudioPlayer::blockSize;
	auto format = stereoFormat();
	for(auto i = 0u; i < nb; ++i) {
		IPLAudioBuffer inb{format, bs, ((float*) in + i * bs * 2), nullptr};
		IPLAudioBuffer outb{format, bs, out + i * bs * 2, nullptr};
		iplApplyBinauralEffect(effect, binauralRenderer_, inb, dir,
			IPL_HRTFINTERPOLATION_BILINEAR, outb);
	}
}

// ConvolutionAudio
ConvolutionAudio::ConvolutionAudio(Audio3D& audio) : audio_(&audio) {
	iplCheck(iplCreateAmbisonicsBinauralEffect(audio.binauralRenderer(),
		ambisonicsFormat(), stereoFormat(), &binauralEffect_));
}

void ConvolutionAudio::render(unsigned nb, float* buf, bool mix) {
	// if(delay_ < 10) {
	// 	delay_ += nb;
	// 	return;
	// }

	IPLint32 bs = AudioPlayer::blockSize;

	if(mix) {
		// No idea what is actually needed tbh
		auto b0 = bufCache.get(0, 64 * bs).data();
		auto b1 = bufCache.get(1, 2 * bs).data();

		IPLAudioBuffer ab {ambisonicsFormat(), bs, b0, nullptr};
		IPLAudioBuffer ab2 {stereoFormat(), bs, b1, nullptr};
		for(auto i = 0u; i < nb; ++i) {
			iplGetMixedEnvironmentalAudio(audio_->envRenderer(),
				audio_->listenerPos(),
				audio_->listenerDir(),
				audio_->listenerUp(), ab);

			iplApplyAmbisonicsBinauralEffect(binauralEffect_,
				audio_->binauralRenderer(),
				ab, ab2);

			for(auto j = 0; j < 2 * bs; ++j) {
				buf[j] += b1[j];
			}
			buf += 2 * bs;
		}
	} else {
		dlg_fatal("Unimplemented");

		// // write directly into the audio buffer
		// IPLAudioBuffer ab {format, bs, buf, nullptr};
		// for(auto i = 0u; i < nb; ++i) {
		// 	iplGetMixedEnvironmentalAudio(audio_->envRenderer(),
		// 		audio_->listenerPos(),
		// 		audio_->listenerDir(),
		// 		audio_->listenerUp(), ab);
		// 	ab.interleavedBuffer += 2 * i * bs;
		// }
	}
}

} // namespace tkn
