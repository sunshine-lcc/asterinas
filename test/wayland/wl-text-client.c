// SPDX-License-Identifier: MPL-2.0
//
// A minimal Wayland client that renders terminal-style text into a wl_shm
// buffer. It uses only wl_compositor and wl_shm, so the companion compositor
// does not need a shell, seat, or DRM objects.

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/memfd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>

#include "font8x8_basic.h"

struct client_state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
};

struct canvas {
	uint32_t *pixels;
	int width;
	int height;
	int stride;
	int cursor_x;
	int cursor_y;
	int scale;
	uint32_t color;
};

static void log_serial(const char *message)
{
	FILE *f = fopen("/dev/ttyS0", "w");
	if (f != NULL) {
		fprintf(f, "WAYLAND-TEXT-CLIENT: %s\n", message);
		fclose(f);
	}
}

static void registry_global(void *data, struct wl_registry *registry,
			    uint32_t name, const char *interface,
			    uint32_t version)
{
	(void)version;
	struct client_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(
			registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name,
					      &wl_shm_interface, 1);
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

static void canvas_fill(struct canvas *canvas, uint32_t color)
{
	for (int y = 0; y < canvas->height; y++) {
		uint32_t *row = canvas->pixels + (size_t)y * canvas->width;
		for (int x = 0; x < canvas->width; x++) {
			row[x] = color;
		}
	}
}

static void canvas_rect(struct canvas *canvas, int x, int y, int w, int h,
			uint32_t color)
{
	for (int yy = y; yy < y + h; yy++) {
		if (yy < 0 || yy >= canvas->height) {
			continue;
		}
		uint32_t *row = canvas->pixels + (size_t)yy * canvas->width;
		for (int xx = x; xx < x + w; xx++) {
			if (xx >= 0 && xx < canvas->width) {
				row[xx] = color;
			}
		}
	}
}

static void canvas_char(struct canvas *canvas, char ch)
{
	unsigned char code = (unsigned char)ch;
	if (code >= 128) {
		code = '?';
	}
	const unsigned char *glyph = font8x8_basic[code];
	int scale = canvas->scale;
	int cell_w = 8 * scale;

	for (int row = 0; row < 8; row++) {
		for (int col = 0; col < 8; col++) {
			if ((glyph[row] & (1u << col)) == 0) {
				continue;
			}
			canvas_rect(canvas, canvas->cursor_x + col * scale,
				    canvas->cursor_y + row * scale,
				    scale, scale, canvas->color);
		}
	}
	canvas->cursor_x += cell_w;
}

static void canvas_newline(struct canvas *canvas)
{
	canvas->cursor_x = 0;
	canvas->cursor_y += 8 * canvas->scale + 6;
}

static void canvas_text(struct canvas *canvas, const char *text)
{
	for (const char *p = text; *p != '\0'; p++) {
		if (*p == '\n') {
			canvas_newline(canvas);
		} else if (*p == '\t') {
			canvas->cursor_x += 4 * 8 * canvas->scale;
		} else if (*p == '\r') {
			continue;
		} else {
			if (canvas->cursor_x + 8 * canvas->scale >
			    canvas->width) {
				canvas_newline(canvas);
			}
			canvas_char(canvas, *p);
		}
	}
}

static void strip_ansi(const char *input, char *output, size_t output_size)
{
	size_t out = 0;
	/* 0 = text, 1 = ESC, 2 = CSI, 3 = OSC, 4 = OSC ESC */
	int state = 0;
	for (const unsigned char *p = (const unsigned char *)input;
	     *p != '\0' && out + 1 < output_size; p++) {
		unsigned char c = *p;
		switch (state) {
		case 1:
			if (c == '[') {
				state = 2;
			} else if (c == ']') {
				state = 3;
			} else {
				state = 0;
			}
			continue;
		case 2:
			if (c >= 0x40 && c <= 0x7e) {
				state = 0;
			}
			continue;
		case 3:
			if (c == 0x07) {
				state = 0;
			} else if (c == 0x1b) {
				state = 4;
			}
			continue;
		case 4:
			state = c == '\\' ? 0 : 3;
			continue;
		default:
			break;
		}
		if (c == 0x1b) {
			state = 1;
			continue;
		}
		if (c == '\r') {
			continue;
		}
		if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f)) {
			output[out++] = (char)c;
		} else if (c >= 0x80) {
			/* The 8x8 font is ASCII-only; keep block art visible. */
			output[out++] = '#';
		}
	}
	output[out] = '\0';
}

static int run_command(const char *command, char *output, size_t output_size)
{
	FILE *pipe = popen(command, "r");
	if (pipe == NULL) {
		return -1;
	}
	size_t used = fread(output, 1, output_size - 1, pipe);
	output[used] = '\0';
	pclose(pipe);
	return 0;
}

