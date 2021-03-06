#pragma once

#include <tkn/audio.hpp>
#include <tkn/lockfree.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/vec.hpp>
#include <dlg/dlg.hpp>
#include <phonon.h>

// TODO: make it possible to change radius and 'useHRTF' per
// audio source. Add mechanism that automatically switches from
// HRTF to panning when audio source is far away
// TODO: add support for effect baking
// TODO: allow multiple different materials

namespace tkn {

IPLAudioFormat stereoFormat();
IPLAudioFormat ambisonicsFormat();

/// Always outputs interleaved stereo.
class Audio3D {
public:
	static constexpr auto ambisonicsOrder = 1u;

	struct Settings {
		static constexpr auto numOcclusionSamples = 128u;
		static constexpr auto maxConvolutionSources = 4u;
		static constexpr auto irradianceMinDistance = 1.0;
		static constexpr auto numRays = 1024;
		static constexpr auto numDiffuseSamples = 256;
		static constexpr auto numBounces = 6u;
		static constexpr auto numThreads = 4;
		static constexpr auto irDuration = 1.5;
	};

public:
	Audio3D(unsigned rate);
	~Audio3D();

	// all methods only for render thread
	IPLDirectSoundPath path(const IPLSource& source, float radius) const;
	void update();

	void addStaticMesh(const std::vector<IPLVector3>& positions,
		const std::vector<IPLTriangle>& tris,
		const std::vector<IPLint32>& mats);

	// Applies the direct effect for the given sound path
	void applyDirectEffect(IPLhandle directEffect,
		unsigned nb, float* in, float* out,
		const IPLDirectSoundPath& path);
	// Applies hrtf (head related transfer function), i.e. renders the input
	// buffer into the output buffer as if it comes from the given direction
	// to the listener.
	void applyHRTF(IPLhandle effect, unsigned nb, float* in, float* out,
		IPLVector3 dir);
	// Cheaper than hrtf; good for distant sources.
	// Applies a simpler stereo effect for the given audio without considering
	// actual head-related data.
	void applyPanning(IPLhandle effect, unsigned nb, float* in, float* out,
		IPLVector3 dir);

	// any thread
	IPLhandle context() const { return context_; }
	IPLhandle envRenderer() const { return envRenderer_; }
	IPLhandle binauralRenderer() const { return binauralRenderer_; }
	IPLhandle environment() const { return env_; }
	IPLhandle scene() const { return scene_; }

	void toggleIndirect() {
		indirect_ ^= 1; // toggle
		flushIndirect_ = 1;
	}

	// render thread
	IPLVector3 listenerPos() const { return listenerPos_; }
	IPLVector3 listenerUp() const { return listenerUp_; }
	IPLVector3 listenerDir() const { return listenerDir_; }

	bool indirect() const { return indirect_.load(); }
	bool flushIndirect() const { return flushIndirect_.load() == 1; }

	// any thread
	void updateListener(nytl::Vec3f pos, nytl::Vec3f dir, nytl::Vec3f up) {
		std::array<nytl::Vec3f, 3> l {pos, dir, up};
		updateListener_.enqueue(l);
	}

protected:
	static void log(char* msg);

protected:
	IPLhandle context_;
	IPLhandle env_;
	IPLhandle envRenderer_;
	IPLhandle binauralRenderer_;
	IPLhandle scene_;

	IPLVector3 listenerPos_ {0.f, 0.f, 0.f};
	IPLVector3 listenerUp_ {0.f, 1.f, 0.f};
	IPLVector3 listenerDir_ {0.f, 0.f, -1.f};

	std::atomic<int> indirect_ {1};
	std::atomic<int> flushIndirect_ {0};

	tkn::Shared<std::array<nytl::Vec3f, 3>> updateListener_;
};

template<typename Source>
class AudioSource3D : public tkn::AudioSource {
public:
	static constexpr auto useHRTF = true;
	static constexpr auto radius = 0.25f;

