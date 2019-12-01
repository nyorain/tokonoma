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
	format.ambisonicsOrder = 1;
	format.ambisonicsOrdering = IPL_AMBISONICSORDERING_ACN;
	format.ambisonicsNormalization = IPL_AMBISONICSNORMALIZATION_N3D;
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
	ssettings.sceneType = IPL_SCENETYPE_EMBREE;
	// ssettings.sceneType = IPL_SCENETYPE_PHONON;
	ssettings.numOcclusionSamples = 128;
	ssettings.maxConvolutionSources = 4u;
	ssettings.irradianceMinDistance = 1.0;

	// higher values here seems to cause *way* higher latency for
	// the indirect sound
	// ssettings.numRays = 4096;
	// ssettings.numDiffuseSamples = 512;
	// ssettings.numBounces = 8;
	// ssettings.numThreads = 3;
	// ssettings.irDuration = 1.5;

	ssettings.ambisonicsOrder = 1;
	ssettings.numRays = 8 * 4096;
	ssettings.numDiffuseSamples = 1024;
	ssettings.numBounces = 8;
	ssettings.numThreads = 6;
	ssettings.irDuration = 2;

	// generic material
	IPLMaterial material {0.1f,0.2f,0.3f,0.05f,0.100f,0.050f,0.030f};

	// brick
	material = {0.03f,0.04f,0.07f,0.05f,0.015f,0.015f,0.015f};

	iplCheck(iplCreateScene(context_, nullptr, ssettings, 1, &material,
		nullptr, nullptr, nullptr, nullptr, this, &scene_));

	// environment
	iplCheck(iplCreateEnvironment(context_, nullptr, ssettings, scene_,
		nullptr, &env_));

	IPLhandle mesh;
	iplCheck(iplCreateStaticMesh(scene_, positions.size(),
		tris.size(), positions.data(), tris.data(), mats.data(),
		&mesh));

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
	// listener
	std::array<nytl::Vec3f, 3> l;
	if(updateListener_.dequeue(l)) {
		listenerPos_ = {l[0].x, l[0].y, l[0].z};
		listenerDir_ = {l[1].x, l[1].y, l[1].z};
		listenerUp_ = {l[2].x, l[2].y, l[2].z};
	}

	// indirect
	if(flushIndirect_ == 1) {
		flushIndirect_ = 2;
	} else if(flushIndirect_ == 2) {
		flushIndirect_ = 0;
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
	if(useHRTF) {
		iplCheck(iplCreateAmbisonicsBinauralEffect(audio.binauralRenderer(),
			ambisonicsFormat(), stereoFormat(), &binauralEffect_));
	} else {
		iplCheck(iplCreateAmbisonicsPanningEffect(audio.binauralRenderer(),
			ambisonicsFormat(), stereoFormat(), &binauralEffect_));
	}
}

void ConvolutionAudio::render(unsigned nb, float* buf, bool mix) {
	// if(delay_ < 10) {
	// 	delay_ += nb;
	// 	return;
	// }

	if(audio_->flushIndirect()) {
		iplFlushAmbisonicsBinauralEffect(binauralEffect_);
	}

	if(!audio_->indirect()) {
		// TODO: if mix is not set, we must clear the buffer!
		return;
	}

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

			// TODO: do we have to rotate the output ambisonic audio?
			// wouldn't make such sense though i guess?

			if(useHRTF) {
				iplApplyAmbisonicsBinauralEffect(binauralEffect_,
					audio_->binauralRenderer(),
					ab, ab2);
			} else {
				iplApplyAmbisonicsPanningEffect(binauralEffect_,
					audio_->binauralRenderer(),
					ab, ab2);
			}

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
