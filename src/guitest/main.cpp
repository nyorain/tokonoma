#include <tkn/singlePassApp.hpp>
#include <dlg/dlg.hpp>

#include <vui/gui.hpp>
#include <vui/dat.hpp>
#include <vui/textfield.hpp>
#include <vui/colorPicker.hpp>

class DummyApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	bool init(const nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		rvgInit();
		auto startPaint = rvg::Color {200, 150, 170};
		bgPaint_ = {rvgContext(), rvg::colorPaint(startPaint)};
		bgShape_ = {rvgContext(), {-1, -1}, {2, 2}, {true, 0.f}};

		auto& gui = this->gui();
		auto bounds = nytl::Rect2f {100, 100, vui::autoSize, vui::autoSize};
		auto& cp = gui.create<vui::ColorPicker>(bounds);
		cp.onChange = [&](auto& cp){
			bgPaint_.paint(rvg::colorPaint(cp.picked()));
			App::scheduleRedraw();
		};

		bounds.position = {100, 600};
		auto& tf = gui.create<vui::Textfield>(bounds);
		tf.onSubmit = [&](auto& tf) {
			dlg_info("submitted: {}", tf.utf8());
		};

		tf.onCancel = [&](auto& tf) {
			dlg_info("cancelled: {}", tf.utf8());
		};

		bounds.position = {100, 500};
		auto& btn = gui.create<vui::LabeledButton>(bounds, "Waddup my man");
		btn.onClick = [&](auto&) {
			dlg_info("button pressed");
		};

		auto pos = nytl::Vec2f {500, 0};
		auto& panel = gui.create<vui::dat::Panel>(pos, 300.f);

		auto& f1 = panel.create<vui::dat::Folder>("folder 1");
		auto& b1 = f1.create<vui::dat::Button>("button 1");
		b1.onClick = [&](){ dlg_info("click 1"); };
		f1.create<vui::dat::Button>("button 4");
		f1.create<vui::dat::Button>("button 5");

		auto& f2 = panel.create<vui::dat::Folder>("folder 2");
		auto& b2 = f2.create<vui::dat::Button>("button 2");
		b2.onClick = [&](){ dlg_info("click 2"); };

		auto& nf1 = f2.create<vui::dat::Folder>("nested folder 1");
		auto& b3 = nf1.create<vui::dat::Button>("button 3");
		b3.onClick = [&](){ dlg_info("click 3"); };

		auto& b6 = nf1.create<vui::dat::Button>("button 6");

		auto& b7 = nf1.create<vui::dat::Button>("button 7");
		b7.onClick = [&]{
			if(removed6_) {
				nf1.add(std::move(removed6_));
			} else {
				removed6_ = nf1.remove(b6);
			}
		};

		nf1.create<vui::dat::Button>("button 8");

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		rvgContext().bindDefaults(cb);
		bgPaint_.bind(cb);
		bgShape_.fill(cb);
		gui().draw(cb);
	}

	const char* name() const override { return "guitest"; }

protected:
	rvg::Paint bgPaint_;
	rvg::RectShape bgShape_;
	std::unique_ptr<vui::Widget> removed6_ {};
};

int main(int argc, const char** argv) {
	return tkn::appMain<DummyApp>(argc, argv);
}