static int create_shm_fd(size_t size)
{
	int fd = (int)syscall(SYS_memfd_create, "wl-text",
			      MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0 && errno == ENOSYS) {
		char name[64];
		snprintf(name, sizeof(name), "/wl-text-%d", getpid());
		fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
		}
	}
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(void)
{
	struct client_state state = {0};
	state.display = wl_display_connect(NULL);
	if (state.display == NULL) {
		fprintf(stderr, "WAYLAND-TEXT-CLIENT: connect failed: %s\n",
			strerror(errno));
		return 1;
	}

	state.registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(state.registry, &registry_listener, &state);
	wl_display_roundtrip(state.display);

	if (state.compositor == NULL || state.shm == NULL) {
		fprintf(stderr, "WAYLAND-TEXT-CLIENT: missing globals\n");
		return 1;
	}

	int fb_fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
	struct fb_var_screeninfo var;
	int width = 1280;
	int height = 800;
	if (fb_fd >= 0 && ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) == 0) {
		width = (int)var.xres;
		height = (int)var.yres;
	}
	if (fb_fd >= 0) {
		close(fb_fd);
	}

	int stride = width * 4;
	size_t size = (size_t)stride * height;
	int fd = create_shm_fd(size);
	if (fd < 0) {
		fprintf(stderr, "WAYLAND-TEXT-CLIENT: shm fd failed\n");
		return 1;
	}
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE,
				MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "WAYLAND-TEXT-CLIENT: mmap failed\n");
		return 1;
	}

	struct canvas canvas = {
		.pixels = pixels,
		.width = width,
		.height = height,
		.stride = stride,
		.cursor_x = 0,
		.cursor_y = 0,
		.scale = 2,
		.color = 0x00e8e8e8,
	};

	canvas_fill(&canvas, 0x00101820);
	canvas_rect(&canvas, 0, 0, width, 8 * canvas.scale + 10,
		    0x00304868);
	canvas.color = 0x00a8e0ff;
	canvas_text(&canvas, " Asterinas Wayland Text Demo\n");
	canvas_newline(&canvas);
	canvas.color = 0x00e8e8e8;

	canvas_text(&canvas,
		    "root@asterinas-ubuntu:~# wayland-info | head\n");
	char info[4096];
	if (run_command("wayland-info 2>/dev/null | head -n 8", info,
			sizeof(info)) == 0 && info[0] != '\0') {
		canvas.color = 0x00b8d8b8;
		canvas_text(&canvas, info);
		canvas.color = 0x00e8e8e8;
	}
	canvas_newline(&canvas);

	canvas_text(&canvas, "root@asterinas-ubuntu:~# uname -a\n");
	char uname_output[2048];
	if (run_command("uname -a", uname_output, sizeof(uname_output)) == 0) {
		canvas.color = 0x00ffd080;
		canvas_text(&canvas, uname_output);
		canvas.color = 0x00e8e8e8;
	}
	canvas_newline(&canvas);

	canvas_text(&canvas, "root@asterinas-ubuntu:~# screenfetch\n");
	char screenfetch_raw[8192];
	char screenfetch[8192];
	if (run_command("screenfetch -E 2>/dev/null", screenfetch_raw,
			sizeof(screenfetch_raw)) == 0 &&
	    screenfetch_raw[0] != '\0') {
		strip_ansi(screenfetch_raw, screenfetch, sizeof(screenfetch));
		canvas.color = 0x00d0d0ff;
		canvas_text(&canvas, screenfetch);
		canvas.color = 0x00e8e8e8;
	} else {
		canvas_text(&canvas, "screenfetch: not available\n");
	}

	canvas_text(&canvas,
		    "\nWayland: wl_compositor + wl_shm\n"
		    "Scanout: /dev/fb0 (simpledrm framebuffer)\n"
		    "Compositor: wl-text-compositor\n");

	struct wl_shm_pool *pool =
		wl_shm_create_pool(state.shm, fd, (int32_t)size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(
		pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);

	struct wl_surface *surface =
		wl_compositor_create_surface(state.compositor);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	wl_display_roundtrip(state.display);

	printf("WAYLAND-TEXT-CLIENT: READY %dx%d\n", width, height);
	fflush(stdout);
	log_serial("READY");

	/* Keep the surface content on screen for the host-side screenshot. */
	sleep(600);

	wl_buffer_destroy(buffer);
	wl_surface_destroy(surface);
	munmap(pixels, size);
	close(fd);
	wl_display_disconnect(state.display);
	return 0;
}
