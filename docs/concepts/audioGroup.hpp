#include <tkn/sound.hpp>

// Allows grouping audio sources into groups.
// Useful for dynamically regulating the volume or applying effects
// to many sources.
class AudioSourceGroup : public tkn::AudioSource {
public:
	tkn::AudioSource& add(std::unique_ptr<tkn::AudioSource> impl);
	bool remove(const tkn::AudioSource& source);

	// Simply updates all its audio sources.
	void update() override;

	// Renders all its audio sources into the given buffer.
	void render(unsigned nb, float* buf, bool mix) override;

protected:
	// TODO: can't be implemented like this; thread-safety
	// between add/remove and update/render. And should be lockfree.
	// Probably need to use a tagged linked list and a destroyed_ list
	// as well, just as the audio player does it.
	// Could base the AudioPlayer implementation on this, i.e.
	// just have one AudioSourceGroup in the AudioPlayer.
	// std::vector<std::unique_ptr<tkn::AudioSource>> impls_;
};

