#include <json.h>
#include <libevdev/libevdev.h>
#include <stdio.h>
#include <ctype.h>
#include "config.h"
#include "log.h"
#include "sway/config.h"
#include "sway/ipc-json.h"
#include "sway/tree/container.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "sway/output.h"
#include "sway/input/input-manager.h"
#include "sway/input/cursor.h"
#include "sway/input/keyboard.h"
#include "sway/input/seat.h"
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_output.h>
#include <xkbcommon/xkbcommon.h>
#include "wlr-layer-shell-unstable-v1-protocol.h"

static const int i3_output_id = INT32_MAX;
static const int i3_scratch_id = INT32_MAX - 1;

static const char *ipc_json_layout_description(enum sway_container_layout l) {
	switch (l) {
	case L_VERT:
		return "splitv";
	case L_HORIZ:
		return "splith";
	case L_TABBED:
		return "tabbed";
	case L_STACKED:
		return "stacked";
	case L_NONE:
		break;
	}
	return "none";
}

static const char *ipc_json_orientation_description(enum sway_container_layout l) {
	switch (l) {
	case L_VERT:
		return "vertical";
	case L_HORIZ:
		return "horizontal";
	default:
		return "none";
	}
}

static const char *ipc_json_border_description(enum sway_container_border border) {
	switch (border) {
	case B_NONE:
		return "none";
	case B_PIXEL:
		return "pixel";
	case B_NORMAL:
		return "normal";
	case B_CSD:
		return "csd";
	}
	return "unknown";
}

static const char *ipc_json_output_transform_description(enum wl_output_transform transform) {
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
		return "normal";
	case WL_OUTPUT_TRANSFORM_90:
		return "90";
	case WL_OUTPUT_TRANSFORM_180:
		return "180";
	case WL_OUTPUT_TRANSFORM_270:
		return "270";
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		return "flipped";
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		return "flipped-90";
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		return "flipped-180";
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		return "flipped-270";
	}
	return NULL;
}

static const char *ipc_json_device_type_description(struct sway_input_device *device) {
	switch (device->wlr_device->type) {
	case WLR_INPUT_DEVICE_POINTER:
		return "pointer";
	case WLR_INPUT_DEVICE_KEYBOARD:
		return "keyboard";
	case WLR_INPUT_DEVICE_TOUCH:
		return "touch";
	case WLR_INPUT_DEVICE_TABLET_TOOL:
		return "tablet_tool";
	case WLR_INPUT_DEVICE_TABLET_PAD:
		return "tablet_pad";
	case WLR_INPUT_DEVICE_SWITCH:
		return "switch";
	}
	return "unknown";
}

json_object *ipc_json_get_version(void) {
	int major = 0, minor = 0, patch = 0;
	json_object *version = json_object_new_object();

	sscanf(SWAY_VERSION, "%u.%u.%u", &major, &minor, &patch);

	json_object_object_add(version, "human_readable", json_object_new_string(SWAY_VERSION));
	json_object_object_add(version, "variant", json_object_new_string("sway"));
	json_object_object_add(version, "major", json_object_new_int(major));
	json_object_object_add(version, "minor", json_object_new_int(minor));
	json_object_object_add(version, "patch", json_object_new_int(patch));
	json_object_object_add(version, "loaded_config_file_name", json_object_new_string(config->current_config_path));

	return version;
}

static json_object *ipc_json_create_rect(struct wlr_box *box) {
	json_object *rect = json_object_new_object();

	json_object_object_add(rect, "x", json_object_new_int(box->x));
	json_object_object_add(rect, "y", json_object_new_int(box->y));
	json_object_object_add(rect, "width", json_object_new_int(box->width));
	json_object_object_add(rect, "height", json_object_new_int(box->height));

	return rect;
}

static json_object *ipc_json_create_empty_rect(void) {
	struct wlr_box empty = {0, 0, 0, 0};

	return ipc_json_create_rect(&empty);
}

static json_object *ipc_json_create_node(int id, char *name,
		bool focused, json_object *focus, struct wlr_box *box) {
	json_object *object = json_object_new_object();

	json_object_object_add(object, "id", json_object_new_int(id));
	json_object_object_add(object, "name",
			name ? json_object_new_string(name) : NULL);
	json_object_object_add(object, "rect", ipc_json_create_rect(box));
	json_object_object_add(object, "focused", json_object_new_boolean(focused));
	json_object_object_add(object, "focus", focus);

	// set default values to be compatible with i3
	json_object_object_add(object, "border",
			json_object_new_string(
				ipc_json_border_description(B_NONE)));
	json_object_object_add(object, "current_border_width",
			json_object_new_int(0));
	json_object_object_add(object, "layout",
			json_object_new_string(
				ipc_json_layout_description(L_HORIZ)));
	json_object_object_add(object, "orientation",
			json_object_new_string(
				ipc_json_orientation_description(L_HORIZ)));
	json_object_object_add(object, "percent", NULL);
	json_object_object_add(object, "window_rect", ipc_json_create_empty_rect());
	json_object_object_add(object, "deco_rect", ipc_json_create_empty_rect());
	json_object_object_add(object, "geometry", ipc_json_create_empty_rect());
	json_object_object_add(object, "window", NULL);
	json_object_object_add(object, "urgent", json_object_new_boolean(false));
	json_object_object_add(object, "floating_nodes", json_object_new_array());
	json_object_object_add(object, "sticky", json_object_new_boolean(false));

	return object;
}

