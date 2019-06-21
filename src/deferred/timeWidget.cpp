#include "timeWidget.hpp"
#include <dlg/dlg.hpp>

TimeWidget::TimeWidget(rvg::Context& ctx, const rvg::Font& font) :
		ctx_(&ctx), font_(font) {
	vk::QueryPoolCreateInfo qpi;
	qpi.queryCount = maxCount + 1;
	qpi.queryType = vk::QueryType::timestamp;
	pool_ = {ctx.device(), qpi};

	bgPaint_ = {rvgContext(), rvg::colorPaint({20, 20, 20, 200})};
	fgPaint_ = {rvgContext(), rvg::colorPaint({230, 230, 230, 255})};
	totalName = {rvgContext(), {}, "total:", font, 14.f};
	totalTime = {rvgContext(), {}, "...................", font, 14.f};

	auto dm = rvg::DrawMode {};
	dm.deviceLocal = true;
	dm.fill = true;
	dm.stroke = 0.f;
	dm.aaFill = false;
	bg_ = {rvgContext(), {}, {}, dm};
}

void TimeWidget::updateDevice() {
	if(++updateCounter_ < updateAfter_) {
		return;
	}

	dlg_assert(entries_.size() <= maxCount);
	dlg_assert(id_ <= maxCount);

	auto& dev = rvgContext().device();
	updateCounter_ = 0;

	std::uint32_t queries[maxCount + 1];
	vk::getQueryPoolResults(dev, pool_, 0, maxCount + 1,
		sizeof(queries), queries, 4, {});

	auto last = queries[0] & bits_;
	for(auto i = 0u; i < entries_.size(); ++i) {
		auto& entry = entries_[i];
		auto current = queries[i + 1] & bits_;
		std::string text;
		if(relative_) {
			double diff = current - last;
			diff *= dev.properties().limits.timestampPeriod; // ns
			diff /= 1000 * 1000; // ms
			text = "+" + std::to_string(diff);
			last = current;
		} else {
			current *= dev.properties().limits.timestampPeriod; // ns
			current /= 1000 * 1000; // ms
			text = std::to_string(current);
		}
		entry.time.change()->text = text;
		entry.time.updateDevice();
	}

	double diff = queries[id_] - queries[0];
	diff *= dev.properties().limits.timestampPeriod; // ns
	diff /= 1000 * 1000; // ms
	totalTime.change()->text = std::to_string(diff);
	totalTime.updateDevice();
}

void TimeWidget::draw(vk::CommandBuffer cb) {
	bgPaint_.bind(cb);
	bg_.fill(cb);

	fgPaint_.bind(cb);
	for(auto& e : entries_) {
		e.name.draw(cb);
		e.time.draw(cb);
	}

	totalName.draw(cb);
	totalTime.draw(cb);
}

void TimeWidget::start(vk::CommandBuffer cb, nytl::Vec2f position) {
	pos_ = position;

	entries_.clear();
	entries_.reserve(maxCount);
	updateCounter_ = updateAfter_;

	y_ = pos_.y + 20;
	id_ = 0;
	cb_ = cb;
	vk::cmdWriteTimestamp(cb_, vk::PipelineStageBits::bottomOfPipe,
		pool_, 0);
}

void TimeWidget::add(std::string name, vk::PipelineStageBits stage) {
	name += ":";
	auto& entry = entries_.emplace_back();
	auto pos = nytl::Vec2f{pos_.x + 20, y_};
	entry.name = {rvgContext(), pos, std::move(name), font(), 14.f};
	entry.name.updateDevice();
	pos.x += 100;
	entry.time = {rvgContext(), pos, "......................", font(), 14.f};
	entry.time.updateDevice();

	vk::cmdWriteTimestamp(cb_, stage, pool_, ++id_);
	y_ += 20;
}

void TimeWidget::finish() {
	auto x = pos_.x + 20;
	totalName.change()->position = {x, y_};
	totalName.updateDevice();
	x += 100;
	totalTime.change()->position = {x, y_};
	totalTime.updateDevice();

	y_ += 35;
	vk::cmdWriteTimestamp(cb_, vk::PipelineStageBits::bottomOfPipe,
		pool_, ++id_);
	cb_ = {};

	auto bgc = bg_.change();
	bgc->position = pos_;
	bgc->size.x = width_;
	bgc->size.y = y_ - pos_.y;
}

void TimeWidget::hide(bool h) {
	bg_.disable(h);
	totalName.disable(h);
	totalTime.disable(h);

	for(auto& e : entries_) {
		e.name.disable(h);
		e.time.disable(h);
	}
}

void TimeWidget::toggleRelative() {
	relative_ ^= true;
	updateCounter_ = updateAfter_;
}
