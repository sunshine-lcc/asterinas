// SPDX-License-Identifier: MPL-2.0
//
// Draw four color bars directly through /dev/fb0. This isolates the
// framebuffer/scanout path from Xorg and Wayland before testing a compositor.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define FB_DEVICE "/dev/fb0"

static uint32_t bar_color(unsigned int index, unsigned int bpp)
{
	static const uint32_t colors32[] = {
		0x00ff0000, /* red */
		0x0000ff00, /* green */
		0x000000ff, /* blue */
		0x00ffffff, /* white */
	};
	static const uint16_t colors16[] = {
		0xf800, 0x07e0, 0x001f, 0xffff,
	};

	if (bpp == 16)
		return colors16[index % 4];
	return colors32[index % 4];
}

static void draw_bars(uint8_t *base, const struct fb_fix_screeninfo *fix,
		      const struct fb_var_screeninfo *var)
{
	unsigned int bytes_per_pixel = var->bits_per_pixel / 8;
	for (unsigned int y = 0; y < var->yres; y++) {
		uint8_t *row = base + y * fix->line_length;
		for (unsigned int x = 0; x < var->xres; x++) {
			unsigned int bar = (x * 4) / var->xres;
			uint32_t color = bar_color(bar, var->bits_per_pixel);
			uint8_t *pixel = row + x * bytes_per_pixel;
			if (bytes_per_pixel == 4) {
				memcpy(pixel, &color, 4);
			} else if (bytes_per_pixel == 2) {
				uint16_t c = (uint16_t)color;
				memcpy(pixel, &c, 2);
			} else if (bytes_per_pixel == 3) {
				pixel[0] = color & 0xff;
				pixel[1] = (color >> 8) & 0xff;
				pixel[2] = (color >> 16) & 0xff;
			}
		}
	}
}

int main(void)
{
	int fd = open(FB_DEVICE, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "FB-TEST: SKIP (%s: %s)\n", FB_DEVICE,
			strerror(errno));
		return 0;
	}

	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0) {
		fprintf(stderr, "FB-TEST: FAIL (ioctl: %s)\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("FB-TEST: %ux%u %ubpp line_length=%u smem_len=%u\n", var.xres,
	       var.yres, var.bits_per_pixel, fix.line_length, fix.smem_len);
	fflush(stdout);

	uint8_t *base = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE,
			     MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		fprintf(stderr, "FB-TEST: FAIL (mmap: %s)\n", strerror(errno));
		close(fd);
		return 1;
	}

	draw_bars(base, &fix, &var);
	msync(base, fix.smem_len, MS_SYNC);

	printf("FB-TEST: READY\n");
	fflush(stdout);
	sleep(20);

	munmap(base, fix.smem_len);
	close(fd);
	printf("FB-TEST: DONE\n");
	return 0;
}
