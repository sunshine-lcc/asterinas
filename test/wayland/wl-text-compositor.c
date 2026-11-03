// SPDX-License-Identifier: MPL-2.0
//
// A deliberately tiny Wayland compositor that scans out through /dev/fb0.
//
// It implements just enough of the core protocol for a full-screen wl_shm
// client: wl_compositor, wl_surface, wl_shm, and wl_callback. There are no
// shell roles, seats, or DRM objects. This intentionally bypasses the DRM
// dumb-buffer path while still exercising the Wayland protocol.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wayland-server-protocol.h>

struct fb_state {
	int fd;
	uint8_t *data;
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	unsigned int bytes_pp;
};

struct surface {
	struct wl_resource *resource;
	struct wl_resource *pending_buffer;
	struct wl_list frame_callbacks;
};

static struct fb_state fb;

static void log_serial(const char *message)
{
	FILE *f = fopen("/dev/ttyS0", "w");
	if (f != NULL) {
		fprintf(f, "WAYLAND-TEXT-COMPOSITOR: %s\n", message);
		fclose(f);
	}
}

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void blit_shm_buffer(struct wl_shm_buffer *shm)
{
	int width = wl_shm_buffer_get_width(shm);
	int height = wl_shm_buffer_get_height(shm);
	int stride = wl_shm_buffer_get_stride(shm);
	uint32_t format = wl_shm_buffer_get_format(shm);
	uint8_t *src = wl_shm_buffer_get_data(shm);

	if (fb.bytes_pp != 4) {
		return;
	}
	if (format != WL_SHM_FORMAT_XRGB8888 &&
	    format != WL_SHM_FORMAT_ARGB8888) {
		return;
	}

	int copy_width = width < (int)fb.var.xres ? width : (int)fb.var.xres;
	int copy_height = height < (int)fb.var.yres ? height : (int)fb.var.yres;
	size_t row_bytes = (size_t)copy_width * 4;

	for (int y = 0; y < copy_height; y++) {
		memcpy(fb.data + (size_t)y * fb.fix.line_length,
		       src + (size_t)y * stride, row_bytes);
	}
}

