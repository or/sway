#define _XOPEN_SOURCE 500
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <wlc/wlc.h>
#include "sway/extensions.h"
#include "sway/config.h"
#include "sway/container.h"
#include "sway/workspace.h"
#include "sway/focus.h"
#include "sway/output.h"
#include "sway/ipc-server.h"
#include "sway/border.h"
#include "sway/layout.h"
#include "list.h"
#include "log.h"

swayc_t root_container;
swayc_t *current_focus;
list_t *scratchpad;

int min_sane_h = 60;
int min_sane_w = 100;

void init_layout(void) {
	root_container.id = 0; // normally assigned in new_swayc()
	root_container.type = C_ROOT;
	root_container.layout = L_NONE;
	root_container.name = strdup("root");
	root_container.children = create_list();
	root_container.handle = -1;
	root_container.visible = true;
	current_focus = &root_container;
	scratchpad = create_list();
}

int index_child(const swayc_t *child) {
	swayc_t *parent = child->parent;
	int i, len;
	if (!child->is_floating) {
		len = parent->children->length;
		for (i = 0; i < len; ++i) {
			if (parent->children->items[i] == child) {
				break;
			}
		}
	} else {
		len = parent->floating->length;
		for (i = 0; i < len; ++i) {
			if (parent->floating->items[i] == child) {
				break;
			}
		}
	}
	if (!sway_assert(i < len, "Stray container")) {
		return -1;
	}
	return i;
}

void add_child(swayc_t *parent, swayc_t *child) {
	sway_log(L_DEBUG, "Adding %p (%d, %fx%f) to %p (%d, %fx%f)", child, child->type,
		child->width, child->height, parent, parent->type, parent->width, parent->height);
	list_add(parent->children, child);
	child->parent = parent;
	// set focus for this container
	if (!parent->focused) {
		parent->focused = child;
	}
	if (parent->type == C_WORKSPACE && child->type == C_VIEW && (parent->workspace_layout == L_TABBED || parent->workspace_layout == L_STACKED)) {
		child = new_container(child, parent->workspace_layout);
	}
}

static double *get_height(swayc_t *cont) {
	return &cont->height;
}

static double *get_width(swayc_t *cont) {
	return &cont->width;
}

void insert_child(swayc_t *parent, swayc_t *child, int index) {
	if (index > parent->children->length) {
		index = parent->children->length;
	}
	if (index < 0) {
		index = 0;
	}
	list_insert(parent->children, index, child);
	child->parent = parent;
	if (!parent->focused) {
		parent->focused = child;
	}
	if (parent->type == C_WORKSPACE && child->type == C_VIEW && (parent->workspace_layout == L_TABBED || parent->workspace_layout == L_STACKED)) {
		child = new_container(child, parent->workspace_layout);
	}
	if (is_auto_layout(parent->layout)) {
		/* go through each group, adjust the size of the first child of each group */
		double *(*get_maj_dim)(swayc_t *cont);
		double *(*get_min_dim)(swayc_t *cont);
		if (parent->layout == L_AUTO_LEFT || parent->layout == L_AUTO_RIGHT) {
			get_maj_dim = get_width;
			get_min_dim = get_height;
		} else {
			get_maj_dim = get_height;
			get_min_dim = get_width;
		}
		for (int i = index; i < parent->children->length;) {
			int start = auto_group_start_index(parent, i);
			int end = auto_group_end_index(parent, i);
			swayc_t *first = parent->children->items[start];
			if (start + 1 < parent->children->length) {
				/* preserve the group's dimension along major axis */
				*get_maj_dim(first) = *get_maj_dim(parent->children->items[start + 1]);
			} else {
				/* new group, let the apply_layout handle it */
				first->height = first->width = 0;
				break;
			}
			double remaining = *get_min_dim(parent);
			for (int j = end - 1; j > start; --j) {
				swayc_t *sibling = parent->children->items[j];
				if (sibling == child) {
					/* the inserted child won't yet have its minor
					   dimension set */
					remaining -= *get_min_dim(parent) / (end - start);
				} else {
					remaining -= *get_min_dim(sibling);
				}
			}
			*get_min_dim(first) = remaining;
			i = end;
		}
	}
}

void add_floating(swayc_t *ws, swayc_t *child) {
	sway_log(L_DEBUG, "Adding %p (%d, %fx%f) to %p (%d, %fx%f)", child, child->type,
		child->width, child->height, ws, ws->type, ws->width, ws->height);
	if (!sway_assert(ws->type == C_WORKSPACE, "Must be of workspace type")) {
		return;
	}
	list_add(ws->floating, child);
	child->parent = ws;
	child->is_floating = true;
	if (!ws->focused) {
		ws->focused = child;
	}
	ipc_event_window(child, "floating");
}

swayc_t *add_sibling(swayc_t *fixed, swayc_t *active) {
	swayc_t *parent = fixed->parent;
	if (fixed->is_floating) {
		if (active->is_floating) {
			int i = index_child(fixed);
			list_insert(parent->floating, i + 1, active);
		} else {
			list_add(parent->children, active);
		}
	} else {
		if (active->is_floating) {
			list_add(parent->floating, active);
		} else {
			int i = index_child(fixed);
			if (is_auto_layout(parent->layout)) {
				list_add(parent->children, active);
			} else {
				list_insert(parent->children, i + 1, active);
			}
		}
	}
	active->parent = parent;
	// focus new child
	parent->focused = active;
	return active->parent;
}

swayc_t *replace_child(swayc_t *child, swayc_t *new_child) {
	swayc_t *parent = child->parent;
	if (parent == NULL) {
		return NULL;
	}
	int i = index_child(child);
	if (child->is_floating) {
		parent->floating->items[i] = new_child;
	} else {
		parent->children->items[i] = new_child;
	}
	// Set parent and focus for new_child
	new_child->parent = child->parent;
	if (child->parent->focused == child) {
		child->parent->focused = new_child;
	}
	child->parent = NULL;

	// Set geometry for new child
	new_child->x = child->x;
	new_child->y = child->y;
	new_child->width = child->width;
	new_child->height = child->height;

	// reset geometry for child
	child->width = 0;
	child->height = 0;

	// deactivate child
	if (child->type == C_VIEW) {
		wlc_view_set_state(child->handle, WLC_BIT_ACTIVATED, false);
	}
	return parent;
}

