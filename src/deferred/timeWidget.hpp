#pragma once

#include <stage/types.hpp>
#include <vpp/handles.hpp>
#include <vpp/vk.hpp>
#include <rvg/context.hpp>
#include <rvg/text.hpp>
#include <rvg/paint.hpp>
#include <rvg/shapes.hpp>

using namespace doi::types;

struct TimeWidget {
public:
	static constexpr auto maxCount = 25u;

public:
	TimeWidget() = default;
	TimeWidget(rvg::Context& ctx, const rvg::Font& font);

	void updateDevice();
	void draw(vk::CommandBuffer cb);
	void start(vk::CommandBuffer cb, nytl::Vec2f position);
	void add(std::string name, vk::PipelineStageBits stage =
			vk::PipelineStageBits::bottomOfPipe);
	void finish();
	void hide(bool h);
	void toggleRelative();

	rvg::Context& rvgContext() const { return *ctx_; }
	const rvg::Font& font() const { return font_; }

protected:
	rvg::Context* ctx_;
	rvg::Font font_;
	vpp::QueryPool pool_;

	u32 bits_ {0xFFFFFFFFu}; // TODO
	float period_;

	float width_ {220};
	nytl::Vec2f pos_;
	float y_ {0};
	unsigned id_ {0};
	vk::CommandBuffer cb_;

	struct Entry {
		unsigned id;
		rvg::Text name;
		rvg::Text time;
	};

	std::vector<Entry> entries_;
	unsigned updateCounter_ {0};
	unsigned updateAfter_ {30};
	bool relative_ {true};

	rvg::RectShape bg_;
	rvg::Paint bgPaint_;
	rvg::Paint fgPaint_;

	rvg::Text totalName;
	rvg::Text totalTime;
};

