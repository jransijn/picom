// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include "picom_assert.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "utils/kernel.h"
#include "utils/utils.h"

#include "backend/backend_common.h"
#include "backend/xrender/xrender.h"
#include "common.h"
#include "picom.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "win.h"
#include "x.h"
#include "types.h"

struct _xrender_image_data {
	// Pixmap that the client window draws to,
	// it will contain the content of client window.
	xcb_pixmap_t pixmap;
	// A Picture links to the Pixmap
	xcb_render_picture_t pict;
	int width, height;
	// The effective size of the image
	int ewidth, eheight;
	bool has_alpha;
	double opacity;
	xcb_visualid_t visual;
	uint8_t depth;
	bool owned;
};

static void compose(backend_t *base, void *img_data, int dst_x, int dst_y,
                    const region_t *reg_paint, const region_t *reg_visible) {
	struct _xrender_data *xd = (void *)base;
	struct _xrender_image_data *img = img_data;
	uint8_t op = (img->has_alpha ? XCB_RENDER_PICT_OP_OVER : XCB_RENDER_PICT_OP_SRC);
	auto alpha_pict = xd->alpha_pict[(int)(img->opacity * MAX_ALPHA)];
	region_t reg;
	pixman_region32_init(&reg);
	pixman_region32_intersect(&reg, (region_t *)reg_paint, (region_t *)reg_visible);

	// Clip region of rendered_pict might be set during rendering, clear it to make
	// sure we get everything into the buffer
	x_clear_picture_clip_region(base->c, img->pict);

	x_set_picture_clip_region(base->c, xd->back[2], 0, 0, &reg);
	xcb_render_composite(base->c, op, img->pict, alpha_pict, xd->back[2], 0, 0, 0, 0,
	                     to_i16_checked(dst_x), to_i16_checked(dst_y),
	                     to_u16_checked(img->ewidth), to_u16_checked(img->eheight));
	pixman_region32_fini(&reg);
}

static void fill(backend_t *base, struct color c, const region_t *clip) {
	struct _xrender_data *xd = (void *)base;
	const rect_t *extent = pixman_region32_extents((region_t *)clip);
	x_set_picture_clip_region(base->c, xd->back[2], 0, 0, clip);
	// color is in X fixed point representation
	xcb_render_fill_rectangles(
	    base->c, XCB_RENDER_PICT_OP_OVER, xd->back[2],
	    (xcb_render_color_t){.red = (uint16_t)(c.red * 0xffff),
	                         .green = (uint16_t)(c.green * 0xffff),
	                         .blue = (uint16_t)(c.blue * 0xffff),
	                         .alpha = (uint16_t)(c.alpha * 0xffff)},
	    1,
	    (xcb_rectangle_t[]){{.x = to_i16_checked(extent->x1),
	                         .y = to_i16_checked(extent->y1),
	                         .width = to_u16_checked(extent->x2 - extent->x1),
	                         .height = to_u16_checked(extent->y2 - extent->y1)}});
}

static void *
bind_pixmap(backend_t *base, xcb_pixmap_t pixmap, struct xvisual_info fmt, bool owned) {
	xcb_generic_error_t *e;
	auto r = xcb_get_geometry_reply(base->c, xcb_get_geometry(base->c, pixmap), &e);
	if (!r) {
		log_error("Invalid pixmap: %#010x", pixmap);
		x_print_error(e->full_sequence, e->major_code, e->minor_code, e->error_code);
		return NULL;
	}

	auto img = ccalloc(1, struct _xrender_image_data);
	img->depth = (uint8_t)fmt.visual_depth;
	img->width = img->ewidth = r->width;
	img->height = img->eheight = r->height;
	img->pixmap = pixmap;
	img->opacity = 1;
	img->has_alpha = fmt.alpha_size != 0;
	img->pict =
	    x_create_picture_with_visual_and_pixmap(base->c, fmt.visual, pixmap, 0, NULL);
	img->owned = owned;
	img->visual = fmt.visual;
	if (img->pict == XCB_NONE) {
		free(img);
		return NULL;
	}
	return img;
}

static void release_image(backend_t *base, void *image) {
	struct _xrender_image_data *img = image;
	xcb_render_free_picture(base->c, img->pict);
	if (img->owned) {
		xcb_free_pixmap(base->c, img->pixmap);
	}
	free(img);
}