static void ipc_json_describe_root(struct sway_root *root, json_object *object) {
	json_object_object_add(object, "type", json_object_new_string("root"));
}

static void ipc_json_describe_output(struct sway_output *output,
		json_object *object) {
	struct wlr_output *wlr_output = output->wlr_output;
	json_object_object_add(object, "type", json_object_new_string("output"));
	json_object_object_add(object, "active", json_object_new_boolean(true));
	json_object_object_add(object, "primary", json_object_new_boolean(false));
	json_object_object_add(object, "layout", json_object_new_string("output"));
	json_object_object_add(object, "orientation",
			json_object_new_string(
				ipc_json_orientation_description(L_NONE)));
	json_object_object_add(object, "make",
			json_object_new_string(wlr_output->make));
	json_object_object_add(object, "model",
			json_object_new_string(wlr_output->model));
	json_object_object_add(object, "serial",
			json_object_new_string(wlr_output->serial));
	json_object_object_add(object, "scale",
			json_object_new_double(wlr_output->scale));
	json_object_object_add(object, "transform",
		json_object_new_string(
			ipc_json_output_transform_description(wlr_output->transform)));

	struct sway_workspace *ws = output_get_active_workspace(output);
	if (!sway_assert(ws, "Expected output to have a workspace")) {
		return;
	}
	json_object_object_add(object, "current_workspace",
			json_object_new_string(ws->name));

	json_object *modes_array = json_object_new_array();
	struct wlr_output_mode *mode;
	wl_list_for_each(mode, &wlr_output->modes, link) {
		json_object *mode_object = json_object_new_object();
		json_object_object_add(mode_object, "width",
			json_object_new_int(mode->width));
		json_object_object_add(mode_object, "height",
			json_object_new_int(mode->height));
		json_object_object_add(mode_object, "refresh",
			json_object_new_int(mode->refresh));
		json_object_array_add(modes_array, mode_object);
	}

	json_object_object_add(object, "modes", modes_array);

	json_object *current_mode_object = json_object_new_object();
	json_object_object_add(current_mode_object, "width",
		json_object_new_int(wlr_output->width));
	json_object_object_add(current_mode_object, "height",
		json_object_new_int(wlr_output->height));
	json_object_object_add(current_mode_object, "refresh",
		json_object_new_int(wlr_output->refresh));
	json_object_object_add(object, "current_mode", current_mode_object);

	struct sway_node *parent = node_get_parent(&output->node);
	struct wlr_box parent_box = {0, 0, 0, 0};

	if (parent != NULL) {
		node_get_box(parent, &parent_box);
	}

	if (parent_box.width != 0 && parent_box.height != 0) {
		double percent = ((double)output->width / parent_box.width)
				* ((double)output->height / parent_box.height);
		json_object_object_add(object, "percent", json_object_new_double(percent));
	}
}

json_object *ipc_json_describe_disabled_output(struct sway_output *output) {
	struct wlr_output *wlr_output = output->wlr_output;

	json_object *object = json_object_new_object();

	json_object_object_add(object, "type", json_object_new_string("output"));
	json_object_object_add(object, "name",
			json_object_new_string(wlr_output->name));
	json_object_object_add(object, "active", json_object_new_boolean(false));
	json_object_object_add(object, "primary", json_object_new_boolean(false));
	json_object_object_add(object, "make",
			json_object_new_string(wlr_output->make));
	json_object_object_add(object, "model",
			json_object_new_string(wlr_output->model));
	json_object_object_add(object, "serial",
			json_object_new_string(wlr_output->serial));
	json_object_object_add(object, "modes", json_object_new_array());

	json_object_object_add(object, "current_workspace", NULL);

	json_object *rect_object = json_object_new_object();
	json_object_object_add(rect_object, "x", json_object_new_int(0));
	json_object_object_add(rect_object, "y", json_object_new_int(0));
	json_object_object_add(rect_object, "width", json_object_new_int(0));
	json_object_object_add(rect_object, "height", json_object_new_int(0));
	json_object_object_add(object, "rect", rect_object);

	json_object_object_add(object, "percent", NULL);

	return object;
}

static json_object *ipc_json_describe_scratchpad_output(void) {
	struct wlr_box box;
	root_get_box(root, &box);

	// Create focus stack for __i3_scratch workspace
	json_object *workspace_focus = json_object_new_array();
	for (int i = root->scratchpad->length - 1; i >= 0; --i) {
		struct sway_container *container = root->scratchpad->items[i];
		json_object_array_add(workspace_focus,
				json_object_new_int(container->node.id));
	}

	json_object *workspace = ipc_json_create_node(i3_scratch_id,
				"__i3_scratch", false, workspace_focus, &box);
	json_object_object_add(workspace, "type",
			json_object_new_string("workspace"));

	// List all hidden scratchpad containers as floating nodes
	json_object *floating_array = json_object_new_array();
	for (int i = 0; i < root->scratchpad->length; ++i) {
		struct sway_container *container = root->scratchpad->items[i];
		if (container_is_scratchpad_hidden(container)) {
			json_object_array_add(floating_array,
				ipc_json_describe_node_recursive(&container->node));
		}
	}
	json_object_object_add(workspace, "floating_nodes", floating_array);

	// Create focus stack for __i3 output
	json_object *output_focus = json_object_new_array();
	json_object_array_add(output_focus, json_object_new_int(i3_scratch_id));

	json_object *output = ipc_json_create_node(i3_output_id,
					"__i3", false, output_focus, &box);
	json_object_object_add(output, "type",
			json_object_new_string("output"));
	json_object_object_add(output, "layout",
			json_object_new_string("output"));

	json_object *nodes = json_object_new_array();
	json_object_array_add(nodes, workspace);
	json_object_object_add(output, "nodes", nodes);

	return output;
}

