#define _GNU_SOURCE
#include "config.h"
#include "display.h"
#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* DRM_IOCTL_MODE_DIRTYFB – some kernel headers omit it; define defensively. */
#ifndef DRM_IOCTL_MODE_DIRTYFB
#define DRM_IOCTL_MODE_DIRTYFB \
    DRM_IOWR(0xB1, struct drm_mode_fb_dirty_cmd)
#endif

/* DRM_IOCTL_MODE_DESTROY_DUMB – likewise. */
#ifndef DRM_IOCTL_MODE_DESTROY_DUMB
struct drm_mode_destroy_dumb { __u32 handle; };
#define DRM_IOCTL_MODE_DESTROY_DUMB \
    DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#endif

/*  helpers  */

/* Release GEM dumb buffer: unmap -> RMFB -> DESTROY_DUMB.
 * Must be called before close(d->fd) for a clean teardown.
 * Does NOT call close(d->fd) – caller owns that. */
static void drm_buf_free(DisplayDev *d) {
  if (d->buf.map && d->buf.map != MAP_FAILED) {
    munmap(d->buf.map, d->buf.size);
    d->buf.map = NULL;
  }
  if (d->fd < 0)
    return;
  if (d->buf.fb_id) {
    ioctl(d->fd, DRM_IOCTL_MODE_RMFB, &d->buf.fb_id);
    d->buf.fb_id = 0;
  }
  if (d->buf.handle) {
    struct drm_mode_destroy_dumb dd = {.handle = d->buf.handle};
    ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    d->buf.handle = 0;
  }
}

/*  public  */

bool drm_init_dev(DisplayDev *d) {
  d->fd = -1;

  if ((d->fd = open(DRM_DEVICE, O_RDWR | O_CLOEXEC)) < 0)
    return false;

  /* Enable universal planes so we see all plane types. */
  {
    struct drm_set_client_cap cp = {
        .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1};
    ioctl(d->fd, DRM_IOCTL_SET_CLIENT_CAP, &cp);
  }

  /*  Step 1: enumerate resources  */
  struct drm_mode_card_res res = {0};
  if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
    goto fail_fd;

  if (res.count_connectors == 0 || res.count_crtcs == 0)
    goto fail_fd;

  uint32_t *crtcs  = calloc(res.count_crtcs,      sizeof(uint32_t));
  uint32_t *conns  = calloc(res.count_connectors,  sizeof(uint32_t));
  uint32_t *encs   = calloc(res.count_encoders,    sizeof(uint32_t));
  if (!crtcs || !conns || !encs) {
    free(crtcs); free(conns); free(encs);
    goto fail_fd;
  }
  res.crtc_id_ptr       = (uintptr_t)crtcs;
  res.connector_id_ptr  = (uintptr_t)conns;
  res.encoder_id_ptr    = (uintptr_t)encs;
  if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
    free(crtcs); free(conns); free(encs);
    goto fail_fd;
  }

  /*  Step 2: pick best connected connector  *
   * Android/embedded: prefer DSI (4) > eDP (3) > LVDS (2) > anything (1).
   * Desktop: fall through to first connected connector.
   * DRM_CONN_ID config override wins unconditionally.              */
  uint32_t best_conn = DRM_CONN_ID;
  uint32_t used_crtc = DRM_CRTC_ID;
  if (!best_conn) {
    int best_score = -1;
    for (uint32_t i = 0; i < res.count_connectors; i++) {
      struct drm_mode_get_connector gc = {.connector_id = conns[i]};
      if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0)
        continue;
      if (gc.connection != 1) /* not connected */
        continue;
      int score = (gc.connector_type == DRM_MODE_CONNECTOR_DSI)  ? 4
                : (gc.connector_type == DRM_MODE_CONNECTOR_eDP)  ? 3
                : (gc.connector_type == DRM_MODE_CONNECTOR_LVDS) ? 2
                                                                  : 1;
      if (score > best_score) { best_score = score; best_conn = conns[i]; }
    }
  }
  if (!best_conn) { free(crtcs); free(conns); free(encs); goto fail_fd; }

  /*  Step 3: get connector modes + active encoder  */
  struct drm_mode_get_connector con = {.connector_id = best_conn};
  if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &con) < 0 ||
      con.count_modes == 0) {
    free(crtcs); free(conns); free(encs);
    goto fail_fd;
  }
  uint32_t saved_enc_id = con.encoder_id;

  struct drm_mode_modeinfo *modes = calloc(con.count_modes, sizeof(*modes));
  if (!modes) { free(crtcs); free(conns); free(encs); goto fail_fd; }
  con.modes_ptr       = (uintptr_t)modes;
  con.count_props     = 0;
  con.count_encoders  = 0;
  if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &con) < 0) {
    free(modes); free(crtcs); free(conns); free(encs);
    goto fail_fd;
  }

  /* Pick PREFERRED mode, fall back to first (largest) mode. */
  int midx = 0;
  for (uint32_t i = 0; i < con.count_modes; i++) {
    if (modes[i].type & DRM_MODE_TYPE_PREFERRED) { midx = (int)i; break; }
  }
  d->width  = (int)modes[midx].hdisplay;
  d->height = (int)modes[midx].vdisplay;

  /*  Step 4: resolve CRTC  */
  if (!used_crtc) {
    if (saved_enc_id) {
      struct drm_mode_get_encoder ge = {.encoder_id = saved_enc_id};
      if (ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &ge) == 0 && ge.crtc_id)
        used_crtc = ge.crtc_id;
    }
    /* If encoder had no CRTC, scan crtc_id bitmask from encoder. */
    if (!used_crtc && saved_enc_id) {
      struct drm_mode_get_encoder ge = {.encoder_id = saved_enc_id};
      ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &ge);
      for (uint32_t i = 0; i < res.count_crtcs; i++) {
        if (ge.possible_crtcs & (1u << i)) { used_crtc = crtcs[i]; break; }
      }
    }
    if (!used_crtc && res.count_crtcs > 0)
      used_crtc = crtcs[0];
  }

  free(crtcs); free(conns); free(encs);

  /* Save for display_free / VT re-entry. */
  d->buf.conn_id = best_conn;
  d->buf.crtc_id = used_crtc;

  /*  Step 5: allocate dumb buffer  */
  struct drm_mode_create_dumb cr = {
      .width  = (uint32_t)d->width,
      .height = (uint32_t)d->height,
      .bpp    = 32,
  };
  if (ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr) < 0) {
    free(modes); goto fail_fd;
  }
  d->buf.handle = cr.handle;
  d->buf.pitch  = cr.pitch;
  d->buf.size   = (uint32_t)cr.size;

  /*  Step 6: add framebuffer  */
  struct drm_mode_fb_cmd fb = {
      .width  = (uint32_t)d->width,
      .height = (uint32_t)d->height,
      .pitch  = d->buf.pitch,
      .bpp    = 32,
      .depth  = 24,
      .handle = d->buf.handle,
  };
  if (ioctl(d->fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
    /* Destroy dumb buffer before giving up. */
    struct drm_mode_destroy_dumb dd = {.handle = d->buf.handle};
    ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    d->buf.handle = 0;
    free(modes); goto fail_fd;
  }
  d->buf.fb_id = fb.fb_id;

  /*  Step 7: mmap dumb buffer  */
  struct drm_mode_map_dumb mq = {.handle = d->buf.handle};
  if (ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mq) < 0) {
    drm_buf_free(d); free(modes); goto fail_fd;
  }
  d->buf.map = mmap(NULL, d->buf.size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, d->fd, (off_t)mq.offset);
  if (d->buf.map == MAP_FAILED) {
    d->buf.map = NULL;
    drm_buf_free(d); free(modes); goto fail_fd;
  }
  memset(d->buf.map, 0, d->buf.size);

  /*  Step 8: program CRTC (needs master)  */
  ioctl(d->fd, DRM_IOCTL_SET_MASTER, 0);
  struct drm_mode_crtc cc = {
      .crtc_id              = used_crtc,
      .fb_id                = d->buf.fb_id,
      .set_connectors_ptr   = (uintptr_t)&best_conn,
      .count_connectors     = 1,
      .mode_valid           = 1,
      .mode                 = modes[midx],
  };
  if (ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &cc) < 0) {
    ioctl(d->fd, DRM_IOCTL_DROP_MASTER, 0);
    drm_buf_free(d); free(modes); goto fail_fd;
  }
  /* Drop master immediately; vt_acquire() will re-grab on each VT entry. */
  ioctl(d->fd, DRM_IOCTL_DROP_MASTER, 0);

  free(modes);
  d->is_drm = true;
  return true;

