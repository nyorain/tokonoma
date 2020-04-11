#pragma once

#include <tkn/types.hpp>
#include <vpp/handles.hpp>
#include <vpp/vk.hpp>
#include <rvg/context.hpp>
#include <rvg/text.hpp>
#include <rvg/paint.hpp>
#include <rvg/shapes.hpp>

namespace tkn {

// NOTE: the api here is weird and maybe even a bit counter-intuitive.
// Not really sure though how this could be done better for now.
// We need to seperate the whole "register timing segment" thing
// from the timestamp recording (since the drawing of the time widget
// will/might happen in the same recording pass as the recording of
// timestamps and therefore timestamps *after* the recording of the
// render commands wouldn't be possible) but it can probably be
// done cleaner. Especially the fact that associating of timings with
// names is currently done based on order feels bad.
// The whole 'maxCount' thing feels bad as well.
// Naming of functions can be improved as well.

struct TimeWidget {
public:
	static constexpr unsigned maxCount = 25;
	static constexpr float width = 220;
	static constexpr float entryHeight = 20;
	static constexpr float labelWidth = 100;

public:
	TimeWidget() = default;
	TimeWidget(rvg::Context& ctx, const rvg::Font& font);

	void updateDevice();
	void draw(vk::CommandBuffer cb);
	void move(nytl::Vec2f pos);

	// Reset all previous registered timing segments.
	void reset();
	// Add a new timing segment. Should be done between a call to
	// reset() and complete(). Must be done before recording
	// the timestamps. The order in which addTiming will be used
	// to associate timestamps from addTimestamp later on with the
	// names passed here.
	void addTiming(std::string name);
	// Signal that all timing segments were added using addTiming.
	void complete();


	// Start a frame timing.
	void start(vk::CommandBuffer);
	// Record adding a timestamp at the given pipeline stage.
	// Will be associated with the timing (added using addTiming) by order
	// (i.e. the name of the first addTiming will be associated with the
	// first addTimestamp). Must only be called between 'start'
	// and 'finish'.
	void addTimestamp(vk::CommandBuffer, vk::PipelineStageBits =
		vk::PipelineStageBits::bottomOfPipe);
	// Finish a frame timing.
	void finish(vk::CommandBuffer);

	void hide(bool h);
	void toggleRelative();

	rvg::Context& rvgContext() const { return *ctx_; }
	const rvg::Font& font() const { return font_; }
	const auto& queryPool() const { return pool_; }
	bool valid() const { return ctx_; }

protected:
	rvg::Context* ctx_ {};
	rvg::Font font_;
	vpp::QueryPool pool_;

	u32 bits_ {0xFFFFFFFFu}; // TODO: actually care for valid timestamp bits
	float period_;

	float y_ {0};
	unsigned id_ {0};

	struct Entry {
		// unsigned id;
		rvg::Text name;
		rvg::Text time;
	};

	std::vector<Entry> entries_;
	unsigned updateCounter_ {0};
	// After how many frames the widget should be updated
	// TODO: rather make this dependent on time!
	unsigned updateAfter_ {30};
	bool relative_ {true};

	rvg::RectShape bg_;
	rvg::Paint bgPaint_;
	rvg::Paint fgPaint_;

	rvg::Text totalName;
	rvg::Text totalTime;
};

} // namespace tkn