static void ipc_json_describe_workspace(struct sway_workspace *workspace,
		json_object *object) {
	int num = isdigit(workspace->name[0]) ? atoi(workspace->name) : -1;

	json_object_object_add(object, "num", json_object_new_int(num));
	json_object_object_add(object, "output", workspace->output ?
			json_object_new_string(workspace->output->wlr_output->name) : NULL);
	json_object_object_add(object, "type", json_object_new_string("workspace"));
	json_object_object_add(object, "urgent",
			json_object_new_boolean(workspace->urgent));
	json_object_object_add(object, "representation", workspace->representation ?
			json_object_new_string(workspace->representation) : NULL);

	json_object_object_add(object, "layout",
			json_object_new_string(
				ipc_json_layout_description(workspace->layout)));
	json_object_object_add(object, "orientation",
			json_object_new_string(
				ipc_json_orientation_description(workspace->layout)));

	// Floating
	json_object *floating_array = json_object_new_array();
	for (int i = 0; i < workspace->floating->length; ++i) {
		struct sway_container *floater = workspace->floating->items[i];
		json_object_array_add(floating_array,
				ipc_json_describe_node_recursive(&floater->node));
	}
	json_object_object_add(object, "floating_nodes", floating_array);
}

static void get_deco_rect(struct sway_container *c, struct wlr_box *deco_rect) {
	enum sway_container_layout parent_layout = container_parent_layout(c);
	if ((parent_layout != L_TABBED && parent_layout != L_STACKED &&
			c->current.border != B_NORMAL) ||
			c->fullscreen_mode != FULLSCREEN_NONE) {
		deco_rect->x = deco_rect->y = deco_rect->width = deco_rect->height = 0;
		return;
	}

	if (c->parent) {
		deco_rect->x = c->x - c->parent->x;
		deco_rect->y = c->y - c->parent->y;
	} else {
		deco_rect->x = c->x - c->workspace->x;
		deco_rect->y = c->y - c->workspace->y;
	}
	deco_rect->width = c->width;
	deco_rect->height = container_titlebar_height();

	if (parent_layout == L_TABBED) {
		deco_rect->width = c->parent
			? c->parent->width / c->parent->children->length
			: c->workspace->width / c->workspace->tiling->length;
		deco_rect->x += deco_rect->width * container_sibling_index(c);
	} else if (container_parent_layout(c) == L_STACKED) {
		if (!c->view) {
			size_t siblings = container_get_siblings(c)->length;
			deco_rect->y -= deco_rect->height * siblings;
		}
		deco_rect->y += deco_rect->height * container_sibling_index(c);
	}
}

static void ipc_json_describe_view(struct sway_container *c, json_object *object) {
	json_object_object_add(object, "pid", json_object_new_int(c->view->pid));

	const char *app_id = view_get_app_id(c->view);
	json_object_object_add(object, "app_id",
			app_id ? json_object_new_string(app_id) : NULL);

	bool visible = view_is_visible(c->view);
	json_object_object_add(object, "visible", json_object_new_boolean(visible));

	json_object *marks = json_object_new_array();
	list_t *con_marks = c->marks;
	for (int i = 0; i < con_marks->length; ++i) {
		json_object_array_add(marks, json_object_new_string(con_marks->items[i]));
	}

	json_object_object_add(object, "marks", marks);

	struct wlr_box window_box = {
		c->content_x - c->x,
		(c->current.border == B_PIXEL) ? c->current.border_thickness : 0,
		c->content_width,
		c->content_height
	};

	json_object_object_add(object, "window_rect", ipc_json_create_rect(&window_box));

	struct wlr_box geometry = {0, 0, c->view->natural_width, c->view->natural_height};
	json_object_object_add(object, "geometry", ipc_json_create_rect(&geometry));

#if HAVE_XWAYLAND
	if (c->view->type == SWAY_VIEW_XWAYLAND) {
		json_object_object_add(object, "window",
				json_object_new_int(view_get_x11_window_id(c->view)));

		json_object *window_props = json_object_new_object();

		const char *class = view_get_class(c->view);
		if (class) {
			json_object_object_add(window_props, "class", json_object_new_string(class));
		}
		const char *instance = view_get_instance(c->view);
		if (instance) {
			json_object_object_add(window_props, "instance", json_object_new_string(instance));
		}
		if (c->title) {
			json_object_object_add(window_props, "title", json_object_new_string(c->title));
		}

		// the transient_for key is always present in i3's output
		uint32_t parent_id = view_get_x11_parent_id(c->view);
		json_object_object_add(window_props, "transient_for",
				parent_id ? json_object_new_int(parent_id) : NULL);

		const char *role = view_get_window_role(c->view);
		if (role) {
			json_object_object_add(window_props, "window_role", json_object_new_string(role));
		}

		json_object_object_add(object, "window_properties", window_props);
	}
#endif
}

