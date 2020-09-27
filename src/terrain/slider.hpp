#pragma once

#include <vui/widget.hpp>
#include <rvg/paint.hpp>
#include <rvg/shapes.hpp>

class FloatSlider : public vui::Widget {
public:
	std::function<void(FloatSlider&)> onChange;

public:
	FloatSlider(vui::Gui&, vui::ContainerWidget*, const nytl::Rect2f& bounds,
		float* value = nullptr);

	void hide(bool h) override;
	bool hidden() const override;
	void bounds(const nytl::Rect2f& bounds) override;
	void draw(vk::CommandBuffer cb) const override;

protected:
	float* ptr_;
	float value_ {};
	float min_ {};
	float max_ {};

	rvg::RectShape bg_;
	rvg::RectShape fg_;
	rvg::Paint bgPaint_;
	rvg::Paint fgPaint_;
};

