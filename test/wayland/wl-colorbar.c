// SPDX-License-Identifier: MPL-2.0
//
// A deterministic Wayland client: one fullscreen xdg_toplevel showing four
// vertical color bars. It prints a serial marker so the host-side harness can
// take a QEMU screenshot and assert the bar colors.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

struct state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_output *output;
	int width;
	int height;
	int configured;
};

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
	(void)data;
	(void)shm;
	if (format != WL_SHM_FORMAT_XRGB8888 &&
	    format != WL_SHM_FORMAT_ARGB8888)
		return;
}

static const struct wl_shm_listener shm_listener = {
	.format = shm_format,
};

static void output_geometry(void *data, struct wl_output *output, int32_t x,
			    int32_t y, int32_t physical_width,
			    int32_t physical_height, int32_t subpixel,
			    const char *make, const char *model, int32_t transform)
{
	(void)data;
	(void)output;
	(void)x;
	(void)y;
	(void)physical_width;
	(void)physical_height;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
			int32_t width, int32_t height, int32_t refresh)
{
	struct state *state = data;
	(void)output;
	(void)refresh;
	if ((flags & WL_OUTPUT_MODE_CURRENT) && width > 0 && height > 0) {
		state->width = width;
		state->height = height;
	}
}

static void output_done(void *data, struct wl_output *output)
{
	(void)data;
	(void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
	(void)data;
	(void)output;
	(void)factor;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
			 uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void registry_global(void *data, struct wl_registry *registry,
			    uint32_t name, const char *interface,
			    uint32_t version)
{
	struct state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(
			registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface,
					      1);
		wl_shm_add_listener(state->shm, &shm_listener, state);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		state->wm_base = wl_registry_bind(
			registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(state->wm_base, &wm_base_listener,
					 state);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		state->output = wl_registry_bind(registry, name,
						 &wl_output_interface, 1);
		wl_output_add_listener(state->output, &output_listener, state);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
				   uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static int create_shm_fd(size_t size)
{
	int fd = (int)syscall(SYS_memfd_create, "wl-colorbar",
			      MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0 && errno == ENOSYS) {
		char name[64];
		snprintf(name, sizeof(name), "/wl-colorbar-%d", getpid());
		fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0)
			shm_unlink(name);
	}
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static uint32_t bar_color(int index)
{
	switch (index & 3) {
	case 0:
		return 0x00ff0000;
	case 1:
		return 0x0000ff00;
	case 2:
		return 0x000000ff;
	default:
		return 0x00ffffff;
	}
}

static void xdg_surface_configure(void *data, struct xdg_surface *surface,
				  uint32_t serial)
{
	struct state *state = data;
	xdg_surface_ack_configure(surface, serial);
	state->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
				   int32_t width, int32_t height,
				   struct wl_array *states)
{
	struct state *state = data;
	(void)toplevel;
	(void)states;
	if (width > 0 && height > 0) {
		state->width = width;
		state->height = height;
	}
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	(void)data;
	(void)toplevel;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static void buffer_release(void *data, struct wl_buffer *buffer)
{
	int *released = data;
	(void)buffer;
	*released = 1;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

int main(void)
{
	struct state state = {
		.width = 1280,
		.height = 800,
	};
	state.display = wl_display_connect(NULL);
	if (!state.display) {
		fprintf(stderr, "WL-COLORBAR: FAIL (connect: %s)\n",
			strerror(errno));
		return 1;
	}

	state.registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(state.registry, &registry_listener, &state);
	wl_display_roundtrip(state.display);

	if (!state.compositor || !state.shm || !state.wm_base) {
		fprintf(stderr,
			"WL-COLORBAR: FAIL (missing compositor/shm/xdg_wm_base)\n");
		return 1;
	}

	printf("WL-COLORBAR: output=%dx%d\n", state.width, state.height);
	fflush(stdout);

	struct wl_surface *surface = wl_compositor_create_surface(state.compositor);
	struct xdg_surface *xdg_surface =
		xdg_wm_base_get_xdg_surface(state.wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, &state);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &xdg_toplevel_listener, &state);
	xdg_toplevel_set_title(toplevel, "Asterinas Wayland Colorbar");
	xdg_toplevel_set_app_id(toplevel, "asterinas-colorbar");
	xdg_toplevel_set_fullscreen(toplevel, NULL);
	wl_surface_commit(surface);

	while (!state.configured)
		wl_display_dispatch(state.display);

	int width = state.width > 0 ? state.width : 1280;
	int height = state.height > 0 ? state.height : 800;
	size_t stride = (size_t)width * 4;
	size_t size = stride * (size_t)height;
	int fd = create_shm_fd(size);
	if (fd < 0) {
		fprintf(stderr, "WL-COLORBAR: FAIL (shm fd: %s)\n",
			strerror(errno));
		return 1;
	}
	uint32_t *pixels =
		mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "WL-COLORBAR: FAIL (mmap: %s)\n",
			strerror(errno));
		return 1;
	}
	for (int y = 0; y < height; y++) {
		uint32_t *row = pixels + (size_t)y * (size_t)width;
		for (int x = 0; x < width; x++)
			row[x] = bar_color((x * 4) / width);
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(state.shm, fd, (int32_t)size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(
		pool, 0, width, height, (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
	int released = 0;
	wl_buffer_add_listener(buffer, &buffer_listener, &released);
	wl_shm_pool_destroy(pool);

	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	wl_display_roundtrip(state.display);

	printf("WAYLAND-TEST: COLORBAR-READY %dx%d\n", width, height);
	fflush(stdout);
	FILE *serial = fopen("/dev/ttyS0", "w");
	if (serial) {
		fprintf(serial, "WAYLAND-TEST: COLORBAR-READY %dx%d\n", width,
			height);
		fclose(serial);
	}

	sleep(60);

	(void)released;
	munmap(pixels, size);
	close(fd);
	wl_buffer_destroy(buffer);
	xdg_toplevel_destroy(toplevel);
	xdg_surface_destroy(xdg_surface);
	wl_surface_destroy(surface);
	wl_display_disconnect(state.display);
	return 0;
}
