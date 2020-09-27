#include "slider.hpp"

FloatSlider::FloatSlider(vui::Gui&, vui::ContainerWidget*, const nytl::Rect2f& bounds,
	float* value = nullptr);

void FloatSlider::hide(bool h) override;
bool FloatSlider::hidden() const override;
void FloatSlider::bounds(const nytl::Rect2f& bounds) override;
void FloatSlider::draw(vk::CommandBuffer cb) const override;