static void deinit(backend_t *backend_data) {
	struct _xrender_data *xd = (void *)backend_data;
	for (int i = 0; i < 256; i++) {
		xcb_render_free_picture(xd->base.c, xd->alpha_pict[i]);
	}
	xcb_render_free_picture(xd->base.c, xd->target);
	xcb_render_free_picture(xd->base.c, xd->root_pict);
	for (int i = 0; i < 2; i++) {
		xcb_render_free_picture(xd->base.c, xd->back[i]);
		xcb_free_pixmap(xd->base.c, xd->back_pixmap[i]);
	}
	if (xd->present_event) {
		xcb_unregister_for_special_event(xd->base.c, xd->present_event);
	}
	xcb_render_free_picture(xd->base.c, xd->white_pixel);
	xcb_render_free_picture(xd->base.c, xd->black_pixel);
	free(xd);
}

static void present(backend_t *base, const region_t *region) {
	struct _xrender_data *xd = (void *)base;
	const rect_t *extent = pixman_region32_extents((region_t *)region);
	int16_t orig_x = to_i16_checked(extent->x1), orig_y = to_i16_checked(extent->y1);
	uint16_t region_width = to_u16_checked(extent->x2 - extent->x1),
	         region_height = to_u16_checked(extent->y2 - extent->y1);

	// compose() sets clip region on the back buffer, so clear it first
	x_clear_picture_clip_region(base->c, xd->back[xd->curr_back]);

	// limit the region of update
	x_set_picture_clip_region(base->c, xd->back[2], 0, 0, region);

	if (xd->vsync) {
		// Update the back buffer first, then present
		xcb_render_composite(base->c, XCB_RENDER_PICT_OP_SRC, xd->back[2],
		                     XCB_NONE, xd->back[xd->curr_back], orig_x, orig_y, 0,
		                     0, orig_x, orig_y, region_width, region_height);

		// Make sure we got reply from PresentPixmap before waiting for events,
		// to avoid deadlock
		auto e = xcb_request_check(
		    base->c, xcb_present_pixmap_checked(
		                 xd->base.c, xd->target_win,
		                 xd->back_pixmap[xd->curr_back], 0, XCB_NONE, XCB_NONE, 0,
		                 0, XCB_NONE, XCB_NONE, XCB_NONE, 0, 0, 0, 0, 0, NULL));
		if (e) {
			log_error("Failed to present pixmap");
			free(e);
			return;
		}
		// TODO don't block wait for present completion
		xcb_present_generic_event_t *pev =
		    (void *)xcb_wait_for_special_event(base->c, xd->present_event);
		if (!pev) {
			// We don't know what happened, maybe X died
			// But reset buffer age, so in case we do recover, we will
			// render correctly.
			xd->buffer_age[0] = xd->buffer_age[1] = -1;
			return;
		}
		assert(pev->evtype == XCB_PRESENT_COMPLETE_NOTIFY);
		xcb_present_complete_notify_event_t *pcev = (void *)pev;
		// log_trace("Present complete: %d %ld", pcev->mode, pcev->msc);
		xd->buffer_age[xd->curr_back] = 1;

		// buffer_age < 0 means that back buffer is empty
		if (xd->buffer_age[1 - xd->curr_back] > 0) {
			xd->buffer_age[1 - xd->curr_back]++;
		}
		if (pcev->mode == XCB_PRESENT_COMPLETE_MODE_FLIP) {
			// We cannot use the pixmap we used anymore
			xd->curr_back = 1 - xd->curr_back;
		}
		free(pev);
	} else {
		// No vsync needed, draw into the target picture directly
		xcb_render_composite(base->c, XCB_RENDER_PICT_OP_SRC, xd->back[2],
		                     XCB_NONE, xd->target, orig_x, orig_y, 0, 0, orig_x,
		                     orig_y, region_width, region_height);
	}
}

static int buffer_age(backend_t *backend_data) {
	struct _xrender_data *xd = (void *)backend_data;
	if (!xd->vsync) {
		// Only the target picture really holds the screen content, and its
		// content is always up to date. So buffer age is always 1.
		return 1;
	}
	return xd->buffer_age[xd->curr_back];
}

static bool is_image_transparent(backend_t *bd attr_unused, void *image) {
	struct _xrender_image_data *img = image;
	return img->has_alpha;
}

