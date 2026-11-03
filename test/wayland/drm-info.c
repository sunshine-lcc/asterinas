// SPDX-License-Identifier: MPL-2.0
//
// Dump the DRM/KMS topology and capabilities visible to userspace. This is a
// bring-up probe for compositor compatibility, not a test assertion.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_DEVICE "/dev/dri/card0"

int main(void)
{
	int fd = open(DRM_DEVICE, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("DRM-INFO: open failed: %s\n", strerror(errno));
		return 1;
	}

	printf("DRM-INFO: fd=%d master=%d\n", fd, drmIsMaster(fd));
	uint64_t cap = 0;
	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) == 0)
		printf("DRM-INFO: DUMB_BUFFER=%llu\n", (unsigned long long)cap);
	if (drmGetCap(fd, DRM_CAP_PRIME, &cap) == 0)
		printf("DRM-INFO: PRIME=%llu\n", (unsigned long long)cap);
	if (drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0)
		printf("DRM-INFO: ADDFB2_MODIFIERS=%llu\n",
		       (unsigned long long)cap);

	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		printf("DRM-INFO: GETRESOURCES failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("DRM-INFO: connectors=%d crtcs=%d encoders=%d fbs=%d\n",
	       res->count_connectors, res->count_crtcs, res->count_encoders,
	       res->count_fbs);

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *conn =
			drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			printf("DRM-INFO: connector %u get failed: %s\n",
			       res->connectors[i], strerror(errno));
			continue;
		}
		printf("DRM-INFO: connector id=%u type=%u/%u connection=%u "
		       "modes=%d encoder=%u mm=%dx%d\n",
		       conn->connector_id, conn->connector_type,
		       conn->connector_type_id, conn->connection,
		       conn->count_modes, conn->encoder_id, conn->mmWidth,
		       conn->mmHeight);
		for (int m = 0; m < conn->count_modes && m < 8; m++) {
			drmModeModeInfo *mode = &conn->modes[m];
			printf("DRM-INFO:   mode %dx%d clock=%u flags=0x%x "
			       "type=0x%x name=%s\n",
			       mode->hdisplay, mode->vdisplay, mode->clock,
			       mode->flags, mode->type, mode->name);
		}
		drmModeFreeConnector(conn);
	}

	for (int i = 0; i < res->count_encoders; i++) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, res->encoders[i]);
		if (!enc)
			continue;
		printf("DRM-INFO: encoder id=%u type=%u crtc=%u "
		       "possible_crtcs=0x%x possible_clones=0x%x\n",
		       enc->encoder_id, enc->encoder_type, enc->crtc_id,
		       enc->possible_crtcs, enc->possible_clones);
		drmModeFreeEncoder(enc);
	}

	for (int i = 0; i < res->count_crtcs; i++) {
		drmModeCrtc *crtc = drmModeGetCrtc(fd, res->crtcs[i]);
		if (!crtc)
			continue;
		printf("DRM-INFO: crtc id=%u buffer=%u mode_valid=%d "
		       "mode=%dx%d\n",
		       crtc->crtc_id, crtc->buffer_id, crtc->mode_valid,
		       crtc->mode.hdisplay, crtc->mode.vdisplay);
		drmModeFreeCrtc(crtc);
	}

	drmModeFreeResources(res);
	close(fd);
	return 0;
}
