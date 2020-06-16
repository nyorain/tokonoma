#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/audio.hpp>
#include <tkn/sound.hpp>
#include <tkn/sampling.hpp>

class DummyAudioApp : public tkn::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		ap_.emplace("dummy audio");
		auto& ap = *ap_;

		auto asset = openAsset("test.ogg");
		audio_ = &ap.create<tkn::StreamedVorbisAudio>(ap, std::move(asset));

		ap.start();
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		((void) cb);
	}

	void update(double dt) override {
		App::update(dt);
	}

	bool touchBegin(const ny::TouchBeginEvent& ev) override {
		if(App::touchBegin(ev)) {
			return true;
		}

		audio_->volume(audio_->playing() ? 0.f : 1.f);
		return true;
	}

	const char* name() const override { return "dummy-audio"; }

protected:
	std::optional<tkn::AudioPlayer> ap_;
	tkn::StreamedVorbisAudio* audio_;
};

int main(int argc, const char** argv) {
	DummyAudioApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