swayc_t *remove_child(swayc_t *child) {
	int i;
	swayc_t *parent = child->parent;
	if (child->is_floating) {
		// Special case for floating views
		for (i = 0; i < parent->floating->length; ++i) {
			if (parent->floating->items[i] == child) {
				list_del(parent->floating, i);
				break;
			}
		}
		i = 0;
	} else {
		for (i = 0; i < parent->children->length; ++i) {
			if (parent->children->items[i] == child) {
				list_del(parent->children, i);
				break;
			}
		}
		if (is_auto_layout(parent->layout) && parent->children->length) {
			/* go through each group, adjust the size of the last child of each group */
			double *(*get_maj_dim)(swayc_t *cont);
			double *(*get_min_dim)(swayc_t *cont);
			if (parent->layout == L_AUTO_LEFT || parent->layout == L_AUTO_RIGHT) {
				get_maj_dim = get_width;
				get_min_dim = get_height;
			} else {
				get_maj_dim = get_height;
				get_min_dim = get_width;
			}
			for (int j = parent->children->length - 1; j >= i;) {
				int start = auto_group_start_index(parent, j);
				int end = auto_group_end_index(parent, j);
				swayc_t *first = parent->children->items[start];
				if (i == start) {
					/* removed element was first child in the current group,
					   use its size along the major axis */
					*get_maj_dim(first) = *get_maj_dim(child);
				} else if (start > i) {
					/* preserve the group's dimension along major axis */
					*get_maj_dim(first) = *get_maj_dim(parent->children->items[start - 1]);
				}
				if (end != parent->children->length) {
					double remaining = *get_min_dim(parent);
					for (int k = start; k < end - 1; ++k) {
						swayc_t *sibling = parent->children->items[k];
						remaining -= *get_min_dim(sibling);
					}
					/* last element of the group gets remaining size, elements
					   that don't change groups keep their ratio */
					*get_min_dim((swayc_t *) parent->children->items[end - 1]) = remaining;
				} /* else last group, let apply_layout handle it */
				j = start - 1;
			}
		}
	}
	// Set focused to new container
	if (parent->focused == child) {
		if (parent->children->length > 0) {
			parent->focused = parent->children->items[i ? i-1:0];
		} else if (parent->floating && parent->floating->length) {
			parent->focused = parent->floating->items[parent->floating->length - 1];
		} else {
			parent->focused = NULL;
		}
	}
	child->parent = NULL;
	// deactivate view
	if (child->type == C_VIEW) {
		wlc_view_set_state(child->handle, WLC_BIT_ACTIVATED, false);
	}
	return parent;
}

void swap_container(swayc_t *a, swayc_t *b) {
	if (!sway_assert(a&&b, "parameters must be non null") ||
		!sway_assert(a->parent && b->parent, "containers must have parents")) {
		return;
	}
	size_t a_index = index_child(a);
	size_t b_index = index_child(b);
	swayc_t *a_parent = a->parent;
	swayc_t *b_parent = b->parent;
	// Swap the pointers
	if (a->is_floating) {
		a_parent->floating->items[a_index] = b;
	} else {
		a_parent->children->items[a_index] = b;
	}
	if (b->is_floating) {
		b_parent->floating->items[b_index] = a;
	} else {
		b_parent->children->items[b_index] = a;
	}
	a->parent = b_parent;
	b->parent = a_parent;
	if (a_parent->focused == a) {
		a_parent->focused = b;
	}
	// don't want to double switch
	if (b_parent->focused == b && a_parent != b_parent) {
		b_parent->focused = a;
	}
}

void swap_geometry(swayc_t *a, swayc_t *b) {
	double x = a->x;
	double y = a->y;
	double w = a->width;
	double h = a->height;
	a->x = b->x;
	a->y = b->y;
	a->width = b->width;
	a->height = b->height;
	b->x = x;
	b->y = y;
	b->width = w;
	b->height = h;
}

static void swap_children(swayc_t *container, int a, int b) {
	if (a >= 0 && b >= 0 && a < container->children->length
			&& b < container->children->length
		&& a != b) {
		swayc_t *pa = (swayc_t *)container->children->items[a];
		swayc_t *pb = (swayc_t *)container->children->items[b];
		container->children->items[a] = container->children->items[b];
		container->children->items[b] = pa;
		if (is_auto_layout(container->layout)) {
			size_t ga = auto_group_index(container, a);
			size_t gb = auto_group_index(container, b);
			if (ga != gb) {
				swap_geometry(pa, pb);
			}
		}
	}
}

void move_container(swayc_t *container, enum movement_direction dir, int move_amt) {
	enum swayc_layouts layout = L_NONE;
	swayc_t *parent = container->parent;
	if (container->is_floating) {
		swayc_t *output = swayc_parent_by_type(container, C_OUTPUT);
		switch(dir) {
		case MOVE_LEFT:
			container->x = MAX(0, container->x - move_amt);
			break;
		case MOVE_RIGHT:
			container->x = MIN(output->width - container->width, container->x + move_amt);
			break;
		case MOVE_UP:
			container->y = MAX(0, container->y - move_amt);
			break;
		case MOVE_DOWN:
			container->y = MIN(output->height - container->height, container->y + move_amt);
			break;
		default:
			break;
		}
		update_geometry(container);
		return;
	}
	if (container->type != C_VIEW && container->type != C_CONTAINER) {
		return;
	}
	if (dir == MOVE_UP || dir == MOVE_DOWN) {
		layout = L_VERT;
	} else if (dir == MOVE_LEFT || dir == MOVE_RIGHT) {
		layout = L_HORIZ;
	} else if (dir == MOVE_FIRST) {
		// swap first child in auto layout with currently focused child
		if (is_auto_layout(parent->layout)) {
			int focused_idx = index_child(container);
			swayc_t *first = parent->children->items[0];
			if (focused_idx > 0) {
				list_swap(parent->children, 0, focused_idx);
				swap_geometry(first, container);
			}
			arrange_windows(parent->parent, -1, -1);
			ipc_event_window(container, "move");
			set_focused_container_for(parent->parent, container);
		}
		return;
	} else if (! (dir == MOVE_NEXT || dir == MOVE_PREV)) {
		return;
	}
	swayc_t *child = container;
	bool ascended = false;

	// View is wrapped in intermediate container which is needed for displaying
	// the titlebar. Moving only the view outside of its parent container would just
	// wrap it again under worspace. There would effectively be no movement,
	// just a change of wrapping container.
	if (child->type == C_VIEW &&
		parent->type == C_CONTAINER &&
		parent->children->length == 1 &&
		parent->parent->type == C_WORKSPACE) {
		child = parent;
		parent = parent->parent;
	}

	while (true) {
		sway_log(L_DEBUG, "container:%p, parent:%p, child %p,",
				container,parent,child);
		if (parent->layout == layout
			|| (layout == L_NONE && (parent->type == C_CONTAINER || parent->type == C_WORKSPACE)) /* accept any layout for next/prev direction */
			|| (parent->layout == L_TABBED && layout == L_HORIZ)
			|| (parent->layout == L_STACKED && layout == L_VERT)
			|| is_auto_layout(parent->layout)) {
			int diff;
			// If it has ascended (parent has moved up), no container is removed
			// so insert it at index, or index+1.
			// if it has not, the moved container is removed, so it needs to be
			// inserted at index-1, or index+1
			if (ascended) {
				diff = dir == MOVE_LEFT || dir == MOVE_UP || dir == MOVE_PREV ? 0 : 1;
			} else {
				diff = dir == MOVE_LEFT || dir == MOVE_UP || dir == MOVE_PREV ? -1 : 1;
			}
			int idx = index_child(child);
			int desired = idx + diff;
			if (dir == MOVE_NEXT || dir == MOVE_PREV) {
				// Next/Prev always wrap.
				if (desired < 0) {
					desired += parent->children->length;
				} else if (desired >= parent->children->length) {
					desired = 0;
				}
			}
			// when it has ascended, legal insertion position is 0:len
			// when it has not, legal insertion position is 0:len-1
			if (desired >= 0 && desired - ascended < parent->children->length) {
				if (!ascended) {
					child = parent->children->items[desired];
					// Move container into sibling container
					if (child->type == C_CONTAINER) {
						parent = child;
						// Insert it in first/last if matching layout, otherwise
						// insert it next to focused container
						if (parent->layout == layout
							|| (parent->layout == L_TABBED && layout == L_HORIZ)
							|| (parent->layout == L_STACKED && layout == L_VERT)
							|| is_auto_layout(parent->layout)) {
							desired = (diff < 0) * parent->children->length;
						} else {
							desired = index_child(child->focused) + 1;
						}
						//reset geometry
						container->width = container->height = 0;
					}
				}
				if (container->parent == parent) {
					swap_children(parent, idx, desired);
				} else {
					swayc_t *old_parent = remove_child(container);
					insert_child(parent, container, desired);
					destroy_container(old_parent);
					sway_log(L_DEBUG,"Moving to %p %d", parent, desired);
				}
				break;
			}
		}
		// Change parent layout if we need to
		if (parent->children->length == 1 && parent->layout != layout && layout != L_NONE) {
			/* swayc_change_layout(parent, layout); */
			parent->layout = layout;
			continue;
		}
		if (parent->type == C_WORKSPACE) {
			// If moving to an adjacent output we need a starting position (since this
			// output might border to multiple outputs).
			struct wlc_point abs_pos;
			get_absolute_center_position(container, &abs_pos);

			swayc_t *output = swayc_adjacent_output(parent->parent, dir, &abs_pos, true);

			if (output) {
				sway_log(L_DEBUG, "Moving between outputs");
				swayc_t *old_parent = remove_child(container);
				destroy_container(old_parent);

				swayc_t *dest = output->focused;
				switch (dir) {
				case MOVE_LEFT:
				case MOVE_UP:
					// reset container geometry
					container->width = container->height = 0;
					add_child(dest, container);
					break;
				case MOVE_RIGHT:
				case MOVE_DOWN:
					// reset container geometry
					container->width = container->height = 0;
					insert_child(dest, container, 0);
					break;
				default:
					break;
				}
				// arrange new workspace
				arrange_windows(dest, -1, -1);
				set_focused_container(container);
				break;
			}

			// We simply cannot move any further.
			if (parent->layout == layout) {
				break;
			}
			// Create container around workspace to insert child into
			parent = new_container(parent, layout);
			// Previous line set the resulting container's layout to
			// workspace_layout. It should have been just layout.
			parent->layout = parent->parent->layout;
		}
		ascended = true;
		child = parent;
		parent = child->parent;
	}
	arrange_windows(parent->parent, -1, -1);
	ipc_event_window(container, "move");
	set_focused_container_for(parent->parent, container);
}

