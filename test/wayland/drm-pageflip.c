// SPDX-License-Identifier: MPL-2.0
//
// Verify the legacy KMS page-flip path used by wlroots/Weston:
//   SETCRTC(red) -> PAGE_FLIP(green) -> poll/read FLIP_COMPLETE
//
// The test intentionally prints markers and sleeps so a host-side harness can
// capture the QEMU scanout in both states.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_DEVICE "/dev/dri/card0"

struct dumb_buffer {
	uint32_t handle;
	uint32_t fb_id;
	uint32_t pitch;
	uint64_t size;
	uint8_t *map;
};

static int fill_buffer(struct dumb_buffer *buf, uint32_t width, uint32_t height,
		       uint32_t color)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32_t *row = (uint32_t *)(buf->map + y * buf->pitch);
		for (uint32_t x = 0; x < width; x++)
			row[x] = color;
	}
	return 0;
}

static int create_buffer(int fd, uint32_t width, uint32_t height,
			 uint32_t color, struct dumb_buffer *out)
{
	uint32_t handle = 0;
	uint32_t pitch = 0;
	uint64_t size = 0;
	if (drmModeCreateDumbBuffer(fd, width, height, 32, 0, &handle, &pitch,
				    &size) != 0) {
		fprintf(stderr, "CREATE_DUMB failed: %s\n", strerror(errno));
		return -1;
	}

	uint64_t offset = 0;
	if (drmModeMapDumbBuffer(fd, handle, &offset) != 0) {
		fprintf(stderr, "MAP_DUMB failed: %s\n", strerror(errno));
		drmModeDestroyDumbBuffer(fd, handle);
		return -1;
	}

	void *mapped =
		mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	if (mapped == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		drmModeDestroyDumbBuffer(fd, handle);
		return -1;
	}

	if (drmModeAddFB(fd, width, height, 24, 32, pitch, handle,
			 &out->fb_id) != 0) {
		fprintf(stderr, "ADDFB failed: %s\n", strerror(errno));
		munmap(mapped, size);
		drmModeDestroyDumbBuffer(fd, handle);
		return -1;
	}

	out->handle = handle;
	out->pitch = pitch;
	out->size = size;
	out->map = mapped;
	fill_buffer(out, width, height, color);
	return 0;
}

static void destroy_buffer(int fd, struct dumb_buffer *buf)
{
	if (buf->fb_id)
		drmModeRmFB(fd, buf->fb_id);
	if (buf->map)
		munmap(buf->map, buf->size);
	if (buf->handle)
		drmModeDestroyDumbBuffer(fd, buf->handle);
}

static volatile int flip_complete;

static void page_flip_handler(int fd, unsigned int sequence,
			      unsigned int tv_sec, unsigned int tv_usec,
			      unsigned int crtc_id, void *user_data)
{
	(void)fd;
	(void)sequence;
	(void)tv_sec;
	(void)tv_usec;
	(void)crtc_id;
	printf("DRM-TEST: FLIP-COMPLETE user_data=%p\n", user_data);
	fflush(stdout);
	flip_complete = 1;
}

int main(void)
{
	int fd = open(DRM_DEVICE, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT || errno == ENODEV) {
			fprintf(stderr, "DRM-TEST: SKIP (%s: %s)\n", DRM_DEVICE,
				strerror(errno));
			return 0;
		}
		fprintf(stderr, "open(%s) failed: %s\n", DRM_DEVICE,
			strerror(errno));
		return 1;
	}

	if (drmSetMaster(fd) != 0 && errno != EACCES) {
		fprintf(stderr, "SET_MASTER failed: %s\n", strerror(errno));
	}

	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "GETRESOURCES failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *candidate =
			drmModeGetConnector(fd, res->connectors[i]);
		if (!candidate)
			continue;
		if (candidate->connection == DRM_MODE_CONNECTED &&
		    candidate->count_modes > 0) {
			conn = candidate;
			break;
		}
		drmModeFreeConnector(candidate);
	}

	if (!conn) {
		fprintf(stderr, "DRM-TEST: SKIP (no connected connector)\n");
		drmModeFreeResources(res);
		close(fd);
		return 0;
	}

	drmModeEncoder *enc =
		conn->encoder_id ? drmModeGetEncoder(fd, conn->encoder_id) : NULL;
	uint32_t crtc_id = enc && enc->crtc_id ? enc->crtc_id : res->crtcs[0];
	drmModeModeInfo mode = conn->modes[0];
	if (!crtc_id || !mode.hdisplay || !mode.vdisplay) {
		fprintf(stderr, "DRM-TEST: SKIP (no CRTC or mode)\n");
		if (enc)
			drmModeFreeEncoder(enc);
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		close(fd);
		return 0;
	}

	printf("DRM-TEST: connector=%u crtc=%u mode=%ux%u\n",
	       conn->connector_id, crtc_id, mode.hdisplay, mode.vdisplay);
	fflush(stdout);

	struct dumb_buffer red = { 0 };
	struct dumb_buffer green = { 0 };
	if (create_buffer(fd, mode.hdisplay, mode.vdisplay, 0x00ff0000, &red) ||
	    create_buffer(fd, mode.hdisplay, mode.vdisplay, 0x0000ff00,
			  &green)) {
		destroy_buffer(fd, &red);
		destroy_buffer(fd, &green);
		if (enc)
			drmModeFreeEncoder(enc);
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		close(fd);
		return 1;
	}

	if (drmModeSetCrtc(fd, crtc_id, red.fb_id, 0, 0, &conn->connector_id,
			   1, &mode) != 0) {
		fprintf(stderr, "SETCRTC failed: %s\n", strerror(errno));
		goto out;
	}
	printf("DRM-TEST: RED\n");
	fflush(stdout);
	sleep(8);

	if (drmModePageFlip(fd, crtc_id, green.fb_id, DRM_MODE_PAGE_FLIP_EVENT,
			    (void *)0x1234) != 0) {
		fprintf(stderr, "PAGE_FLIP failed: %s\n", strerror(errno));
		goto out;
	}
	printf("DRM-TEST: PAGE-FLIP-REQUESTED\n");
	fflush(stdout);

	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int poll_ret = poll(&pfd, 1, 5000);
	if (poll_ret <= 0) {
		fprintf(stderr, "poll failed: ret=%d errno=%s\n", poll_ret,
			strerror(errno));
		goto out;
	}

	drmEventContext evctx = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler2 = page_flip_handler,
	};
	if (drmHandleEvent(fd, &evctx) != 0) {
		fprintf(stderr, "drmHandleEvent failed: %s\n", strerror(errno));
		goto out;
	}

	printf("DRM-TEST: GREEN\n");
	fflush(stdout);
	sleep(15);

out:
	destroy_buffer(fd, &red);
	destroy_buffer(fd, &green);
	if (enc)
		drmModeFreeEncoder(enc);
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	drmDropMaster(fd);
	close(fd);

	if (!flip_complete) {
		fprintf(stderr, "DRM-TEST: FAIL (no flip-complete event)\n");
		return 1;
	}
	printf("DRM-TEST: PASS\n");
	return 0;
}