static bool image_op(backend_t *base, enum image_operations op, void *image,
                     const region_t *reg_op, const region_t *reg_visible, void *arg) {
	struct _xrender_data *xd = (void *)base;
	struct _xrender_image_data *img = image;
	region_t reg;
	double *dargs = arg;
	int *iargs = arg;
	if (op == IMAGE_OP_APPLY_ALPHA_ALL) {
		img->opacity *= dargs[0];
		img->has_alpha = true;
		return true;
	}

	pixman_region32_init(&reg);

	const auto tmpw = to_u16_checked(img->width);
	const auto tmph = to_u16_checked(img->height);
	switch (op) {
	case IMAGE_OP_INVERT_COLOR_ALL:
		x_set_picture_clip_region(base->c, img->pict, 0, 0, reg_visible);
		if (img->has_alpha) {
			auto tmp_pict =
			    x_create_picture_with_visual(base->c, base->root, img->width,
			                                 img->height, img->visual, 0, NULL);
			xcb_render_composite(base->c, XCB_RENDER_PICT_OP_SRC, img->pict,
			                     XCB_NONE, tmp_pict, 0, 0, 0, 0, 0, 0, tmpw, tmph);

			xcb_render_composite(base->c, XCB_RENDER_PICT_OP_DIFFERENCE,
			                     xd->white_pixel, XCB_NONE, img->pict, 0, 0,
			                     0, 0, 0, 0, tmpw, tmph);
			xcb_render_composite(base->c, XCB_RENDER_PICT_OP_IN_REVERSE,
			                     tmp_pict, XCB_NONE, img->pict, 0, 0, 0, 0, 0,
			                     0, tmpw, tmph);
			xcb_render_free_picture(base->c, tmp_pict);
		} else {
			xcb_render_composite(base->c, XCB_RENDER_PICT_OP_DIFFERENCE,
			                     xd->white_pixel, XCB_NONE, img->pict, 0, 0,
			                     0, 0, 0, 0, tmpw, tmph);
		}
		break;
	case IMAGE_OP_DIM_ALL:
		x_set_picture_clip_region(base->c, img->pict, 0, 0, reg_visible);

		xcb_render_color_t color = {
		    .red = 0, .green = 0, .blue = 0, .alpha = (uint16_t)(0xffff * dargs[0])};

		// Dim the actually content of window
		xcb_rectangle_t rect = {
		    .x = 0,
		    .y = 0,
		    .width = tmpw,
		    .height = tmph,
		};

		xcb_render_fill_rectangles(base->c, XCB_RENDER_PICT_OP_OVER, img->pict,
		                           color, 1, &rect);
		break;
	case IMAGE_OP_APPLY_ALPHA:
		assert(reg_op);
		pixman_region32_intersect(&reg, (region_t *)reg_op, (region_t *)reg_visible);
		if (!pixman_region32_not_empty(&reg)) {
			break;
		}

		if (dargs[0] == 1) {
			break;
		}

		auto alpha_pict = xd->alpha_pict[(int)((1 - dargs[0]) * MAX_ALPHA)];
		x_set_picture_clip_region(base->c, img->pict, 0, 0, &reg);
		xcb_render_composite(base->c, XCB_RENDER_PICT_OP_OUT_REVERSE, alpha_pict,
		                     XCB_NONE, img->pict, 0, 0, 0, 0, 0, 0, tmpw, tmph);
		img->has_alpha = true;
		break;
	case IMAGE_OP_RESIZE_TILE:
		img->ewidth = iargs[0];
		img->eheight = iargs[1];
		break;
	case IMAGE_OP_APPLY_ALPHA_ALL: assert(false);
	case IMAGE_OP_MAX_BRIGHTNESS: assert(false);
	}
	pixman_region32_fini(&reg);
	return true;
}

// TODO: use copy-on-write
static void *copy(backend_t *base, const void *image, const region_t *reg) {
	const struct _xrender_image_data *img = image;
	struct _xrender_data *xd = (void *)base;
	auto new_img = ccalloc(1, struct _xrender_image_data);
	assert(img->visual != XCB_NONE);
	log_trace("xrender: copying %#010x visual %#x", img->pixmap, img->visual);
	*new_img = *img;
	x_set_picture_clip_region(base->c, img->pict, 0, 0, reg);
	new_img->pixmap =
	    x_create_pixmap(base->c, img->depth, base->root, img->width, img->height);
	new_img->opacity = 1;
	new_img->owned = true;
	if (new_img->pixmap == XCB_NONE) {
		log_error("Failed to create pixmap for copy");
		free(new_img);
		return NULL;
	}
	new_img->pict = x_create_picture_with_visual_and_pixmap(base->c, img->visual,
	                                                        new_img->pixmap, 0, NULL);
	if (new_img->pict == XCB_NONE) {
		log_error("Failed to create picture for copy");
		xcb_free_pixmap(base->c, new_img->pixmap);
		free(new_img);
		return NULL;
	}

	xcb_render_picture_t alpha_pict =
	    img->opacity == 1 ? XCB_NONE : xd->alpha_pict[(int)(img->opacity * MAX_ALPHA)];
	xcb_render_composite(base->c, XCB_RENDER_PICT_OP_SRC, img->pict, alpha_pict,
	                     new_img->pict, 0, 0, 0, 0, 0, 0, to_u16_checked(img->width),
	                     to_u16_checked(img->height));
	return new_img;
}