void move_container_to(swayc_t* container, swayc_t* destination) {
	if (container == destination || swayc_is_parent_of(container, destination)) {
		return;
	}
	swayc_t *parent = remove_child(container);
	// Send to new destination
	if (container->is_floating) {
		swayc_t *ws = swayc_active_workspace_for(destination);
		add_floating(ws, container);

		// If the workspace only has one child after adding one, it
		// means that the workspace was just initialized.
		if (ws->children->length + ws->floating->length == 1) {
			ipc_event_workspace(NULL, ws, "init");
		}
	} else if (destination->type == C_WORKSPACE) {
		// reset container geometry
		container->width = container->height = 0;
		add_child(destination, container);

		// If the workspace only has one child after adding one, it
		// means that the workspace was just initialized.
		if (destination->children->length + destination->floating->length == 1) {
			ipc_event_workspace(NULL, destination, "init");
		}
	} else {
		// reset container geometry
		container->width = container->height = 0;
		add_sibling(destination, container);
	}
	// Destroy old container if we need to
	parent = destroy_container(parent);
	// Refocus
	swayc_t *op1 = swayc_parent_by_type(destination, C_OUTPUT);
	swayc_t *op2 = swayc_parent_by_type(parent, C_OUTPUT);
	set_focused_container(get_focused_view(op1));
	arrange_windows(op1, -1, -1);
	update_visibility(op1);
	if (op1 != op2) {
		set_focused_container(get_focused_view(op2));
		arrange_windows(op2, -1, -1);
		update_visibility(op2);
	}
}

void move_workspace_to(swayc_t* workspace, swayc_t* destination) {
	if (workspace == destination || swayc_is_parent_of(workspace, destination)) {
		return;
	}
	swayc_t *src_op = remove_child(workspace);
	// reset container geometry
	workspace->width = workspace->height = 0;
	add_child(destination, workspace);
	sort_workspaces(destination);
	// Refocus destination (change to new workspace)
	set_focused_container(get_focused_view(workspace));
	arrange_windows(destination, -1, -1);
	update_visibility(destination);

	// make sure source output has a workspace
	if (src_op->children->length == 0) {
		char *ws_name = workspace_next_name(src_op->name);
		swayc_t *ws = new_workspace(src_op, ws_name);
		ws->is_focused = true;
		free(ws_name);
	}
	set_focused_container(get_focused_view(src_op));
	update_visibility(src_op);
}

static void adjust_border_geometry(swayc_t *c, struct wlc_geometry *g,
	const struct wlc_size *res, int left, int right, int top, int bottom) {

	g->size.w += left + right;
	if (g->origin.x - left < 0) {
		g->size.w += g->origin.x - left;
	} else if (g->origin.x + g->size.w - right > res->w) {
		g->size.w = res->w - g->origin.x + right;
	}

	g->size.h += top + bottom;
	if (g->origin.y - top < 0) {
		g->size.h += g->origin.y - top;
	} else if (g->origin.y + g->size.h - top > res->h) {
		g->size.h = res->h - g->origin.y + top;
	}

	g->origin.x = MIN((uint32_t)MAX(g->origin.x - left, 0), res->w);
	g->origin.y = MIN((uint32_t)MAX(g->origin.y - top, 0), res->h);

}

static void update_border_geometry_floating(swayc_t *c, struct wlc_geometry *geometry) {
	struct wlc_geometry g = *geometry;
	c->actual_geometry = g;

	swayc_t *output = swayc_parent_by_type(c, C_OUTPUT);
	struct wlc_size res;
	output_get_scaled_size(output->handle, &res);

	switch (c->border_type) {
	case B_NONE:
		break;
	case B_PIXEL:
		adjust_border_geometry(c, &g, &res, c->border_thickness,
			c->border_thickness, c->border_thickness, c->border_thickness);
		break;
	case B_NORMAL:
	{
		int title_bar_height = config->font_height + 4; // borders + padding

		adjust_border_geometry(c, &g, &res, c->border_thickness,
			c->border_thickness, title_bar_height, c->border_thickness);

		struct wlc_geometry title_bar = {
			.origin = {
				.x = c->actual_geometry.origin.x - c->border_thickness,
				.y = c->actual_geometry.origin.y - title_bar_height
			},
			.size = {
				.w = c->actual_geometry.size.w + (2 * c->border_thickness),
				.h = title_bar_height
			}
		};
		c->title_bar_geometry = title_bar;
		break;
	}
	}

	c->border_geometry = g;
	*geometry = c->actual_geometry;

	update_container_border(c);
}

