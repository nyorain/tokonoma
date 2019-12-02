#include <tkn/audio.hpp>
#include <tkn/audio3D.hpp>
#include <tkn/sound.hpp>
#include <tkn/sampling.hpp>
#include <dlg/dlg.hpp>

class AudioPlayer : public tkn::AudioPlayer {
public:
	using tkn::AudioPlayer::AudioPlayer;

	tkn::Audio3D* audio {};
	void renderUpdate() override {
		if(audio) {
			audio->update();
		}
	}
};