static void ipc_json_describe_container(struct sway_container *c, json_object *object) {
	json_object_object_add(object, "name",
			c->title ? json_object_new_string(c->title) : NULL);
	json_object_object_add(object, "type",
			json_object_new_string(container_is_floating(c) ? "floating_con" : "con"));

	json_object_object_add(object, "layout",
			json_object_new_string(
				ipc_json_layout_description(c->layout)));

	json_object_object_add(object, "orientation",
			json_object_new_string(
				ipc_json_orientation_description(c->layout)));

	bool urgent = c->view ?
		view_is_urgent(c->view) : container_has_urgent_child(c);
	json_object_object_add(object, "urgent", json_object_new_boolean(urgent));
	json_object_object_add(object, "sticky", json_object_new_boolean(c->is_sticky));

	json_object_object_add(object, "fullscreen_mode",
			json_object_new_int(c->fullscreen_mode));

	struct sway_node *parent = node_get_parent(&c->node);
	struct wlr_box parent_box = {0, 0, 0, 0};

	if (parent != NULL) {
		node_get_box(parent, &parent_box);
	}

	if (parent_box.width != 0 && parent_box.height != 0) {
		double percent = ((double)c->width / parent_box.width)
				* ((double)c->height / parent_box.height);
		json_object_object_add(object, "percent", json_object_new_double(percent));
	}

	json_object_object_add(object, "border",
			json_object_new_string(
				ipc_json_border_description(c->current.border)));
	json_object_object_add(object, "current_border_width",
			json_object_new_int(c->current.border_thickness));
	json_object_object_add(object, "floating_nodes", json_object_new_array());

	struct wlr_box deco_box = {0, 0, 0, 0};
	get_deco_rect(c, &deco_box);
	json_object_object_add(object, "deco_rect", ipc_json_create_rect(&deco_box));

	if (c->view) {
		ipc_json_describe_view(c, object);
	}
}

struct focus_inactive_data {
	struct sway_node *node;
	json_object *object;
};

static void focus_inactive_children_iterator(struct sway_node *node,
		void *_data) {
	struct focus_inactive_data *data = _data;
	json_object *focus = data->object;
	if (data->node == &root->node) {
		struct sway_output *output = node_get_output(node);
		if (output == NULL) {
			return;
		}
		size_t id = output->node.id;
		int len = json_object_array_length(focus);
		for (int i = 0; i < len; ++i) {
			if ((size_t) json_object_get_int(json_object_array_get_idx(focus, i)) == id) {
				return;
			}
		}
		node = &output->node;
	} else if (node_get_parent(node) != data->node) {
		return;
	}
	json_object_array_add(focus, json_object_new_int(node->id));
}

json_object *ipc_json_describe_node(struct sway_node *node) {
	struct sway_seat *seat = input_manager_get_default_seat();
	bool focused = seat_get_focus(seat) == node;
	char *name = node_get_name(node);

	struct wlr_box box;
	node_get_box(node, &box);
	if (node->type == N_CONTAINER) {
		struct wlr_box deco_rect = {0, 0, 0, 0};
		get_deco_rect(node->sway_container, &deco_rect);
		size_t count = 1;
		if (container_parent_layout(node->sway_container) == L_STACKED) {
			count = container_get_siblings(node->sway_container)->length;
		}
		box.y += deco_rect.height * count;
		box.height -= deco_rect.height * count;
	}

	json_object *focus = json_object_new_array();
	struct focus_inactive_data data = {
		.node = node,
		.object = focus,
	};
	seat_for_each_node(seat, focus_inactive_children_iterator, &data);

	json_object *object = ipc_json_create_node(
				(int)node->id, name, focused, focus, &box);

	switch (node->type) {
	case N_ROOT:
		ipc_json_describe_root(root, object);
		break;
	case N_OUTPUT:
		ipc_json_describe_output(node->sway_output, object);
		break;
	case N_CONTAINER:
		ipc_json_describe_container(node->sway_container, object);
		break;
	case N_WORKSPACE:
		ipc_json_describe_workspace(node->sway_workspace, object);
		break;
	}

	return object;
}

json_object *ipc_json_describe_node_recursive(struct sway_node *node) {
	json_object *object = ipc_json_describe_node(node);
	int i;

	json_object *children = json_object_new_array();
	switch (node->type) {
	case N_ROOT:
		json_object_array_add(children,
				ipc_json_describe_scratchpad_output());
		for (i = 0; i < root->outputs->length; ++i) {
			struct sway_output *output = root->outputs->items[i];
			json_object_array_add(children,
					ipc_json_describe_node_recursive(&output->node));
		}
		break;
	case N_OUTPUT:
		for (i = 0; i < node->sway_output->workspaces->length; ++i) {
			struct sway_workspace *ws = node->sway_output->workspaces->items[i];
			json_object_array_add(children,
					ipc_json_describe_node_recursive(&ws->node));
		}
		break;
	case N_WORKSPACE:
		for (i = 0; i < node->sway_workspace->tiling->length; ++i) {
			struct sway_container *con = node->sway_workspace->tiling->items[i];
			json_object_array_add(children,
					ipc_json_describe_node_recursive(&con->node));
		}
		break;
	case N_CONTAINER:
		if (node->sway_container->children) {
			for (i = 0; i < node->sway_container->children->length; ++i) {
				struct sway_container *child =
					node->sway_container->children->items[i];
				json_object_array_add(children,
						ipc_json_describe_node_recursive(&child->node));
			}
		}
		break;
	}
	json_object_object_add(object, "nodes", children);

	return object;
}