void update_layout_geometry(swayc_t *parent, enum swayc_layouts prev_layout) {
	switch (parent->layout) {
	case L_TABBED:
	case L_STACKED:
		if (prev_layout != L_TABBED && prev_layout != L_STACKED) {
			// cache current geometry for all non-float children
			int i;
			for (i = 0; i < parent->children->length; ++i) {
				swayc_t *child = parent->children->items[i];
				child->cached_geometry.origin.x = child->x;
				child->cached_geometry.origin.y = child->y;
				child->cached_geometry.size.w = child->width;
				child->cached_geometry.size.h = child->height;
			}
		}
		break;
	default:
		if (prev_layout == L_TABBED || prev_layout == L_STACKED) {
			// recover cached geometry for all non-float children
			int i;
			for (i = 0; i < parent->children->length; ++i) {
				swayc_t *child = parent->children->items[i];
				// only recoverer cached geometry if non-zero
				if (!wlc_geometry_equals(&child->cached_geometry, &wlc_geometry_zero)) {
					child->x = child->cached_geometry.origin.x;
					child->y = child->cached_geometry.origin.y;
					child->width = child->cached_geometry.size.w;
					child->height = child->cached_geometry.size.h;
				}
			}
		}
		break;
	}
}

static int update_gap_geometry(swayc_t *container, struct wlc_geometry *g) {
	swayc_t *ws = swayc_parent_by_type(container, C_WORKSPACE);
	swayc_t *op = ws->parent;
	int gap = container->is_floating ? 0 : swayc_gap(container);
	if (gap % 2 != 0) {
		// because gaps are implemented as "half sized margins" it's currently
		// not possible to align views properly with odd sized gaps.
		gap -= 1;
	}

	g->origin.x = container->x + gap/2 < op->width  ? container->x + gap/2 : op->width-1;
	g->origin.y = container->y + gap/2 < op->height ? container->y + gap/2 : op->height-1;
	g->size.w = container->width > gap ? container->width - gap : 1;
	g->size.h = container->height > gap ? container->height - gap : 1;

	if ((!config->edge_gaps && gap > 0) || (config->smart_gaps && ws->children->length == 1)) {
		// Remove gap against the workspace edges. Because a pixel is not
		// divisable, depending on gap size and the number of siblings our view
		// might be at the workspace edge without being exactly so (thus test
		// with gap, and align correctly).
		if (container->x - gap <= ws->x) {
			g->origin.x = ws->x;
			g->size.w = container->width - gap/2;
		}
		if (container->y - gap <= ws->y) {
			g->origin.y = ws->y;
			g->size.h = container->height - gap/2;
		}
		if (container->x + container->width + gap >= ws->x + ws->width) {
			g->size.w = ws->x + ws->width - g->origin.x;
		}
		if (container->y + container->height + gap >= ws->y + ws->height) {
			g->size.h = ws->y + ws->height - g->origin.y;
		}
	}

	return gap;
}

void update_geometry(swayc_t *container) {
	if (container->type != C_VIEW && container->type != C_CONTAINER) {
		return;
	}

	swayc_t *workspace = swayc_parent_by_type(container, C_WORKSPACE);
	swayc_t *op = workspace->parent;
	swayc_t *parent = container->parent;

	struct wlc_geometry geometry = {
		.origin = {
			.x = container->x < op->width ? container->x : op->width-1,
			.y = container->y < op->height ? container->y : op->height-1
		},
		.size = {
			.w = container->width,
			.h = container->height,
		}
	};

	int gap = 0;

	// apply inner gaps to non-tabbed/stacked containers
	swayc_t *p = swayc_tabbed_stacked_ancestor(container);
	if (p == NULL) {
		gap = update_gap_geometry(container, &geometry);
	}

	swayc_t *output = swayc_parent_by_type(container, C_OUTPUT);
	struct wlc_size size;
	output_get_scaled_size(output->handle, &size);

	if (swayc_is_fullscreen(container)) {
		geometry.origin.x = 0;
		geometry.origin.y = 0;
		geometry.size.w = size.w;
		geometry.size.h = size.h;
		if (op->focused == workspace) {
			wlc_view_bring_to_front(container->handle);
		}

		container->border_geometry = wlc_geometry_zero;
		container->title_bar_geometry = wlc_geometry_zero;
		border_clear(container->border);
	} else if (container->is_floating) { // allocate border for floating window
		update_border_geometry_floating(container, &geometry);
	} else if (!container->is_floating) { // allocate border for titled window
		container->border_geometry = geometry;

		int border_top = container->border_thickness;
		int border_bottom = container->border_thickness;
		int border_left = container->border_thickness;
		int border_right = container->border_thickness;

		// handle hide_edge_borders
		if (config->hide_edge_borders != E_NONE && (gap <= 0 || (config->smart_gaps && workspace->children->length == 1))) {
			if (config->hide_edge_borders == E_VERTICAL || config->hide_edge_borders == E_BOTH) {
				if (geometry.origin.x == workspace->x) {
					border_left = 0;
				}

				if (geometry.origin.x + geometry.size.w == workspace->x + workspace->width) {
					border_right = 0;
				}
			}

			if (config->hide_edge_borders == E_HORIZONTAL || config->hide_edge_borders == E_BOTH) {
				if (geometry.origin.y == workspace->y || should_hide_top_border(container, geometry.origin.y)) {
					border_top = 0;
				}

				if (geometry.origin.y + geometry.size.h == workspace->y + workspace->height) {
					border_bottom = 0;
				}
			}

			if (config->hide_edge_borders == E_SMART && workspace->children->length == 1) {
				border_top = 0;
				border_bottom = 0;
				border_left = 0;
				border_right = 0;
			}
		}

		int title_bar_height = config->font_height + 4; //borders + padding

		if (parent->layout == L_TABBED && parent->children->length > 1) {
			int i, x = 0, w, l, r;
			l = parent->children->length;
			w = geometry.size.w / l;
			r = geometry.size.w % l;
			for (i = 0; i < parent->children->length; ++i) {
				swayc_t *view = parent->children->items[i];
				if (view == container) {
					x = w * i;
					if (i == l - 1) {
						w += r;
					}
					break;
				}
			}

			struct wlc_geometry title_bar = {
				.origin = {
					.x = container->border_geometry.origin.x + x,
					.y = container->border_geometry.origin.y
				},
				.size = {
					.w = w,
					.h = title_bar_height
				}
			};
			geometry.origin.x += border_left;
			geometry.origin.y += title_bar.size.h;
			geometry.size.w -= (border_left + border_right);
			geometry.size.h -= (border_bottom + title_bar.size.h);
			container->title_bar_geometry = title_bar;
		} else if (parent->layout == L_STACKED && parent->children->length > 1) {
			int i, y = 0;
			for (i = 0; i < parent->children->length; ++i) {
				swayc_t *view = parent->children->items[i];
				if (view == container) {
					y = title_bar_height * i;
				}
			}

			struct wlc_geometry title_bar = {
				.origin = {
					.x = container->border_geometry.origin.x,
					.y = container->border_geometry.origin.y + y
				},
				.size = {
					.w = container->border_geometry.size.w,
					.h = title_bar_height
				}
			};
			title_bar_height = title_bar_height * parent->children->length;
			geometry.origin.x += border_left;
			geometry.origin.y += title_bar_height;
			geometry.size.w -= (border_left + border_right);
			geometry.size.h -= (border_bottom + title_bar_height);
			container->title_bar_geometry = title_bar;
		} else {
			switch (container->border_type) {
			case B_NONE:
				break;
			case B_PIXEL:
				geometry.origin.x += border_left;
				geometry.origin.y += border_top;
				geometry.size.w -= (border_left + border_right);
				geometry.size.h -= (border_top + border_bottom);
				break;
			case B_NORMAL:
				{
					struct wlc_geometry title_bar = {
						.origin = {
							.x = container->border_geometry.origin.x,
							.y = container->border_geometry.origin.y
						},
						.size = {
							.w = container->border_geometry.size.w,
							.h = title_bar_height
						}
					};
					geometry.origin.x += border_left;
					geometry.origin.y += title_bar.size.h;
					geometry.size.w -= (border_left + border_right);
					geometry.size.h -= (border_bottom + title_bar.size.h);
					container->title_bar_geometry = title_bar;
					break;
				}
			}
		}

		container->actual_geometry = geometry;

		if (container->type == C_VIEW) {
			update_container_border(container);
		}
	}

	if (container->type == C_VIEW) {
		wlc_view_set_geometry(container->handle, 0, &geometry);
	}
}