fail_fd:
  if (d->fd >= 0) { close(d->fd); d->fd = -1; }
  return false;
}

/* Public cleanup: munmap -> RMFB -> DESTROY_DUMB -> close.
 * Call vt_restore() BEFORE this so SETCRTC isn't racing. */
void drm_free_dev(DisplayDev *d) {
  drm_buf_free(d);
  if (d->fd >= 0) { close(d->fd); d->fd = -1; }
}

/* Re-program CRTC with our FB – called from vt_acquire after SET_MASTER. */
void drm_reprogram_crtc(DisplayDev *d) {
  if (d->fd < 0 || !d->buf.fb_id || !d->buf.crtc_id)
    return;
  uint32_t conn = d->buf.conn_id;
  struct drm_mode_crtc cc = {
      .crtc_id            = d->buf.crtc_id,
      .fb_id              = d->buf.fb_id,
      .set_connectors_ptr = (uintptr_t)&conn,
      .count_connectors   = 1,
      .mode_valid         = 0, /* keep current mode */
  };
  ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &cc);
}

/* Notify driver that dirty pixels need scanning out.
 * Best-effort: many Android/embedded DRM drivers scan out continuously
 * from the dumb CMA buffer without needing this, but desktop drivers
 * (i915 shadow FB, virtual) benefit from it.                         */
void drm_kick(DisplayDev *d) {
  struct drm_mode_fb_dirty_cmd dy = {.fb_id = d->buf.fb_id};
  ioctl(d->fd, DRM_IOCTL_MODE_DIRTYFB, &dy); /* ignore ENOSYS / EINVAL */
}

void drm_drop_master(DisplayDev *d) {
  if (d->fd >= 0) ioctl(d->fd, DRM_IOCTL_DROP_MASTER, 0);
}

void drm_set_master(DisplayDev *d) {
  if (d->fd >= 0) ioctl(d->fd, DRM_IOCTL_SET_MASTER, 0);
}