static json_object *describe_libinput_device(struct libinput_device *device) {
	json_object *object = json_object_new_object();

	const char *events = "unknown";
	switch (libinput_device_config_send_events_get_mode(device)) {
	case LIBINPUT_CONFIG_SEND_EVENTS_ENABLED:
		events = "enabled";
		break;
	case LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE:
		events = "disabled_on_external_mouse";
		break;
	case LIBINPUT_CONFIG_SEND_EVENTS_DISABLED:
		events = "disabled";
		break;
	}
	json_object_object_add(object, "send_events",
			json_object_new_string(events));

	if (libinput_device_config_tap_get_finger_count(device) > 0) {
		const char *tap = "unknown";
		switch (libinput_device_config_tap_get_enabled(device)) {
		case LIBINPUT_CONFIG_TAP_ENABLED:
			tap = "enabled";
			break;
		case LIBINPUT_CONFIG_TAP_DISABLED:
			tap = "disabled";
			break;
		}
		json_object_object_add(object, "tap", json_object_new_string(tap));

		const char *button_map = "unknown";
		switch (libinput_device_config_tap_get_button_map(device)) {
		case LIBINPUT_CONFIG_TAP_MAP_LRM:
			button_map = "lrm";
			break;
		case LIBINPUT_CONFIG_TAP_MAP_LMR:
			button_map = "lmr";
			break;
		}
		json_object_object_add(object, "tap_button_map",
				json_object_new_string(button_map));

		const char* drag = "unknown";
		switch (libinput_device_config_tap_get_drag_enabled(device)) {
		case LIBINPUT_CONFIG_DRAG_ENABLED:
			drag = "enabled";
			break;
		case LIBINPUT_CONFIG_DRAG_DISABLED:
			drag = "disabled";
			break;
		}
		json_object_object_add(object, "tap_drag",
				json_object_new_string(drag));

		const char *drag_lock = "unknown";
		switch (libinput_device_config_tap_get_drag_lock_enabled(device)) {
		case LIBINPUT_CONFIG_DRAG_LOCK_ENABLED:
			drag_lock = "enabled";
			break;
		case LIBINPUT_CONFIG_DRAG_LOCK_DISABLED:
			drag_lock = "disabled";
			break;
		}
		json_object_object_add(object, "tap_drag_lock",
				json_object_new_string(drag_lock));
	}

	if (libinput_device_config_accel_is_available(device)) {
		double accel = libinput_device_config_accel_get_speed(device);
		json_object_object_add(object, "accel_speed",
				json_object_new_double(accel));

		const char *accel_profile = "unknown";
		switch (libinput_device_config_accel_get_profile(device)) {
		case LIBINPUT_CONFIG_ACCEL_PROFILE_NONE:
			accel_profile = "none";
			break;
		case LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT:
			accel_profile = "flat";
			break;
		case LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE:
			accel_profile = "adaptive";
			break;
		}
		json_object_object_add(object, "accel_profile",
				json_object_new_string(accel_profile));
	}

	if (libinput_device_config_scroll_has_natural_scroll(device)) {
		const char *natural_scroll = "disabled";
		if (libinput_device_config_scroll_get_natural_scroll_enabled(device)) {
			natural_scroll = "enabled";
		}
		json_object_object_add(object, "natural_scroll",
				json_object_new_string(natural_scroll));
	}

	if (libinput_device_config_left_handed_is_available(device)) {
		const char *left_handed = "disabled";
		if (libinput_device_config_left_handed_get(device) != 0) {
			left_handed = "enabled";
		}
		json_object_object_add(object, "left_handed",
				json_object_new_string(left_handed));
	}

	uint32_t click_methods = libinput_device_config_click_get_methods(device);
	if ((click_methods & ~LIBINPUT_CONFIG_CLICK_METHOD_NONE) != 0) {
		const char *click_method = "unknown";
		switch (libinput_device_config_click_get_method(device)) {
		case LIBINPUT_CONFIG_CLICK_METHOD_NONE:
			click_method = "none";
			break;
		case LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS:
			click_method = "button_areas";
			break;
		case LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER:
			click_method = "clickfinger";
			break;
		}
		json_object_object_add(object, "click_method",
				json_object_new_string(click_method));
	}

	if (libinput_device_config_middle_emulation_is_available(device)) {
		const char *middle_emulation = "unknown";
		switch (libinput_device_config_middle_emulation_get_enabled(device)) {
		case LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED:
			middle_emulation = "enabled";
			break;
		case LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED:
			middle_emulation = "disabled";
			break;
		}
		json_object_object_add(object, "middle_emulation",
				json_object_new_string(middle_emulation));
	}

	uint32_t scroll_methods = libinput_device_config_scroll_get_methods(device);
	if ((scroll_methods & ~LIBINPUT_CONFIG_SCROLL_NO_SCROLL) != 0) {
		const char *scroll_method = "unknown";
		switch (libinput_device_config_scroll_get_method(device)) {
		case LIBINPUT_CONFIG_SCROLL_NO_SCROLL:
			scroll_method = "none";
			break;
		case LIBINPUT_CONFIG_SCROLL_2FG:
			scroll_method = "two_finger";
			break;
		case LIBINPUT_CONFIG_SCROLL_EDGE:
			scroll_method = "edge";
			break;
		case LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN:
			scroll_method = "on_button_down";
			break;
		}
		json_object_object_add(object, "scroll_method",
				json_object_new_string(scroll_method));

		if ((scroll_methods & LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN) != 0) {
			uint32_t button = libinput_device_config_scroll_get_button(device);
			json_object_object_add(object, "scroll_button",
					json_object_new_int(button));
		}
	}

	if (libinput_device_config_dwt_is_available(device)) {
		const char *dwt = "unknown";
		switch (libinput_device_config_dwt_get_enabled(device)) {
		case LIBINPUT_CONFIG_DWT_ENABLED:
			dwt = "enabled";
			break;
		case LIBINPUT_CONFIG_DWT_DISABLED:
			dwt = "disabled";
			break;
		}
		json_object_object_add(object, "dwt", json_object_new_string(dwt));
	}

	return object;
}

