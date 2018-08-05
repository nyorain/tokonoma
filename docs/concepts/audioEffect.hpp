// to be added to audio.hpp

/// Interface that can be used to manipulate mixed sound.
class AudioEffect {
public:
	virtual ~AudioEffect() = default;

	/// Called after all audios have been rendered into the given
	/// buffers, can freely manipulate their content.
	virtual void process(float& buffer, unsigned samples) = 0;
};