backend_t *backend_xrender_init(session_t *ps) {
	auto xd = ccalloc(1, struct _xrender_data);
	init_backend_base(&xd->base, ps);

	for (int i = 0; i <= MAX_ALPHA; ++i) {
		double o = (double)i / (double)MAX_ALPHA;
		xd->alpha_pict[i] = solid_picture(ps->c, ps->root, false, o, 0, 0, 0);
		assert(xd->alpha_pict[i] != XCB_NONE);
	}

	xd->target_width = ps->root_width;
	xd->target_height = ps->root_height;
	xd->default_visual = ps->vis;
	xd->black_pixel = solid_picture(ps->c, ps->root, true, 1, 0, 0, 0);
	xd->white_pixel = solid_picture(ps->c, ps->root, true, 1, 1, 1, 1);

	xd->target_win = session_get_target_window(ps);
	xcb_render_create_picture_value_list_t pa = {
	    .subwindowmode = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS,
	};
	xd->target = x_create_picture_with_visual_and_pixmap(
	    ps->c, ps->vis, xd->target_win, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);

	auto pictfmt = x_get_pictform_for_visual(ps->c, ps->vis);
	if (!pictfmt) {
		log_fatal("Default visual is invalid");
		abort();
	}

	xd->vsync = ps->o.vsync;
	if (ps->present_exists) {
		auto eid = x_new_id(ps->c);
		auto e =
		    xcb_request_check(ps->c, xcb_present_select_input_checked(
		                                 ps->c, eid, xd->target_win,
		                                 XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
		if (e) {
			log_error("Cannot select present input, vsync will be disabled");
			xd->vsync = false;
			free(e);
		}

		xd->present_event =
		    xcb_register_for_special_xge(ps->c, &xcb_present_id, eid, NULL);
		if (!xd->present_event) {
			log_error("Cannot register for special XGE, vsync will be "
			          "disabled");
			xd->vsync = false;
		}
	} else {
		xd->vsync = false;
	}

	// We might need to do double buffering for vsync, and buffer 0 and 1 are for
	// double buffering.
	int first_buffer_index = xd->vsync ? 0 : 2;
	for (int i = first_buffer_index; i < 3; i++) {
		xd->back_pixmap[i] = x_create_pixmap(ps->c, pictfmt->depth, ps->root,
		                                     to_u16_checked(ps->root_width),
		                                     to_u16_checked(ps->root_height));
		xd->back[i] = x_create_picture_with_pictfmt_and_pixmap(
		    ps->c, pictfmt, xd->back_pixmap[i], 0, NULL);
		xd->buffer_age[i] = -1;
		if (xd->back_pixmap[i] == XCB_NONE || xd->back[i] == XCB_NONE) {
			log_error("Cannot create pixmap for rendering");
			goto err;
		}
	}
	xd->curr_back = 0;

	xcb_pixmap_t root_pixmap = x_get_root_back_pixmap(ps);
	if (root_pixmap == XCB_NONE) {
		xd->root_pict = solid_picture(ps->c, ps->root, false, 1, 0.5, 0.5, 0.5);
	} else {
		xd->root_pict = x_create_picture_with_visual_and_pixmap(
		    ps->c, ps->vis, root_pixmap, 0, NULL);
	}
	return &xd->base;
err:
	deinit(&xd->base);
	return NULL;
}

struct backend_operations xrender_ops = {
    .init = backend_xrender_init,
    .deinit = deinit,
    .present = present,
    .compose = compose,
    .fill = fill,
    .bind_pixmap = bind_pixmap,
    .release_image = release_image,
    .render_shadow = default_backend_render_shadow,
    //.prepare_win = prepare_win,
    //.release_win = release_win,
    .is_image_transparent = is_image_transparent,
    .buffer_age = buffer_age,
    .max_buffer_age = 2,

    .image_op = image_op,
    .copy = copy,
};

// vim: set noet sw=8 ts=8:
