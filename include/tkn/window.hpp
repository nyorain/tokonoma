// Copyright (c) 2017 nyorain
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt

#pragma once

#include <ny/fwd.hpp>
#include <ny/windowListener.hpp>
#include <nytl/vec.hpp>
#include <vpp/fwd.hpp>
#include <functional>

namespace tkn {

// Represents the main rendering window.
class [[deprecated("will be removed, use swa instead")]]
MainWindow : public ny::WindowListener {
public:
	std::function<void(const ny::KeyEvent&)> onKey;
	std::function<void(const ny::FocusEvent&)> onFocus;
	std::function<void(const ny::MouseMoveEvent&)> onMouseMove;
	std::function<void(const ny::MouseButtonEvent&)> onMouseButton;
	std::function<void(const ny::MouseWheelEvent&)> onMouseWheel;
	std::function<void(const ny::MouseCrossEvent&)> onMouseCross;
	std::function<void(const ny::SizeEvent&)> onResize;
	std::function<void(const ny::CloseEvent&)> onClose;
	std::function<void(const ny::DrawEvent&)> onDraw;
	std::function<void(const ny::TouchBeginEvent&)> onTouchBegin;
	std::function<void(const ny::TouchUpdateEvent&)> onTouchUpdate;
	std::function<void(const ny::TouchCancelEvent&)> onTouchCancel;
	std::function<void(const ny::TouchEndEvent&)> onTouchEnd;
	std::function<void()> onSurfaceDestroyed;
	std::function<void(vk::SurfaceKHR)> onSurfaceCreated;

public:
	MainWindow(ny::AppContext& ac, vk::Instance instance);
	~MainWindow();

	ny::AppContext& appContext() const { return ac_; }
	ny::WindowContext& windowContext() const { return *wc_; }
	vk::SurfaceKHR vkSurface() const { return surface_; }
	nytl::Vec2ui size() const { return size_; }
	bool focus() const { return focus_; }
	bool mouseOver() const { return mouseOver_; }

protected:
	void mouseButton(const ny::MouseButtonEvent&) override;
	void mouseWheel(const ny::MouseWheelEvent&) override;
	void mouseMove(const ny::MouseMoveEvent&) override;
	void mouseCross(const ny::MouseCrossEvent&) override;
	void touchBegin(const ny::TouchBeginEvent&) override;
	void touchEnd(const ny::TouchEndEvent&) override;
	void touchUpdate(const ny::TouchUpdateEvent&) override;
	void touchCancel(const ny::TouchCancelEvent&) override;
	void key(const ny::KeyEvent&) override;
	void state(const ny::StateEvent&) override;
	void close(const ny::CloseEvent&) override;
	void resize(const ny::SizeEvent&) override;
	void focus(const ny::FocusEvent&) override;
	void draw(const ny::DrawEvent&) override;
	void surfaceDestroyed(const ny::SurfaceDestroyedEvent&) override;
	void surfaceCreated(const ny::SurfaceCreatedEvent&) override;

protected:
	ny::AppContext& ac_;
	std::unique_ptr<ny::WindowContext> wc_;

	vk::SurfaceKHR surface_ {};
	nytl::Vec2ui size_ {2340, 1080};
	ny::ToplevelState state_ {};
	bool focus_ {};
	bool mouseOver_ {};
};

} // namespace tkn