static void surface_handle_destroy(struct wl_client *client,
				   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void surface_handle_attach(struct wl_client *client,
				  struct wl_resource *resource,
				  struct wl_resource *buffer,
				  int32_t x, int32_t y)
{
	(void)client;
	(void)x;
	(void)y;
	struct surface *surface = wl_resource_get_user_data(resource);

	if (surface->pending_buffer != NULL &&
	    surface->pending_buffer != buffer) {
		wl_buffer_send_release(surface->pending_buffer);
	}
	surface->pending_buffer = buffer;
}

static void surface_handle_damage(struct wl_client *client,
				  struct wl_resource *resource,
				  int32_t x, int32_t y,
				  int32_t width, int32_t height)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static void surface_handle_frame(struct wl_client *client,
				 struct wl_resource *resource, uint32_t callback)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	struct wl_resource *cb = wl_resource_create(
		client, &wl_callback_interface, 1, callback);
	if (cb == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_insert(surface->frame_callbacks.prev,
		       wl_resource_get_link(cb));
}

static void surface_handle_set_opaque_region(struct wl_client *client,
					     struct wl_resource *resource,
					     struct wl_resource *region)
{
	(void)client;
	(void)resource;
	(void)region;
}

static void surface_handle_set_input_region(struct wl_client *client,
					    struct wl_resource *resource,
					    struct wl_resource *region)
{
	(void)client;
	(void)resource;
	(void)region;
}

static void surface_handle_commit(struct wl_client *client,
				  struct wl_resource *resource)
{
	(void)client;
	struct surface *surface = wl_resource_get_user_data(resource);

	if (surface->pending_buffer != NULL) {
		struct wl_shm_buffer *shm =
			wl_shm_buffer_get(surface->pending_buffer);
		if (shm != NULL) {
			wl_shm_buffer_begin_access(shm);
			blit_shm_buffer(shm);
			wl_shm_buffer_end_access(shm);
		}
		wl_buffer_send_release(surface->pending_buffer);
		surface->pending_buffer = NULL;
	}

	uint32_t time = now_ms();
	struct wl_resource *cb, *tmp;
	wl_resource_for_each_safe(cb, tmp, &surface->frame_callbacks) {
		wl_callback_send_done(cb, time);
		wl_resource_destroy(cb);
	}
	wl_list_init(&surface->frame_callbacks);
}

static void surface_handle_set_buffer_transform(struct wl_client *client,
						struct wl_resource *resource,
						int32_t transform)
{
	(void)client;
	(void)resource;
	(void)transform;
}

static void surface_handle_set_buffer_scale(struct wl_client *client,
					    struct wl_resource *resource,
					    int32_t scale)
{
	(void)client;
	(void)resource;
	(void)scale;
}

static void surface_handle_damage_buffer(struct wl_client *client,
					 struct wl_resource *resource,
					 int32_t x, int32_t y,
					 int32_t width, int32_t height)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static const struct wl_surface_interface surface_impl = {
	.destroy = surface_handle_destroy,
	.attach = surface_handle_attach,
	.damage = surface_handle_damage,
	.frame = surface_handle_frame,
	.set_opaque_region = surface_handle_set_opaque_region,
	.set_input_region = surface_handle_set_input_region,
	.commit = surface_handle_commit,
	.set_buffer_transform = surface_handle_set_buffer_transform,
	.set_buffer_scale = surface_handle_set_buffer_scale,
	.damage_buffer = surface_handle_damage_buffer,
};

static void surface_resource_destroy(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (surface->pending_buffer != NULL) {
		wl_buffer_send_release(surface->pending_buffer);
	}
	free(surface);
}

static void compositor_create_surface(struct wl_client *client,
				      struct wl_resource *resource,
				      uint32_t id)
{
	struct surface *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&surface->frame_callbacks);
	surface->resource = wl_resource_create(
		client, &wl_surface_interface,
		wl_resource_get_version(resource), id);
	if (surface->resource == NULL) {
		free(surface);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(surface->resource, &surface_impl,
				       surface, surface_resource_destroy);
}

static void region_handle_destroy(struct wl_client *client,
				  struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void region_handle_add(struct wl_client *client,
			      struct wl_resource *resource,
			      int32_t x, int32_t y,
			      int32_t width, int32_t height)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static void region_handle_subtract(struct wl_client *client,
				   struct wl_resource *resource,
				   int32_t x, int32_t y,
				   int32_t width, int32_t height)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static const struct wl_region_interface region_impl = {
	.destroy = region_handle_destroy,
	.add = region_handle_add,
	.subtract = region_handle_subtract,
};

static void compositor_create_region(struct wl_client *client,
				     struct wl_resource *resource,
				     uint32_t id)
{
	struct wl_resource *region = wl_resource_create(
		client, &wl_region_interface,
		wl_resource_get_version(resource), id);
	if (region == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(region, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
	.create_surface = compositor_create_surface,
	.create_region = compositor_create_region,
};

static void bind_compositor(struct wl_client *client, void *data,
			    uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *resource = wl_resource_create(
		client, &wl_compositor_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &compositor_impl, NULL, NULL);
}

static int init_framebuffer(void)
{
	fb.fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb.fd < 0) {
		perror("open /dev/fb0");
		return -1;
	}
	if (ioctl(fb.fd, FBIOGET_FSCREENINFO, &fb.fix) != 0 ||
	    ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.var) != 0) {
		perror("framebuffer ioctl");
		return -1;
	}
	fb.bytes_pp = fb.var.bits_per_pixel / 8;
	if (fb.bytes_pp != 4) {
		fprintf(stderr, "unsupported framebuffer bpp %u\n",
			fb.var.bits_per_pixel);
		return -1;
	}
	size_t size = (size_t)fb.fix.line_length * fb.var.yres;
	fb.data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fb.fd, 0);
	if (fb.data == MAP_FAILED) {
		perror("mmap framebuffer");
		return -1;
	}
	memset(fb.data, 0, size);
	printf("framebuffer %ux%u %ubpp line=%u\n", fb.var.xres,
	       fb.var.yres, fb.var.bits_per_pixel, fb.fix.line_length);
	return 0;
}

int main(void)
{
	if (init_framebuffer() != 0) {
		return 1;
	}

	struct wl_display *display = wl_display_create();
	if (display == NULL) {
		fprintf(stderr, "wl_display_create failed\n");
		return 1;
	}

	wl_display_init_shm(display);
	wl_display_add_shm_format(display, WL_SHM_FORMAT_XRGB8888);
	wl_display_add_shm_format(display, WL_SHM_FORMAT_ARGB8888);

	if (wl_global_create(display, &wl_compositor_interface, 4, NULL,
			     bind_compositor) == NULL) {
		fprintf(stderr, "wl_global_create failed\n");
		return 1;
	}

	if (wl_display_add_socket(display, "wayland-0") != 0) {
		fprintf(stderr, "wl_display_add_socket failed: %s\n",
			strerror(errno));
		return 1;
	}
	const char *socket = "wayland-0";
	log_serial(socket);
	printf("listening on %s\n", socket);
	fflush(stdout);

	wl_display_run(display);

	wl_display_destroy(display);
	return 0;
}
