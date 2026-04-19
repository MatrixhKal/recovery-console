#define _GNU_SOURCE
#include "config.h"
#include "display.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Attempt to unblank fb via sysfs; non-fatal if unavailable. */
static void sysfs_unblank(const char *path) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return;
  (void)write(fd, "0\n", 2);
  close(fd);
}

bool fbdev_init(DisplayDev *d) {
  d->fd = open(FB_DEVICE, O_RDWR | O_CLOEXEC);
  if (d->fd < 0)
    d->fd = open(FB_DEVICE_ALT, O_RDWR | O_CLOEXEC);
  if (d->fd < 0)
    return false;

  /* Proactively unblank via sysfs – some Android kernels ignore FBIOBLANK. */
  sysfs_unblank("/sys/class/graphics/fb0/blank");
  sysfs_unblank("/sys/class/graphics/fb1/blank");

  struct fb_var_screeninfo vi;
  struct fb_fix_screeninfo fi;
  if (ioctl(d->fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
      ioctl(d->fd, FBIOGET_FSCREENINFO, &fi) < 0) {
    close(d->fd);
    d->fd = -1;
    return false;
  }

  /* Ensure screen is on before mapping. */
  ioctl(d->fd, FBIOBLANK, FB_BLANK_UNBLANK);

  d->width      = (int)vi.xres;
  d->height     = (int)vi.yres;
  d->buf.pitch  = fi.line_length;
  d->buf.size   = fi.smem_len;
  d->buf.conn_id = 0;
  d->buf.crtc_id = 0;

  /* Guard: zero-pitch or zero-length means driver is broken. */
  if (d->buf.pitch == 0 || d->buf.size == 0) {
    /* Compute a sane fallback. */
    if (d->buf.pitch == 0)
      d->buf.pitch = (uint32_t)(d->width * 4);
    if (d->buf.size == 0)
      d->buf.size  = d->buf.pitch * (uint32_t)d->height;
  }

  d->buf.map = mmap(NULL, d->buf.size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, d->fd, 0);
  if (d->buf.map == MAP_FAILED) {
    close(d->fd);
    d->fd = -1;
    return false;
  }

  d->is_drm = false;
  memset(d->buf.map, 0, d->buf.size);
  return true;
}

/* Hardware blank/unblank via FBIOBLANK ioctl (preferred over sysfs). */
void fbdev_blank(DisplayDev *d, bool blank) {
  if (d->fd < 0) return;
  ioctl(d->fd, FBIOBLANK,
        blank ? (int)FB_BLANK_POWERDOWN : (int)FB_BLANK_UNBLANK);
}

/* Pan to offset 0 – flushes the rendered frame on double-buffered devices;
 * a no-op on single-buffered hardware (writes are directly visible).   */
void fbdev_kick(DisplayDev *d) {
  if (d->fd < 0 || d->buf.pitch == 0) return;
  struct fb_var_screeninfo vi;
  if (ioctl(d->fd, FBIOGET_VSCREENINFO, &vi) == 0) {
    vi.xoffset = 0;
    vi.yoffset = 0;
    ioctl(d->fd, FBIOPAN_DISPLAY, &vi);
  }
}