json_object *ipc_json_describe_input(struct sway_input_device *device) {
	if (!(sway_assert(device, "Device must not be null"))) {
		return NULL;
	}

	json_object *object = json_object_new_object();

	json_object_object_add(object, "identifier",
		json_object_new_string(device->identifier));
	json_object_object_add(object, "name",
		json_object_new_string(device->wlr_device->name));
	json_object_object_add(object, "vendor",
		json_object_new_int(device->wlr_device->vendor));
	json_object_object_add(object, "product",
		json_object_new_int(device->wlr_device->product));
	json_object_object_add(object, "type",
		json_object_new_string(
			ipc_json_device_type_description(device)));

	if (device->wlr_device->type == WLR_INPUT_DEVICE_KEYBOARD) {
		struct wlr_keyboard *keyboard = device->wlr_device->keyboard;
		struct xkb_keymap *keymap = keyboard->keymap;
		struct xkb_state *state = keyboard->xkb_state;
		xkb_layout_index_t num_layouts = xkb_keymap_num_layouts(keymap);
		xkb_layout_index_t layout_idx;
		for (layout_idx = 0; layout_idx < num_layouts; layout_idx++) {
			bool is_active =
				xkb_state_layout_index_is_active(state,
						layout_idx,
						XKB_STATE_LAYOUT_EFFECTIVE);
			if (is_active) {
				const char *layout =
					xkb_keymap_layout_get_name(keymap, layout_idx);
				json_object_object_add(object, "xkb_active_layout_name",
						layout ? json_object_new_string(layout) : NULL);
				break;
			}
		}
	}

	if (wlr_input_device_is_libinput(device->wlr_device)) {
		struct libinput_device *libinput_dev;
		libinput_dev = wlr_libinput_get_device_handle(device->wlr_device);
		json_object_object_add(object, "libinput",
				describe_libinput_device(libinput_dev));
	}

	return object;
}

json_object *ipc_json_describe_seat(struct sway_seat *seat) {
	if (!(sway_assert(seat, "Seat must not be null"))) {
		return NULL;
	}

	json_object *object = json_object_new_object();
	struct sway_node *focus = seat_get_focus(seat);

	json_object_object_add(object, "name",
		json_object_new_string(seat->wlr_seat->name));
	json_object_object_add(object, "capabilities",
		json_object_new_int(seat->wlr_seat->capabilities));
	json_object_object_add(object, "focus",
		json_object_new_int(focus ? focus->id : 0));

	json_object *devices = json_object_new_array();
	struct sway_seat_device *device = NULL;
	wl_list_for_each(device, &seat->devices, link) {
		json_object_array_add(devices, ipc_json_describe_input(device->input_device));
	}
	json_object_object_add(object, "devices", devices);

	return object;
}

static uint32_t event_to_x11_button(uint32_t event) {
	switch (event) {
	case BTN_LEFT:
		return 1;
	case BTN_MIDDLE:
		return 2;
	case BTN_RIGHT:
		return 3;
	case SWAY_SCROLL_UP:
		return 4;
	case SWAY_SCROLL_DOWN:
		return 5;
	case SWAY_SCROLL_LEFT:
		return 6;
	case SWAY_SCROLL_RIGHT:
		return 7;
	case BTN_SIDE:
		return 8;
	case BTN_EXTRA:
		return 9;
	default:
		return 0;
	}
}