/**
 * Layout application prototypes
 */
static void apply_horiz_layout(swayc_t *container, const double x,
				const double y, const double width,
				const double height, const int start,
				const int end);
static void apply_vert_layout(swayc_t *container, const double x,
				const double y, const double width,
				const double height, const int start,
				const int end);
static void apply_tabbed_or_stacked_layout(swayc_t *container, double x,
				double y, double width,
				double height);

static void apply_auto_layout(swayc_t *container, const double x, const double y,
				const double width, const double height,
				enum swayc_layouts group_layout,
				bool master_first);

static void arrange_windows_r(swayc_t *container, double width, double height) {
	int i;
	if (width == -1 || height == -1) {
		swayc_log(L_DEBUG, container, "Arranging layout for %p", container);
		width = container->width;
		height = container->height;
	}
	// pixels are indivisible. if we don't round the pixels, then the view
	// calculations will be off (e.g. 50.5 + 50.5 = 101, but in reality it's
	// 50 + 50 = 100). doing it here cascades properly to all width/height/x/y.
	width = floor(width);
	height = floor(height);

	sway_log(L_DEBUG, "Arranging layout for %p %s %fx%f+%f,%f", container,
		 container->name, container->width, container->height, container->x,
		 container->y);

	double x = 0, y = 0;
	switch (container->type) {
	case C_ROOT:
		for (i = 0; i < container->children->length; ++i) {
			swayc_t *output = container->children->items[i];
			sway_log(L_DEBUG, "Arranging output '%s' at %f,%f", output->name, output->x, output->y);
			arrange_windows_r(output, -1, -1);
		}
		return;
	case C_OUTPUT:
		{
			struct wlc_size resolution;
			output_get_scaled_size(container->handle, &resolution);
			width = resolution.w; height = resolution.h;
			if (container->handle != UINTPTR_MAX) {
				struct wlc_size resolution = *wlc_output_get_resolution(container->handle);
				width = resolution.w; height = resolution.h;
				// output must have correct size due to e.g. seamless mouse,
				// but a workspace might be smaller depending on panels.
				container->width = width;
				container->height = height;
			}
		}
		// arrange all workspaces:
		for (i = 0; i < container->children->length; ++i) {
			swayc_t *child = container->children->items[i];
			arrange_windows_r(child, -1, -1);
		}
		// Bring all unmanaged views to the front
		for (i = 0; i < container->unmanaged->length; ++i) {
			wlc_handle *handle = container->unmanaged->items[i];
			wlc_view_bring_to_front(*handle);
		}
		return;
	case C_WORKSPACE:
		{
			swayc_t *output = swayc_parent_by_type(container, C_OUTPUT);
			width = output->width, height = output->height;
			for (i = 0; i < desktop_shell.panels->length; ++i) {
				struct panel_config *config = desktop_shell.panels->items[i];
				if (config->output == output->handle) {
					struct wlc_size size = *wlc_surface_get_size(config->surface);
					sway_log(L_DEBUG, "-> Found panel for this workspace: %ux%u, position: %u", size.w, size.h, config->panel_position);
					switch (config->panel_position) {
					case DESKTOP_SHELL_PANEL_POSITION_TOP:
						y += size.h; height -= size.h;
						break;
					case DESKTOP_SHELL_PANEL_POSITION_BOTTOM:
						height -= size.h;
						break;
					case DESKTOP_SHELL_PANEL_POSITION_LEFT:
						x += size.w; width -= size.w;
						break;
					case DESKTOP_SHELL_PANEL_POSITION_RIGHT:
						width -= size.w;
						break;
					}
				}
			}
			int gap = swayc_gap(container);
			x = container->x = x + gap;
			y = container->y = y + gap;
			width = container->width = width - gap * 2;
			height = container->height = height - gap * 2;
			sway_log(L_DEBUG, "Arranging workspace '%s' at %f, %f", container->name, container->x, container->y);
		}
		// children are properly handled below
		break;
	case C_VIEW:
		{
			swayc_t *output = swayc_parent_by_type(container, C_OUTPUT);
			if (output->handle == UINTPTR_MAX) {
				sway_log(L_DEBUG, "Setting view invisible due to vt220");
				wlc_view_set_mask(container->handle, 0);
			}
			container->width = width;
			container->height = height;
			update_geometry(container);
			sway_log(L_DEBUG, "Set view to %.f x %.f @ %.f, %.f", container->width,
					container->height, container->x, container->y);
		}
		return;
	default:
		container->width = width;
		container->height = height;
		x = container->x;
		y = container->y;

		// add gaps to top level tapped/stacked container
		if (container->parent->type == C_WORKSPACE &&
			(container->layout == L_TABBED || container->layout == L_STACKED)) {
			update_geometry(container);
			width = container->border_geometry.size.w;
			height = container->border_geometry.size.h;
			x = container->border_geometry.origin.x;
			y = container->border_geometry.origin.y;
		}

		// update container size if it's a direct child in a tabbed/stacked layout
		// if parent is a workspace, its actual_geometry won't be initialized
		if (swayc_tabbed_stacked_parent(container) != NULL &&
			container->parent->type != C_WORKSPACE) {
			// Use parent actual_geometry as a base for calculating
			// container geometry
			container->width = container->parent->actual_geometry.size.w;
			container->height = container->parent->actual_geometry.size.h;
			container->x = container->parent->actual_geometry.origin.x;
			container->y = container->parent->actual_geometry.origin.y;

			update_geometry(container);
			width = container->width = container->actual_geometry.size.w;
			height = container->height = container->actual_geometry.size.h;
			x = container->x = container->actual_geometry.origin.x;
			y = container->y = container->actual_geometry.origin.y;
		}

		break;
	}

	switch (container->layout) {
	case L_HORIZ:
	default:
		apply_horiz_layout(container, x, y, width, height, 0,
			container->children->length);
		break;
	case L_VERT:
		apply_vert_layout(container, x, y, width, height, 0,
			container->children->length);
		break;
	case L_TABBED:
	case L_STACKED:
		apply_tabbed_or_stacked_layout(container, x, y, width, height);
		break;
	case L_AUTO_LEFT:
		apply_auto_layout(container, x, y, width, height, L_VERT, true);
		break;
	case L_AUTO_RIGHT:
		apply_auto_layout(container, x, y, width, height, L_VERT, false);
		break;
	case L_AUTO_TOP:
		apply_auto_layout(container, x, y, width, height, L_HORIZ, true);
		break;
	case L_AUTO_BOTTOM:
		apply_auto_layout(container, x, y, width, height, L_HORIZ, false);
		break;
	}

	// Arrage floating layouts for workspaces last
	if (container->type == C_WORKSPACE) {
		for (int i = 0; i < container->floating->length; ++i) {
			swayc_t *view = container->floating->items[i];
			if (view->type == C_VIEW) {
				update_geometry(view);
				sway_log(L_DEBUG, "Set floating view to %.f x %.f @ %.f, %.f",
					 view->width, view->height, view->x, view->y);
				if (swayc_is_fullscreen(view)) {
					wlc_view_bring_to_front(view->handle);
				} else if (!container->focused ||
						!swayc_is_fullscreen(container->focused)) {
					wlc_view_bring_to_front(view->handle);
				}
			}
		}
	}
}

