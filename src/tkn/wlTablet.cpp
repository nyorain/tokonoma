#include <tkn/wlTablet.hpp>
#include <tkn/types.hpp>
#include <dlg/dlg.hpp>
#include <cstring>

namespace tkn::wlt {

// pad strip
void handle_pad_strip_source(void *data,
		struct zwp_tablet_pad_strip_v2 *zwp_tablet_pad_strip_v2,
		uint32_t source) {
	// dlg_info("pad strip source {}", source);
}

void handle_pad_strip_position(void *data,
		 struct zwp_tablet_pad_strip_v2 *zwp_tablet_pad_strip_v2,
		 uint32_t position) {
	auto p = position / 65535.f;
	dlg_info("pad strip position {}", p);
}

void handle_pad_strip_stop(void *data,
		struct zwp_tablet_pad_strip_v2 *zwp_tablet_pad_strip_v2) {
	dlg_info("pad strip stop");
}

void handle_pad_strip_frame(void *data,
		struct zwp_tablet_pad_strip_v2 *zwp_tablet_pad_strip_v2,
		uint32_t time) {
}

const static struct zwp_tablet_pad_strip_v2_listener pad_strip_listener = {
	handle_pad_strip_source,
	handle_pad_strip_position,
	handle_pad_strip_stop,
	handle_pad_strip_frame,
};

// pad ring
void handle_pad_ring_source(void *data,
		struct zwp_tablet_pad_ring_v2 *zwp_tablet_pad_ring_v2,
		uint32_t source) {
	// dlg_info("pad ring source {}", source);
}

void handle_pad_ring_angle(void *data,
		struct zwp_tablet_pad_ring_v2 *zwp_tablet_pad_ring_v2,
		wl_fixed_t degrees) {
	Pad& pad = *static_cast<Pad*>(data);
	auto d = wl_fixed_to_double(degrees);
	if(pad.manager->listener) {
		pad.manager->listener->ringAngle(d);
	}

	// dlg_info("pad ring angle: {}", d);
}

void handle_pad_ring_stop(void *data,
		struct zwp_tablet_pad_ring_v2 *zwp_tablet_pad_ring_v2) {
	Pad& pad = *static_cast<Pad*>(data);
	if(pad.manager->listener) {
		pad.manager->listener->ringStop();
	}