json_object *ipc_json_describe_bar_config(struct bar_config *bar) {
	if (!sway_assert(bar, "Bar must not be NULL")) {
		return NULL;
	}

	json_object *json = json_object_new_object();
	json_object_object_add(json, "id", json_object_new_string(bar->id));
	json_object_object_add(json, "mode", json_object_new_string(bar->mode));
	json_object_object_add(json, "hidden_state",
			json_object_new_string(bar->hidden_state));
	json_object_object_add(json, "position",
			json_object_new_string(bar->position));
	json_object_object_add(json, "status_command", bar->status_command ?
			json_object_new_string(bar->status_command) : NULL);
	json_object_object_add(json, "font",
			json_object_new_string((bar->font) ? bar->font : config->font));

	json_object *gaps = json_object_new_object();
	json_object_object_add(gaps, "top",
			json_object_new_int(bar->gaps.top));
	json_object_object_add(gaps, "right",
			json_object_new_int(bar->gaps.right));
	json_object_object_add(gaps, "bottom",
			json_object_new_int(bar->gaps.bottom));
	json_object_object_add(gaps, "left",
			json_object_new_int(bar->gaps.left));
	json_object_object_add(json, "gaps", gaps);

	if (bar->separator_symbol) {
		json_object_object_add(json, "separator_symbol",
				json_object_new_string(bar->separator_symbol));
	}
	json_object_object_add(json, "bar_height",
			json_object_new_int(bar->height));
	json_object_object_add(json, "status_padding",
			json_object_new_int(bar->status_padding));
	json_object_object_add(json, "status_edge_padding",
			json_object_new_int(bar->status_edge_padding));
	json_object_object_add(json, "wrap_scroll",
			json_object_new_boolean(bar->wrap_scroll));
	json_object_object_add(json, "workspace_buttons",
			json_object_new_boolean(bar->workspace_buttons));
	json_object_object_add(json, "strip_workspace_numbers",
			json_object_new_boolean(bar->strip_workspace_numbers));
	json_object_object_add(json, "strip_workspace_name",
			json_object_new_boolean(bar->strip_workspace_name));
	json_object_object_add(json, "binding_mode_indicator",
			json_object_new_boolean(bar->binding_mode_indicator));
	json_object_object_add(json, "verbose",
			json_object_new_boolean(bar->verbose));
	json_object_object_add(json, "pango_markup",
			json_object_new_boolean(bar->pango_markup));

	json_object *colors = json_object_new_object();
	json_object_object_add(colors, "background",
			json_object_new_string(bar->colors.background));
	json_object_object_add(colors, "statusline",
			json_object_new_string(bar->colors.statusline));
	json_object_object_add(colors, "separator",
			json_object_new_string(bar->colors.separator));

	if (bar->colors.focused_background) {
		json_object_object_add(colors, "focused_background",
				json_object_new_string(bar->colors.focused_background));
	} else {
		json_object_object_add(colors, "focused_background",
				json_object_new_string(bar->colors.background));
	}

	if (bar->colors.focused_statusline) {
		json_object_object_add(colors, "focused_statusline",
				json_object_new_string(bar->colors.focused_statusline));
	} else {
		json_object_object_add(colors, "focused_statusline",
				json_object_new_string(bar->colors.statusline));
	}

	if (bar->colors.focused_separator) {
		json_object_object_add(colors, "focused_separator",
				json_object_new_string(bar->colors.focused_separator));
	} else {
		json_object_object_add(colors, "focused_separator",
				json_object_new_string(bar->colors.separator));
	}

	json_object_object_add(colors, "focused_workspace_border",
			json_object_new_string(bar->colors.focused_workspace_border));
	json_object_object_add(colors, "focused_workspace_bg",
			json_object_new_string(bar->colors.focused_workspace_bg));
	json_object_object_add(colors, "focused_workspace_text",
			json_object_new_string(bar->colors.focused_workspace_text));

	json_object_object_add(colors, "inactive_workspace_border",
			json_object_new_string(bar->colors.inactive_workspace_border));
	json_object_object_add(colors, "inactive_workspace_bg",
			json_object_new_string(bar->colors.inactive_workspace_bg));
	json_object_object_add(colors, "inactive_workspace_text",
			json_object_new_string(bar->colors.inactive_workspace_text));

	json_object_object_add(colors, "active_workspace_border",
			json_object_new_string(bar->colors.active_workspace_border));
	json_object_object_add(colors, "active_workspace_bg",
			json_object_new_string(bar->colors.active_workspace_bg));
	json_object_object_add(colors, "active_workspace_text",
			json_object_new_string(bar->colors.active_workspace_text));

	json_object_object_add(colors, "urgent_workspace_border",
			json_object_new_string(bar->colors.urgent_workspace_border));
	json_object_object_add(colors, "urgent_workspace_bg",
			json_object_new_string(bar->colors.urgent_workspace_bg));
	json_object_object_add(colors, "urgent_workspace_text",
			json_object_new_string(bar->colors.urgent_workspace_text));

	if (bar->colors.binding_mode_border) {
		json_object_object_add(colors, "binding_mode_border",
				json_object_new_string(bar->colors.binding_mode_border));
	} else {
		json_object_object_add(colors, "binding_mode_border",
				json_object_new_string(bar->colors.urgent_workspace_border));
	}

	if (bar->colors.binding_mode_bg) {
		json_object_object_add(colors, "binding_mode_bg",
				json_object_new_string(bar->colors.binding_mode_bg));
	} else {
		json_object_object_add(colors, "binding_mode_bg",
				json_object_new_string(bar->colors.urgent_workspace_bg));
	}

	if (bar->colors.binding_mode_text) {
		json_object_object_add(colors, "binding_mode_text",
				json_object_new_string(bar->colors.binding_mode_text));
	} else {
		json_object_object_add(colors, "binding_mode_text",
				json_object_new_string(bar->colors.urgent_workspace_text));
	}

	json_object_object_add(json, "colors", colors);

	if (bar->bindings->length > 0) {
		json_object *bindings = json_object_new_array();
		for (int i = 0; i < bar->bindings->length; ++i) {
			struct bar_binding *binding = bar->bindings->items[i];
			json_object *bind = json_object_new_object();
			json_object_object_add(bind, "input_code",
					json_object_new_int(event_to_x11_button(binding->button)));
			json_object_object_add(bind, "event_code",
					json_object_new_int(binding->button));
			json_object_object_add(bind, "command",
					json_object_new_string(binding->command));
			json_object_object_add(bind, "release",
					json_object_new_boolean(binding->release));
			json_object_array_add(bindings, bind);
		}
		json_object_object_add(json, "bindings", bindings);
	}

	// Add outputs if defined
	if (bar->outputs && bar->outputs->length > 0) {
		json_object *outputs = json_object_new_array();
		for (int i = 0; i < bar->outputs->length; ++i) {
			const char *name = bar->outputs->items[i];
			json_object_array_add(outputs, json_object_new_string(name));
		}
		json_object_object_add(json, "outputs", outputs);
	}
#if HAVE_TRAY
	// Add tray outputs if defined
	if (bar->tray_outputs && bar->tray_outputs->length > 0) {
		json_object *tray_outputs = json_object_new_array();
		for (int i = 0; i < bar->tray_outputs->length; ++i) {
			const char *name = bar->tray_outputs->items[i];
			json_object_array_add(tray_outputs, json_object_new_string(name));
		}
		json_object_object_add(json, "tray_outputs", tray_outputs);
	}

	json_object *tray_bindings = json_object_new_array();
	struct tray_binding *tray_bind = NULL;
	wl_list_for_each(tray_bind, &bar->tray_bindings, link) {
		json_object *bind = json_object_new_object();
		json_object_object_add(bind, "input_code",
				json_object_new_int(event_to_x11_button(tray_bind->button)));
		json_object_object_add(bind, "event_code",
				json_object_new_int(tray_bind->button));
		json_object_object_add(bind, "command",
				json_object_new_string(tray_bind->command));
		json_object_array_add(tray_bindings, bind);
	}
	if (json_object_array_length(tray_bindings) > 0) {
		json_object_object_add(json, "tray_bindings", tray_bindings);
	} else {
		json_object_put(tray_bindings);
	}

	if (bar->icon_theme) {
		json_object_object_add(json, "icon_theme",
				json_object_new_string(bar->icon_theme));
	}

	json_object_object_add(json, "tray_padding",
			json_object_new_int(bar->tray_padding));
#endif
	return json;
}