void apply_horiz_layout(swayc_t *container, const double x, const double y,
			const double width, const double height,
			const int start, const int end) {
	double scale = 0;
	// Calculate total width
	for (int i = start; i < end; ++i) {
		double *old_width = &((swayc_t *)container->children->items[i])->width;
		if (*old_width <= 0) {
			if (end - start > 1) {
				*old_width = width / (end - start - 1);
			} else {
				*old_width = width;
			}
		}
		scale += *old_width;
	}
	scale = width / scale;

	// Resize windows
	double child_x = x;
	if (scale > 0.1) {
		sway_log(L_DEBUG, "Arranging %p horizontally", container);
		swayc_t *focused = NULL;
		for (int i = start; i < end; ++i) {
			swayc_t *child = container->children->items[i];
			sway_log(L_DEBUG,
				 "Calculating arrangement for %p:%d (will scale %f by %f)", child,
				 child->type, width, scale);
			child->x = child_x;
			child->y = y;

			if (child == container->focused) {
				focused = child;
			}

			if (i == end - 1) {
				double remaining_width = x + width - child_x;
				arrange_windows_r(child, remaining_width, height);
			} else {
				arrange_windows_r(child, child->width * scale, height);
			}
			child_x += child->width;
		}

		// update focused view border last because it may
		// depend on the title bar geometry of its siblings.
		if (focused && container->children->length > 1) {
			update_container_border(focused);
		}
	}
}

void apply_vert_layout(swayc_t *container, const double x, const double y,
			const double width, const double height, const int start,
			const int end) {
	int i;
	double scale = 0;
	// Calculate total height
	for (i = start; i < end; ++i) {
		double *old_height = &((swayc_t *)container->children->items[i])->height;
		if (*old_height <= 0) {
			if (end - start > 1) {
				*old_height = height / (end - start - 1);
			} else {
				*old_height = height;
			}
		}
		scale += *old_height;
	}
	scale = height / scale;

	// Resize
	double child_y = y;
	if (scale > 0.1) {
		sway_log(L_DEBUG, "Arranging %p vertically", container);
		swayc_t *focused = NULL;
		for (i = start; i < end; ++i) {
			swayc_t *child = container->children->items[i];
			sway_log(L_DEBUG,
				 "Calculating arrangement for %p:%d (will scale %f by %f)", child,
				 child->type, height, scale);
			child->x = x;
			child->y = child_y;

			if (child == container->focused) {
				focused = child;
			}

			if (i == end - 1) {
				double remaining_height = y + height - child_y;
				arrange_windows_r(child, width, remaining_height);
			} else {
				arrange_windows_r(child, width, child->height * scale);
			}
			child_y += child->height;
		}

		// update focused view border last because it may
		// depend on the title bar geometry of its siblings.
		if (focused && container->children->length > 1) {
			update_container_border(focused);
		}
	}
}

void apply_tabbed_or_stacked_layout(swayc_t *container, double x, double y,
					double width, double height) {
	int i;
	swayc_t *focused = NULL;
	for (i = 0; i < container->children->length; ++i) {
		swayc_t *child = container->children->items[i];
		child->x = x;
		child->y = y;
		if (child == container->focused) {
			focused = child;
		} else {
			arrange_windows_r(child, width, height);
		}
	}

	if (focused) {
		arrange_windows_r(focused, width, height);
	}
}

void apply_auto_layout(swayc_t *container, const double x, const double y,
			const double width, const double height,
			enum swayc_layouts group_layout,
			bool master_first) {
	// Auto layout "container" in width x height @ x, y
	// using "group_layout" for each of the groups in the container.
	// There is one "master" group, plus container->nb_slave_groups.
	// Each group is layed out side by side following the "major" axis.
	// The direction of the layout used for groups is the "minor" axis.
	// Example:
	//
	//     ---- major axis -->
	//   +---------+-----------+
	//   |         |           |   |
	//   | master  | slave 1   |   |
	//   |         +-----------+   | minor axis (direction of group_layout)
	//   |         |           |   |
	//   |         | slave 2   |   V
	//   +---------+-----------+
	//
	//  container with three children (one master and two slaves) and
	//  a single slave group (containing slave 1 and 2). The master
	//  group and slave group are layed out using L_VERT.

	size_t nb_groups = auto_group_count(container);

	// the target dimension of the container along the "major" axis, each
	// group in the container will be layed out using "group_layout" along
	// the "minor" axis.
	double dim_maj;
	double pos_maj;

	// x and y coords for the next group to be laid out.
	const double *group_x, *group_y;

	// pos of the next group to layout along the major axis
	double pos;

	// size of the next group along the major axis.
	double group_dim;

	// height and width of next group to be laid out.
	const double *group_h, *group_w;

	switch (group_layout) {
	default:
		sway_log(L_DEBUG, "Unknown layout type (%d) used in %s()",
			 group_layout, __func__);
		/* fall through */
	case L_VERT:
		dim_maj = width;
		pos_maj = x;

		group_x = &pos;
		group_y = &y;
		group_w = &group_dim;
		group_h = &height;
		break;
	case L_HORIZ:
		dim_maj = height;
		pos_maj = y;

		group_x = &x;
		group_y = &pos;
		group_w = &width;
		group_h = &group_dim;
		break;
	}

	/* Determine the dimension of each of the groups in the layout.
	 * Dimension will be width for a VERT layout and height for a HORIZ
	 * layout. */
	double old_group_dim[nb_groups];
	double old_dim = 0;
	for (size_t group = 0; group < nb_groups; ++group) {
		int idx;
		if (auto_group_bounds(container, group, &idx, NULL)) {
			swayc_t *child = container->children->items[idx];
			double *dim = group_layout == L_HORIZ ? &child->height : &child->width;
			if (*dim <= 0) {
				// New child with uninitialized dimension
				*dim = dim_maj;
				if (nb_groups > 1) {
					// child gets a dimension proportional to existing groups,
					// it will be later scaled based on to the available size
					// in the major axis.
					*dim /= (nb_groups - 1);
				}
			}
			old_dim += *dim;
			old_group_dim[group] = *dim;
		}
	}
	double scale = dim_maj / old_dim;

	/* Apply layout to each group */
	pos = pos_maj;

	for (size_t group = 0; group < nb_groups; ++group) {
		int start, end;	// index of first (inclusive) and last (exclusive) child in the group
		if (auto_group_bounds(container, group, &start, &end)) {
			// adjusted size of the group
			group_dim = old_group_dim[group] * scale;
			if (group == nb_groups - 1) {
				group_dim = pos_maj + dim_maj - pos; // remaining width
			}
			sway_log(L_DEBUG, "Arranging container %p column %zu, children [%d,%d[ (%fx%f+%f,%f)",
				 container, group, start, end, *group_w, *group_h, *group_x, *group_y);
			switch (group_layout) {
			default:
			case L_VERT:
				apply_vert_layout(container, *group_x, *group_y, *group_w, *group_h, start, end);
				break;
			case L_HORIZ:
				apply_horiz_layout(container, *group_x, *group_y, *group_w, *group_h, start, end);
				break;
			}

			/* update position for next group */
			pos += group_dim;
		}
	}
}

