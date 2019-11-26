#pragma once

#include <tkn/audio.hpp>
#include <tkn/lockfree.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/vec.hpp>
#include <dlg/dlg.hpp>
#include <phonon.h>

namespace tkn {

/// Always outputs interleaved stereo.
class Audio3D {
public:
	Audio3D(unsigned rate);
	~Audio3D();

	// all methods only for render thread
	IPLDirectSoundPath path(const IPLSource& source, float radius) const;
	void update();

	// TODO: high level interface applying everything
	// void process(unsigned nb, const IPLSource& source, float* buf);

	// detailed
	void applyDirectEffect(unsigned nb, float* in, float* out,
			const IPLDirectSoundPath& path);
	void applyHRTF(unsigned nb, float* in, float* out, IPLVector3 dir);

	// TODO: cheaper than hrtf; good for distant sources
	// void applyPanning(unsigned nb, float* in, float* out, IPLVector3 dir);

	// any thread
	IPLhandle context() const { return context_; }
	IPLhandle envRenderer() const { return envRenderer_; }
	IPLhandle environment() const { return env_; }

	// main/other threads
	void updateListener(nytl::Vec3f pos, nytl::Vec3f dir, nytl::Vec3f up) {
		std::array<nytl::Vec3f, 3> l {pos, dir, up};
		updateListener_.enqueue(l);
	}

	static void log(char* msg);

protected:
	IPLhandle context_;
	IPLhandle env_;
	IPLhandle envRenderer_;
	IPLhandle directEffect_;

	struct {
		IPLhandle renderer {};
		IPLhandle effect {};
	} binaural_;

	IPLVector3 listenerPos_;
	IPLVector3 listenerUp_;
	IPLVector3 listenerDir_;

	tkn::Shared<std::array<nytl::Vec3f, 3>> updateListener_;
};

template<typename Source>
class AudioSource3D : public tkn::AudioSource {
public:
	// TODO
	BufCache bufCache;

public:
	/// The source must be set up with the same channel count
	/// and sampling rate as the Audio3D object.
	template<typename... Args>
	AudioSource3D(Audio3D& audio, Args&&... args) :
			source_(std::forward<Args>(args)...), audio_(&audio) {
		sourceInfo_.position = {0.f, 0.f, 0.f};
		sourceInfo_.ahead = {0.f, 0.f, -1.f};
		sourceInfo_.up = {0.f, 1.f, 0.f};
		sourceInfo_.directivity.dipoleWeight = 0.f;
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
		auto* b0 = bufCache.get(0, ns).data();
		auto* b1 = bufCache.get(1, ns).data();

		source_.render(nb, b0, false);
		auto path = audio_->path(sourceInfo_, radius_);
		audio_->applyDirectEffect(nb, b0, b1, path);

		if(mix) {
			audio_->applyHRTF(nb, b1, b0, path.direction);
			for(auto i = 0u; i < ns; ++i) {
				buf[i] += b0[i];
			}
		} else {
			audio_->applyHRTF(nb, b1, buf, path.direction);
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
	float radius_ {0.5f}; // TODO: make variable
	tkn::Shared<nytl::Vec3f> updatePos_;
};

} // namespace tkn