	std::atomic<bool> direct {true};
	std::atomic<bool> indirect {true};

public:
	/// The source must be set up with the same channel count
	/// and sampling rate as the Audio3D object.
	template<typename... Args>
	AudioSource3D(Audio3D& audio, BufCaches bc, Args&&... args) :
			source_(std::forward<Args>(args)...), audio_(&audio), bufs_(bc) {
		sourceInfo_.position = {0.f, 0.f, 0.f};
		sourceInfo_.ahead = {0.f, 0.f, -1.f};
		sourceInfo_.up = {0.f, 1.f, 0.f};
		sourceInfo_.right = {1.f, 0.f, 0.f};
		sourceInfo_.directivity.dipoleWeight = 0.f;

		IPLerror err;
		auto format = stereoFormat();
		err = iplCreateDirectSoundEffect(audio.envRenderer(), format, format,
			&directEffect_);
		dlg_assert(!err);

		if(useHRTF) {
			err = iplCreateBinauralEffect(audio.binauralRenderer(), format, format,
				&binauralEffect_);
			dlg_assert(!err);
		} else {
			err = iplCreatePanningEffect(audio.binauralRenderer(), format, format,
				&binauralEffect_);
			dlg_assert(!err);
		}

		auto aformat = ambisonicsFormat();
		err = iplCreateConvolutionEffect(audio.envRenderer(), {},
			IPL_SIMTYPE_REALTIME, format, aformat, &convolutionEffect_);
		dlg_assert(!err);
	}

	Source& inner() { return source_; }
	const Source& inner() const { return source_; }

	// only called in update thread
	void update() override {
		source_.update();
	}

	// only called in render thread
	void render(unsigned nb, float* buf, bool mix) override {
		nytl::Vec3f pos;
		if(updatePos_.dequeue(pos)) {
			sourceInfo_.position = {pos.x, pos.y, pos.z};
		}

		auto ns = nb * tkn::AudioPlayer::blockSize * 2;
		auto* b0 = bufs_.render.get<0>(ns).data();
		auto* b1 = bufs_.render.get<1>(ns).data();

		memset(b0, 0x0, ns * sizeof(float));
		memset(b1, 0x0, ns * sizeof(float));

		// no mixing here, overwrite b0 contents
		source_.render(nb, b0, false);

		// add output to environment
		if(audio_->flushIndirect()) {
			iplFlushConvolutionEffect(convolutionEffect_);
		}

		if(audio_->indirect() && this->indirect.load()) {
			auto format = stereoFormat();
			IPLint32 bs = AudioPlayer::blockSize;
			for(auto i = 0u; i < nb; ++i) {
				IPLAudioBuffer ab {format, bs, b0 + i * bs * 2, nullptr};
				iplSetDryAudioForConvolutionEffect(convolutionEffect_, sourceInfo_, ab);
			}
		}

		// for testing: no direct audio at all
		if(!this->direct.load()) {
			if(!mix) {
				std::memset(buf, 0x0, ns * sizeof(float));
			}

			return;
		}

		auto path = audio_->path(sourceInfo_, radius);
		audio_->applyDirectEffect(directEffect_, nb, b0, b1, path);

		if(mix) {
			if(useHRTF) {
				audio_->applyHRTF(binauralEffect_, nb, b1, b0, path.direction);
			} else {
				audio_->applyPanning(binauralEffect_, nb, b1, b0, path.direction);
			}

			for(auto i = 0u; i < ns; ++i) {
				buf[i] += b0[i];
			}

		} else {
			if(useHRTF) {
				audio_->applyHRTF(binauralEffect_, nb, b1, buf, path.direction);
			} else {
				audio_->applyPanning(binauralEffect_, nb, b1, buf, path.direction);
			}
		}
	}

	// only called from main/other threads
	bool position(nytl::Vec3f pos) {
		return updatePos_.enqueue(pos);
	}

protected:
	Source source_;
	Audio3D* audio_ {};

	IPLSource sourceInfo_ {};
	tkn::Shared<nytl::Vec3f> updatePos_;

	IPLhandle directEffect_ {};
	IPLhandle binauralEffect_ {};
	IPLhandle convolutionEffect_ {};
	BufCaches bufs_;
};

class ConvolutionAudio : public AudioSource {
public:
	static constexpr auto useHRTF = true;
	static constexpr float factor = 1.f;

public:
	ConvolutionAudio(Audio3D& audio, BufCaches bc);
	void render(unsigned nb, float* buf, bool mix) override;

protected:
	Audio3D* audio_;
	IPLhandle binauralEffect_ {};
	BufCaches bufs_;
};

} // namespace tkn