void arrange_windows(swayc_t *container, double width, double height) {
	update_visibility(container);
	arrange_windows_r(container, width, height);
	layout_log(&root_container, 0);
}

void arrange_backgrounds(void) {
	struct background_config *bg;
	for (int i = 0; i < desktop_shell.backgrounds->length; ++i) {
		bg = desktop_shell.backgrounds->items[i];
		wlc_view_send_to_back(bg->handle);
	}
}

/**
 * Get swayc in the direction of newly entered output.
 */
static swayc_t *get_swayc_in_output_direction(swayc_t *output, enum movement_direction dir) {
	if (!output) {
		return NULL;
	}

	swayc_t *ws = swayc_focus_by_type(output, C_WORKSPACE);
	if (ws && ws->children->length > 0) {
		switch (dir) {
		case MOVE_LEFT:
			// get most right child of new output
			return ws->children->items[ws->children->length-1];
		case MOVE_RIGHT:
			// get most left child of new output
			return ws->children->items[0];
		case MOVE_UP:
		case MOVE_DOWN:
			{
				swayc_t *focused_view = swayc_focus_by_type(ws, C_VIEW);
				if (focused_view && focused_view->parent) {
					swayc_t *parent = focused_view->parent;
					if (parent->layout == L_VERT) {
						if (dir == MOVE_UP) {
							// get child furthest down on new output
							return parent->children->items[parent->children->length-1];
						} else if (dir == MOVE_DOWN) {
							// get child furthest up on new output
							return parent->children->items[0];
						}
					}
					return focused_view;
				}
				break;
			}
		default:
			break;
		}
	}

	return output;
}

swayc_t *get_swayc_in_direction_under(swayc_t *container, enum movement_direction dir, swayc_t *limit) {
	if (dir == MOVE_CHILD) {
		return container->focused;
	}

	swayc_t *parent = container->parent;
	if (dir == MOVE_PARENT) {
		if (parent->type == C_OUTPUT) {
			return NULL;
		} else {
			return parent;
		}
	}

	if (dir == MOVE_PREV || dir == MOVE_NEXT) {
		int focused_idx = index_child(container);
		if (focused_idx == -1) {
			return NULL;
		} else {
			int desired = (focused_idx + (dir == MOVE_NEXT ? 1 : -1)) %
				parent->children->length;
			if (desired < 0) {
				desired += parent->children->length;
			}
			return parent->children->items[desired];
		}
	}

	// If moving to an adjacent output we need a starting position (since this
	// output might border to multiple outputs).
	struct wlc_point abs_pos;
	get_absolute_center_position(container, &abs_pos);

	if (container->type == C_VIEW && swayc_is_fullscreen(container)) {
		sway_log(L_DEBUG, "Moving from fullscreen view, skipping to output");
		container = swayc_parent_by_type(container, C_OUTPUT);
		get_absolute_center_position(container, &abs_pos);
		swayc_t *output = swayc_adjacent_output(container, dir, &abs_pos, true);
		return get_swayc_in_output_direction(output, dir);
	}

	if (container->type == C_WORKSPACE && container->fullscreen) {
		sway_log(L_DEBUG, "Moving to fullscreen view");
		return container->fullscreen;
	}

	swayc_t *wrap_candidate = NULL;
	while (true) {
		// Test if we can even make a difference here
		bool can_move = false;
		int desired;
		int idx = index_child(container);
		if (parent->type == C_ROOT) {
			swayc_t *output = swayc_adjacent_output(container, dir, &abs_pos, true);
			if (!output || output == container) {
				return wrap_candidate;
			}
			sway_log(L_DEBUG, "Moving between outputs");
			return get_swayc_in_output_direction(output, dir);
		} else {
			if (is_auto_layout(parent->layout)) {
				bool is_major = parent->layout == L_AUTO_LEFT || parent->layout == L_AUTO_RIGHT
					? dir == MOVE_LEFT || dir == MOVE_RIGHT
					: dir == MOVE_DOWN || dir == MOVE_UP;
				size_t gidx = auto_group_index(parent, idx);
				if (is_major) {
					size_t desired_grp = gidx + (dir == MOVE_RIGHT || dir == MOVE_DOWN ? 1 : -1);
					can_move = auto_group_bounds(parent, desired_grp, &desired, NULL);
				} else {
					desired = idx + (dir == MOVE_RIGHT || dir == MOVE_DOWN ? 1 : -1);
					int start, end;
					can_move = auto_group_bounds(parent, gidx, &start, &end)
							&& desired >= start && desired < end;
				}
			} else {
				if (dir == MOVE_LEFT || dir == MOVE_RIGHT) {
					if (parent->layout == L_HORIZ || parent->layout == L_TABBED) {
						can_move = true;
						desired = idx + (dir == MOVE_LEFT ? -1 : 1);
					}
				} else {
					if (parent->layout == L_VERT || parent->layout == L_STACKED) {
						can_move = true;
						desired = idx + (dir == MOVE_UP ? -1 : 1);
					}
				}
			}
		}

		if (can_move) {
			if (container->is_floating) {
				if (desired < 0) {
					wrap_candidate = parent->floating->items[parent->floating->length-1];
				} else if (desired >= parent->floating->length){
					wrap_candidate = parent->floating->items[0];
				} else {
					wrap_candidate = parent->floating->items[desired];
				}
				if (wrap_candidate) {
					wlc_view_bring_to_front(wrap_candidate->handle);
				}
				return wrap_candidate;
			} else if (desired < 0 || desired >= parent->children->length) {
				can_move = false;
				int len = parent->children->length;
				if (!wrap_candidate && len > 1) {
					if (desired < 0) {
						wrap_candidate = parent->children->items[len-1];
					} else {
						wrap_candidate = parent->children->items[0];
					}
					if (config->force_focus_wrapping) {
						return wrap_candidate;
					}
				}
			} else {
				sway_log(L_DEBUG, "%s cont %d-%p dir %i sibling %d: %p", __func__,
					 idx, container, dir, desired, parent->children->items[desired]);
				return parent->children->items[desired];
			}
		}
		if (!can_move) {
			container = parent;
			parent = parent->parent;
			if (!parent || container == limit) {
				// wrapping is the last chance
				return wrap_candidate;
			}
		}
	}
}

swayc_t *get_swayc_in_direction(swayc_t *container, enum movement_direction dir) {
	return get_swayc_in_direction_under(container, dir, NULL);
}

