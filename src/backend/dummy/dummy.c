#include <uthash.h>
#include <xcb/xcb.h>

#include "utils/compiler.h"
#include "utils/uthash_extra.h"
#include "utils/utils.h"

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "common.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "types.h"
#include "x.h"

struct dummy_image {
	xcb_pixmap_t pixmap;
	bool transparent;
	int *refcount;
	UT_hash_handle hh;
};

struct dummy_data {
	struct backend_base base;
	struct dummy_image *images;
};

struct backend_base *dummy_init(struct session *ps attr_unused) {
	auto ret = (struct backend_base *)ccalloc(1, struct dummy_data);
	ret->c = ps->c;
	ret->loop = ps->loop;
	ret->root = ps->root;
	ret->busy = false;
	return ret;
}

void dummy_deinit(struct backend_base *data) {
	auto dummy = (struct dummy_data *)data;
	HASH_ITER2(dummy->images, img) {
		log_warn("Backend image for pixmap %#010x is not freed", img->pixmap);
		HASH_DEL(dummy->images, img);
		free(img->refcount);
		free(img);
	}
	free(dummy);
}

static void dummy_check_image(struct backend_base *base, const struct dummy_image *img) {
	auto dummy = (struct dummy_data *)base;
	struct dummy_image *tmp = NULL;
	HASH_FIND_INT(dummy->images, &img->pixmap, tmp);
	if (!tmp) {
		log_warn("Using an invalid (possibly freed) image");
		assert(false);
	}
	assert(*tmp->refcount > 0);
}

void dummy_compose(struct backend_base *base, void *image, int dst_x attr_unused,
                   int dst_y attr_unused, const region_t *reg_paint attr_unused,
                   const region_t *reg_visible attr_unused) {
	dummy_check_image(base, image);
}

void dummy_fill(struct backend_base *backend_data attr_unused, struct color c attr_unused,
                const region_t *clip attr_unused) {
}

void *dummy_bind_pixmap(struct backend_base *base, xcb_pixmap_t pixmap,
                        struct xvisual_info fmt, bool owned attr_unused) {
	auto dummy = (struct dummy_data *)base;
	struct dummy_image *img = NULL;
	HASH_FIND_INT(dummy->images, &pixmap, img);
	if (img) {
		(*img->refcount)++;
		return img;
	}

	img = ccalloc(1, struct dummy_image);
	img->pixmap = pixmap;
	img->transparent = fmt.alpha_size != 0;
	img->refcount = ccalloc(1, int);
	*img->refcount = 1;

	HASH_ADD_INT(dummy->images, pixmap, img);
	return (void *)img;
}

void dummy_release_image(backend_t *base, void *image) {
	auto dummy = (struct dummy_data *)base;
	auto img = (struct dummy_image *)image;
	assert(*img->refcount > 0);
	(*img->refcount)--;
	if (*img->refcount == 0) {
		HASH_DEL(dummy->images, img);
		free(img->refcount);
		free(img);
	}
}

bool dummy_is_image_transparent(struct backend_base *base, void *image) {
	auto img = (struct dummy_image *)image;
	dummy_check_image(base, img);
	return img->transparent;
}

int dummy_buffer_age(struct backend_base *base attr_unused) {
	return 2;
}

bool dummy_image_op(struct backend_base *base, enum image_operations op attr_unused,
                    void *image, const region_t *reg_op attr_unused,
                    const region_t *reg_visible attr_unused, void *args attr_unused) {
	dummy_check_image(base, image);
	return true;
}

void *dummy_image_copy(struct backend_base *base, const void *image,
                       const region_t *reg_visible attr_unused) {
	auto img = (const struct dummy_image *)image;
	dummy_check_image(base, img);
	(*img->refcount)++;
	return (void *)img;
}

struct backend_operations dummy_ops = {
    .init = dummy_init,
    .deinit = dummy_deinit,
    .compose = dummy_compose,
    .fill = dummy_fill,
    .bind_pixmap = dummy_bind_pixmap,
    .render_shadow = default_backend_render_shadow,
    .release_image = dummy_release_image,
    .is_image_transparent = dummy_is_image_transparent,
    .buffer_age = dummy_buffer_age,
    .max_buffer_age = 5,

    .image_op = dummy_image_op,
    .copy = dummy_image_copy,
};