	// dlg_info("pad ring stop");
}

void handle_pad_ring_frame(void* data,
		struct zwp_tablet_pad_ring_v2 *zwp_tablet_pad_ring_v2,
		uint32_t time) {
}

const static struct zwp_tablet_pad_ring_v2_listener pad_ring_listener = {
	handle_pad_ring_source,
	handle_pad_ring_angle,
	handle_pad_ring_stop,
	handle_pad_ring_frame,
};

// pad group
void handle_pad_group_buttons(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2,
		struct wl_array *buttons) {
}

void handle_pad_group_ring(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2,
		struct zwp_tablet_pad_ring_v2 *ring) {
	Pad& pad = *static_cast<Pad*>(data);
	zwp_tablet_pad_ring_v2_add_listener(ring, &pad_ring_listener, &pad);
	dlg_info("    >> group ring");
}

void handle_pad_group_strip(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2,
		struct zwp_tablet_pad_strip_v2 *strip) {
	Pad& pad = *static_cast<Pad*>(data);
	zwp_tablet_pad_strip_v2_add_listener(strip, &pad_strip_listener, &pad);
	dlg_info("    >> group strip");
}

void handle_pad_group_modes(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2,
		uint32_t modes) {
	dlg_info("    >> mode {}", modes);
}

void handle_pad_group_done(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2) {
	dlg_info("    >> done!");
}

void handle_pad_group_mode_switch(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2,
		uint32_t time,
		uint32_t serial,
		uint32_t mode) {
}

const static struct zwp_tablet_pad_group_v2_listener pad_group_listener = {
	handle_pad_group_buttons,
	handle_pad_group_ring,
	handle_pad_group_strip,
	handle_pad_group_modes,
	handle_pad_group_done,
	handle_pad_group_mode_switch,
};

// pad
void handle_pad_group(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2,
		struct zwp_tablet_pad_group_v2 *pad_group) {
	Pad& pad = *static_cast<Pad*>(data);
	zwp_tablet_pad_group_v2_add_listener(pad_group, &pad_group_listener, &pad);
	dlg_info("  >> group");
}

void handle_pad_path(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2,
		const char *path) {
	dlg_info("  >> path: {}", path);
}

void handle_pad_buttons(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2,
		uint32_t buttons) {
	dlg_info("  >> buttons: {}", buttons);
}

void handle_pad_done(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2) {
	dlg_info("  >> done!");
}

void handle_pad_button(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2,
		uint32_t time,
		uint32_t button,
		uint32_t state) {
	Pad& pad = *static_cast<Pad*>(data);
	if(pad.manager->listener) {
		pad.manager->listener->padButton(button, state);
	}

	// dlg_info("pad button {}: {}", button, state);
}

void handle_pad_enter(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2,
		uint32_t serial,
		struct zwp_tablet_v2 *tablet,
		struct wl_surface *surface) {
	dlg_info("pad enter");
}

void handle_pad_leave(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2,
		uint32_t serial,
		struct wl_surface *surface) {
	dlg_info("pad leave");
}

void handle_pad_removed(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2) {
	// TODO
}

static const struct zwp_tablet_pad_v2_listener pad_listener = {
	handle_pad_group,
	handle_pad_path,
	handle_pad_buttons,
	handle_pad_done,
	handle_pad_button,
	handle_pad_enter,
	handle_pad_leave,
	handle_pad_removed,
};

// tool
const char* tool_type_name(uint32_t type) {
	switch(type) {
		case ZWP_TABLET_TOOL_V2_TYPE_PEN: return "pen";
		case ZWP_TABLET_TOOL_V2_TYPE_ERASER: return "eraser";
		case ZWP_TABLET_TOOL_V2_TYPE_BRUSH: return "brush";
		case ZWP_TABLET_TOOL_V2_TYPE_PENCIL: return "pencil";
		case ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH: return "airbrush";
		case ZWP_TABLET_TOOL_V2_TYPE_FINGER: return "finger";
		case ZWP_TABLET_TOOL_V2_TYPE_MOUSE: return "mouse";
		case ZWP_TABLET_TOOL_V2_TYPE_LENS: return "lens";
		default: return "?";
	}
}

void handle_tool_type(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		uint32_t tool_type) {
	Tool& tool = *static_cast<Tool*>(data);
	tool.type = zwp_tablet_tool_v2_type(tool_type);

	dlg_info("  >> type: {} ({})", tool_type_name(tool_type), tool_type);
}

void handle_tool_hardware_serial(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		uint32_t hardware_serial_hi,
		uint32_t hardware_serial_lo) {
	u64 serial = u64(hardware_serial_hi) << 32 | u64(hardware_serial_lo);
	dlg_info("  >> hardware serial: {}{}", std::hex, serial);
}

void handle_tool_hardware_id_wacom(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		uint32_t hardware_id_hi,
		uint32_t hardware_id_lo) {
	u64 id = u64(hardware_id_hi) << 32 | u64(hardware_id_lo);
	dlg_info("  >> hardware id wacom: {}{}", std::hex, id);
}

const char* tool_cap_name(uint32_t capability) {
	switch(capability) {
		case ZWP_TABLET_TOOL_V2_CAPABILITY_TILT: return "tilt";
		case ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE: return "pressure";
		case ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE: return "distance";
		case ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION: return "rotation";
		case ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER: return "slider";
		case ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL: return "wheel";
		default: return "?";
	}
}

void handle_tool_capability(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		uint32_t capability) {
	Tool& tool = *static_cast<Tool*>(data);
	tool.caps.push_back(zwp_tablet_tool_v2_capability(capability));

	dlg_info("  >> cap: {} ({})", tool_cap_name(capability), capability);
}

void handle_tool_done(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2) {
	dlg_info("  >> done!");
}

void handle_tool_removed(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2) {
	// TODO
}

void handle_tool_proximity_in(void* data,
		struct zwp_tablet_tool_v2* zwp_tablet_tool_v2,
		uint32_t serial,
		struct zwp_tablet_v2* wlTablet,
		struct wl_surface* surface) {
	Tool& tool = *static_cast<Tool*>(data);
	if(tool.manager->listener) {
		tool.manager->listener->toolProximityIn();
	}

	// dlg_info("Proximity in");
}

void handle_tool_proximity_out(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2) {
	Tool& tool = *static_cast<Tool*>(data);
	if(tool.manager->listener) {
		tool.manager->listener->toolProximityOut();
	}

	// dlg_info("Proximity out");
}

void handle_tool_down(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		uint32_t serial) {
	Tool& tool = *static_cast<Tool*>(data);
	if(tool.manager->listener) {
		tool.manager->listener->toolDown();
	}

	// dlg_info("Tool down");
}

void handle_tool_up(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2) {
	Tool& tool = *static_cast<Tool*>(data);
	if(tool.manager->listener) {
		tool.manager->listener->toolUp();
	}

	// dlg_info("Tool up");
}

void handle_tool_motion(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		wl_fixed_t x,
		wl_fixed_t y) {
	auto dx = wl_fixed_to_double(x);
	auto dy = wl_fixed_to_double(y);

	Tool& tool = *static_cast<Tool*>(data);
	if(tool.manager->listener) {
		tool.manager->listener->toolMotion(dx, dy);
	}

	// dlg_info("Tool motion {} {}", dx, dy);
}

void handle_tool_pressure(void *data,
		 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		 uint32_t pressure) {
	Tool& tool = *static_cast<Tool*>(data);
	float p = pressure / 65535.f;

	if(tool.manager->listener) {
		tool.manager->listener->toolPressure(p);
	}

	// dlg_info("Tool pressure {}", p);
}

void handle_tool_distance(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		uint32_t distance) {
	Tool& tool = *static_cast<Tool*>(data);
	float d = distance / 65535.f;

	if(tool.manager->listener) {
		tool.manager->listener->toolDistance(d);
	}

	// dlg_info("Tool distance {}", d);
}

void handle_tool_tilt(void* data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		wl_fixed_t tilt_x,
		wl_fixed_t tilt_y) {
	auto x = wl_fixed_to_double(tilt_x);
	auto y = wl_fixed_to_double(tilt_y);

	Tool& tool = *static_cast<Tool*>(data);
	if(tool.manager->listener) {
		tool.manager->listener->toolTilt(x, y);
	}

	// dlg_info("Tool tilt {} {}", x, y);
}

void handle_tool_rotation(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		wl_fixed_t degrees) {
	auto d = wl_fixed_to_double(degrees);
	dlg_info("Tool rotation {}", d);
}

void handle_tool_slider(void *data,
		   struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		   int32_t position) {
	float p = position / 65535.f;
	dlg_info("Tool slider {}", p);
}

void handle_tool_wheel(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		wl_fixed_t degrees,
		int32_t clicks) {
	float d = wl_fixed_to_double(degrees);
	dlg_info("Tool wheel: degrees {}, clicks: {}", d, clicks);
}

void handle_tool_button(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		uint32_t serial,
		uint32_t button,
		uint32_t state) {
	Tool& tool = *static_cast<Tool*>(data);
	if(tool.manager->listener) {
		tool.manager->listener->toolButton(button, state);
	}

	// dlg_info("Tool button: {} (state {})", button, state);
}

void handle_tool_frame(void *data,
		struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
		uint32_t time) {
	// dlg_info("Tool frame!");
}

static const struct zwp_tablet_tool_v2_listener tool_listener = {
	handle_tool_type,
	handle_tool_hardware_serial,
	handle_tool_hardware_id_wacom,
	handle_tool_capability,
	handle_tool_done,
	handle_tool_removed,
	handle_tool_proximity_in,
	handle_tool_proximity_out,
	handle_tool_down,
	handle_tool_up,
	handle_tool_motion,
	handle_tool_pressure,
	handle_tool_distance,
	handle_tool_tilt,
	handle_tool_rotation,
	handle_tool_slider,
	handle_tool_wheel,
	handle_tool_button,
	handle_tool_frame,
};

// tablet
void handle_tablet_name(void *data,
		struct zwp_tablet_v2 *zwp_tablet_v2,
		const char *name) {
	dlg_info("  >> name: {}", name);
}

void handle_tablet_id(void *data,
		struct zwp_tablet_v2 *zwp_tablet_v2,
		uint32_t vid,
		uint32_t pid) {
	dlg_info("  >> vid: {}, pid {}", vid, pid);
}

void handle_tablet_path(void *data,
		struct zwp_tablet_v2 *zwp_tablet_v2,
		const char *path) {
	dlg_info("  >> path {}", path);
}

void handle_tablet_done(void *data,
		struct zwp_tablet_v2 *zwp_tablet_v2) {
	dlg_info("  >> done!");
}

void handle_tablet_removed(void *data,
		struct zwp_tablet_v2 *zwp_tablet_v2) {
	// TODO
}

static const struct zwp_tablet_v2_listener tablet_listener = {
	handle_tablet_name,
	handle_tablet_id,
	handle_tablet_path,
	handle_tablet_done,
	handle_tablet_removed,
};

void handle_pad_added(void* data,
		struct zwp_tablet_seat_v2* zwp_tablet_seat_v2,
		struct zwp_tablet_pad_v2* wlPad) {
	TabletManager& manager = *static_cast<TabletManager*>(data);
	dlg_assert(zwp_tablet_seat_v2 == manager.tabletSeat);