json_object *ipc_json_describe_binding(struct sway_binding *binding) {
	if (!sway_assert(binding, "Binding must not be NULL")) {
		return NULL;
	}

	json_object *json_binding = json_object_new_object();
	json_object_object_add(json_binding, "command", json_object_new_string(binding->command));

	const char *names[10];
	int len = get_modifier_names(names, binding->modifiers);
	json_object *modifiers = json_object_new_array();
	for (int i = 0; i < len; ++i) {
		json_object_array_add(modifiers, json_object_new_string(names[i]));
	}
	json_object_object_add(json_binding, "event_state_mask", modifiers);

	json_object *input_codes = json_object_new_array();
	int input_code = 0;
	json_object *symbols = json_object_new_array();
	json_object *symbol = NULL;

	if (binding->type == BINDING_KEYCODE) { // bindcode: populate input_codes
		uint32_t keycode;
		for (int i = 0; i < binding->keys->length; ++i) {
			keycode = *(uint32_t *)binding->keys->items[i];
			json_object_array_add(input_codes, json_object_new_int(keycode));
			if (i == 0) {
				input_code = keycode;
			}
		}
	} else { // bindsym/mouse: populate symbols
		uint32_t keysym;
		char buffer[64];
		for (int i = 0; i < binding->keys->length; ++i) {
			keysym = *(uint32_t *)binding->keys->items[i];
			if (keysym >= BTN_LEFT && keysym <= BTN_LEFT + 8) {
				snprintf(buffer, 64, "button%u", keysym - BTN_LEFT + 1);
			} else if (xkb_keysym_get_name(keysym, buffer, 64) < 0) {
				continue;
			}

			json_object *str = json_object_new_string(buffer);
			if (i == 0) {
				// str is owned by both symbol and symbols. Make sure
				// to bump the ref count.
				json_object_array_add(symbols, json_object_get(str));
				symbol = str;
			} else {
				json_object_array_add(symbols, str);
			}
		}
	}

	json_object_object_add(json_binding, "input_codes", input_codes);
	json_object_object_add(json_binding, "input_code", json_object_new_int(input_code));
	json_object_object_add(json_binding, "symbols", symbols);
	json_object_object_add(json_binding, "symbol", symbol);

	bool mouse = binding->type == BINDING_MOUSECODE ||
		binding->type == BINDING_MOUSESYM;
	json_object_object_add(json_binding, "input_type", mouse
			? json_object_new_string("mouse")
			: json_object_new_string("keyboard"));

	return json_binding;
}

json_object *ipc_json_describe_mode_bindings(struct sway_mode * mode) {
	if (!sway_assert(mode, "Mode must not be NULL")) {
		return NULL;
	}

	json_object *json_bindings = json_object_new_array();

	for (int i = 0; i < mode->keysym_bindings->length; i++) {
		json_object_array_add(json_bindings,
			ipc_json_describe_binding(mode->keysym_bindings->items[i]));
	}

	for (int i = 0; i < mode->keycode_bindings->length; i++) {
		json_object_array_add(json_bindings,
			ipc_json_describe_binding(mode->keycode_bindings->items[i]));
	}

	for (int i = 0; i < mode->mouse_bindings->length; i++) {
		json_object_array_add(json_bindings,
			ipc_json_describe_binding(mode->mouse_bindings->items[i]));
	}

	return json_bindings;
}
