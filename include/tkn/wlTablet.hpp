#pragma once

#include <tkn/config.hpp>
#ifndef TKN_WITH_WL_PROTOS
	#error "wayland protocols required to use this header"
#endif

#include <tkn/tablet-unstable-v2-client-protocol.h> // generated

#include <vector>
#include <memory>

namespace tkn {
namespace wlt {

struct TabletManager;

// Listener for a vastly simplified model, compared to the model exposed
// by the wayland protocol. We assume we have just one tablet, just one tool.
struct Listener {
	virtual ~Listener() = default;

	virtual void toolDown() {}
	virtual void toolUp() {}
	virtual void toolProximityIn() {}
	virtual void toolProximityOut() {}
	virtual void toolDistance(float) {}
	virtual void toolPressure(float) {}
	virtual void toolMotion(float, float) {}
	virtual void toolTilt(float, float) {}
	virtual void toolButton(uint32_t, bool) {}

	virtual void ringAngle(float) {}
	virtual void ringStop() {}

	virtual void padButton(uint32_t, bool) {}
};

struct Tablet {
	TabletManager* manager {};
	zwp_tablet_v2* tablet {};
};

struct Tool {
	TabletManager* manager {};
	zwp_tablet_tool_v2* tool {};

	zwp_tablet_tool_v2_type type {};
	std::vector<zwp_tablet_tool_v2_capability> caps;
};

struct Pad {
	TabletManager* manager {};
	zwp_tablet_pad_v2* pad {};
};

struct TabletManager {
	wl_event_queue* queue {};
	wl_display* dpy {};
	wl_seat* seat {};
	wl_registry* registry {};

	zwp_tablet_manager_v2* manager {};
	zwp_tablet_seat_v2* tabletSeat {};

	std::vector<std::unique_ptr<Tablet>> tablets;
	std::vector<std::unique_ptr<Tool>> tools;
	std::vector<std::unique_ptr<Pad>> pads;

	Listener* listener {};

	~TabletManager();
};

void init(TabletManager&, struct wl_display*, struct wl_seat*);
void dispatch(TabletManager&);

} // namespace wlt
} // namespace tkn