	auto& pad = *manager.pads.emplace_back(std::make_unique<Pad>());
	pad.pad = wlPad;
	pad.manager = &manager;

	zwp_tablet_pad_v2_add_listener(wlPad, &pad_listener, &pad);

	dlg_info("New Pad: {}", (void*) wlPad);
}

void handle_tool_added(void* data,
		struct zwp_tablet_seat_v2* zwp_tablet_seat_v2,
		struct zwp_tablet_tool_v2* wlTool) {
	TabletManager& manager = *static_cast<TabletManager*>(data);
	dlg_assert(zwp_tablet_seat_v2 == manager.tabletSeat);

	auto& tool = *manager.tools.emplace_back(std::make_unique<Tool>());
	tool.tool = wlTool;
	tool.manager = &manager;

	zwp_tablet_tool_v2_add_listener(wlTool, &tool_listener, &tool);

	dlg_info("New Tool: {}", (void*) wlTool);
}

void handle_tablet_added(void* data,
		struct zwp_tablet_seat_v2* zwp_tablet_seat_v2,
		struct zwp_tablet_v2* wlTablet) {
	TabletManager& manager = *static_cast<TabletManager*>(data);
	dlg_assert(zwp_tablet_seat_v2 == manager.tabletSeat);

	auto& tablet = *manager.tablets.emplace_back(std::make_unique<Tablet>());
	tablet.tablet = wlTablet;
	tablet.manager = &manager;

	zwp_tablet_v2_add_listener(wlTablet, &tablet_listener, &tablet);

	dlg_info("New Tablet: {}", (void*) wlTablet);
}

static const struct zwp_tablet_seat_v2_listener seat_listener = {
	handle_tablet_added,
	handle_tool_added,
	handle_pad_added,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	TabletManager& manager = *static_cast<TabletManager*>(data);

	if(!std::strcmp(interface, zwp_tablet_manager_v2_interface.name)) {
		unsigned v = std::min(version, 1u);
		manager.manager = static_cast<zwp_tablet_manager_v2*>(wl_registry_bind(
			registry, name, &zwp_tablet_manager_v2_interface, v));
		manager.tabletSeat = zwp_tablet_manager_v2_get_tablet_seat(manager.manager,
			manager.seat);

		zwp_tablet_seat_v2_add_listener(manager.tabletSeat, &seat_listener, &manager);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	// TODO
	(void) data;
	(void) registry;
	(void) name;
}

static const struct wl_registry_listener registry_listener = {
	handle_global,
	handle_global_remove,
};

void init(TabletManager& manager, struct wl_display* dpy, struct wl_seat* seat) {
	manager.dpy = dpy;
	manager.seat = seat;

	// we create an extra queue for the registry here so we don't have to
	// dispatch any other events, just waiting for the tablet manager
	manager.queue = wl_display_create_queue(dpy);
	manager.registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(manager.registry, &registry_listener, &manager);
	wl_proxy_set_queue((struct wl_proxy*) manager.registry, manager.queue);

	wl_display_roundtrip_queue(dpy, manager.queue);

	if(!manager.tabletSeat) {
		dlg_warn("Failed to get tablet manager seat");
		return;
	}

	// roundtrip once again for initial information
	wl_display_roundtrip_queue(dpy, manager.queue);
}

void dispatch(TabletManager& manager) {
	// NOTE: we rely on events being added to the queue (reading from the
	// fd) in some other place.
	wl_display_dispatch_queue_pending(manager.dpy, manager.queue);
}

TabletManager::~TabletManager() {
	// TODO
}

} // namespace tkn::wlt