void recursive_resize(swayc_t *container, double amount, enum wlc_resize_edge edge) {
	int i;
	bool layout_match = true;
	sway_log(L_DEBUG, "Resizing %p with amount: %f", container, amount);
	if (edge == WLC_RESIZE_EDGE_LEFT || edge == WLC_RESIZE_EDGE_RIGHT) {
		container->width += amount;
		layout_match = container->layout == L_HORIZ;
	} else if (edge == WLC_RESIZE_EDGE_TOP || edge == WLC_RESIZE_EDGE_BOTTOM) {
		container->height += amount;
		layout_match = container->layout == L_VERT;
	}
	if (container->type == C_VIEW) {
		update_geometry(container);
		return;
	}
	if (layout_match) {
		for (i = 0; i < container->children->length; i++) {
			recursive_resize(container->children->items[i], amount/container->children->length, edge);
		}
	} else {
		for (i = 0; i < container->children->length; i++) {
			recursive_resize(container->children->items[i], amount, edge);
		}
	}
}

enum swayc_layouts default_layout(swayc_t *output) {
	if (config->default_layout != L_NONE) {
		return config->default_layout;
	} else if (config->default_orientation != L_NONE) {
		return config->default_orientation;
	} else if (output->width >= output->height) {
		return L_HORIZ;
	} else {
		return L_VERT;
	}
}

bool is_auto_layout(enum swayc_layouts layout) {
	return (layout >= L_AUTO_FIRST) && (layout <= L_AUTO_LAST);
}

/**
 * Return the number of master elements in a container
 */
static inline size_t auto_master_count(const swayc_t *container) {
	sway_assert(container->children->length >= 0, "Container %p has (negative) children %d",
			container, container->children->length);
	return MIN(container->nb_master, (size_t)container->children->length);
}

/**
 * Return the number of children in the slave groups. This corresponds to the children
 * that are not members of the master group.
 */
static inline size_t auto_slave_count(const swayc_t *container) {
	return container->children->length - auto_master_count(container);
}

/**
 * Return the number of slave groups in the container.
 */
size_t auto_slave_group_count(const swayc_t *container) {
	return MIN(container->nb_slave_groups, auto_slave_count(container));
}

/**
 * Return the combined number of master and slave groups in the container.
 */
size_t auto_group_count(const swayc_t *container) {
	return auto_slave_group_count(container)
		+ (container->children->length && container->nb_master ? 1 : 0);
}

/**
 * given the index of a container's child, return the index of the first child of the group
 * which index is a member of.
 */
int auto_group_start_index(const swayc_t *container, int index) {
	if (index < 0 || ! is_auto_layout(container->layout)
		|| (size_t)index < container->nb_master) {
		return 0;
	} else {
		size_t nb_slaves = auto_slave_count(container);
		size_t nb_slave_grp = auto_slave_group_count(container);
		size_t grp_sz = nb_slaves / nb_slave_grp;
		size_t remainder = nb_slaves % nb_slave_grp;
		int idx2 = (nb_slave_grp - remainder) * grp_sz + container->nb_master;
		int start_idx;
		if (index < idx2) {
			start_idx = ((index - container->nb_master) / grp_sz) * grp_sz + container->nb_master;
		} else {
			start_idx = idx2 + ((index - idx2) / (grp_sz + 1)) * (grp_sz + 1);
		}
		return MIN(start_idx, container->children->length);
	}
}

/**
 * given the index of a container's child, return the index of the first child of the group
 * that follows the one which index is a member of.
 * This makes the function usable to walk through the groups in a container.
 */
int auto_group_end_index(const swayc_t *container, int index) {
	if (index < 0 || ! is_auto_layout(container->layout)) {
		return container->children->length;
	} else {
		int nxt_idx;
		if ((size_t)index < container->nb_master) {
			nxt_idx = auto_master_count(container);
		} else {
			size_t nb_slaves = auto_slave_count(container);
			size_t nb_slave_grp = auto_slave_group_count(container);
			size_t grp_sz = nb_slaves / nb_slave_grp;
			size_t remainder = nb_slaves % nb_slave_grp;
			int idx2 = (nb_slave_grp - remainder) * grp_sz + container->nb_master;
			if (index < idx2) {
				nxt_idx = ((index - container->nb_master) / grp_sz + 1) * grp_sz + container->nb_master;
			} else {
				nxt_idx = idx2 + ((index - idx2) / (grp_sz + 1) + 1) * (grp_sz + 1);
			}
		}
		return MIN(nxt_idx, container->children->length);
	}
}

/**
 * return the index of the Group containing <index>th child of <container>.
 * The index is the order of the group along the container's major axis (starting at 0).
 */
size_t auto_group_index(const swayc_t *container, int index) {
	if (index < 0) {
		return 0;
	}
	bool master_first = (container->layout == L_AUTO_LEFT || container->layout == L_AUTO_TOP);
	size_t nb_slaves = auto_slave_count(container);
	if ((size_t)index < container->nb_master) {
		if (master_first || nb_slaves <= 0) {
			return 0;
		} else {
			return auto_slave_group_count(container);
		}
	} else {
		size_t nb_slave_grp = auto_slave_group_count(container);
		size_t grp_sz = nb_slaves / nb_slave_grp;
		size_t remainder = nb_slaves % nb_slave_grp;
		int idx2 = (nb_slave_grp - remainder) * grp_sz + container->nb_master;
		size_t grp_idx;
		if (index < idx2) {
			grp_idx = (index - container->nb_master) / grp_sz;
		} else {
			grp_idx = (nb_slave_grp - remainder) + (index - idx2) / (grp_sz + 1) ;
		}
		return grp_idx + (master_first && container-> nb_master ? 1 : 0);
	}
}

/**
 * Return the first index (inclusive) and last index (exclusive) of the elements of a group in
 * an auto layout.
 * If the bounds of the given group can be calculated, they are returned in the start/end
 * parameters (int pointers) and the return value will be true.
 * The indexes are passed by reference and can be NULL.
 */
bool auto_group_bounds(const swayc_t *container, size_t group_index, int *start, int *end) {
	size_t nb_grp = auto_group_count(container);
	if (group_index >= nb_grp) {
		return false;
	}
	bool master_first = (container->layout == L_AUTO_LEFT || container->layout == L_AUTO_TOP);
	size_t nb_master = auto_master_count(container);
	size_t nb_slave_grp = auto_slave_group_count(container);
	int g_start, g_end;
	if (nb_master && (master_first ? group_index == 0 : group_index == nb_grp - 1)) {
		g_start = 0;
		g_end = nb_master;
	} else {
		size_t nb_slaves = auto_slave_count(container);
		size_t grp_sz = nb_slaves / nb_slave_grp;
		size_t remainder = nb_slaves % nb_slave_grp;
		size_t g0 = master_first && container->nb_master ? 1 : 0;
		size_t g1 = g0 + nb_slave_grp - remainder;
		if (group_index < g1) {
			g_start = container->nb_master + (group_index - g0) * grp_sz;
			g_end = g_start + grp_sz;
		} else {
			size_t g2 = group_index - g1;
			g_start = container->nb_master
				+ (nb_slave_grp - remainder) * grp_sz
				+ g2 * (grp_sz + 1);
			g_end = g_start + grp_sz + 1;
		}
	}
	if (start) {
		*start = g_start;
	}
	if (end) {
		*end = g_end;
	}
	return true;
}
