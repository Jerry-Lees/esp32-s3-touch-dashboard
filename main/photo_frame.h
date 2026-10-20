#pragma once
/*
 * Photo slideshow engine, shared by the Photos app and the screensaver.
 * Shows the JPEG, PNG, GIF and BMP files in dashboard/pictures/ (and subfolders) on
 * the SD card, each scaled to the view, cross-fading every few seconds.
 *
 * Decoding runs in a background task (TJpgDec from LVGL, with its 1/2-1/8
 * downscaling, so 12+ MP phone photos fit in memory; EXIF rotation is
 * honoured). One view at a time. LVGL task only.
 */
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    PF_LOADING,     /* scanning or decoding the first picture */
    PF_SHOWING,
    PF_EMPTY,       /* card mounted, but no pictures */
    PF_NO_CARD,
} pf_state_t;

/* Creates the slideshow in parent, sized w x h (black background).
 * Replaces any slideshow that's already open. */
lv_obj_t *photo_frame_open(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);

/* Stops it (call before deleting its parent). Safe if none is open. */
void photo_frame_close(void);

void photo_frame_step(int dir);            /* +1 next, -1 previous, now */
void photo_frame_set_paused(bool paused);
/* Deletes the picture on screen from the SD card and moves on to the next.
 * Returns false if there's nothing to delete. */
bool photo_frame_delete_current(void);
bool photo_frame_paused(void);
pf_state_t photo_frame_state(void);
/* Current picture's file name ("" if none) and "n / total". */
const char *photo_frame_name(void);
void photo_frame_position(int *n, int *total);

/* Options, saved in settings (applied from the next picture). */
int  photo_frame_interval_s(void);         /* seconds per picture */
void photo_frame_cycle_interval(void);     /* 5 s -> 10 s -> 30 s -> 1 min -> 5 min */
bool photo_frame_shuffle(void);
void photo_frame_set_shuffle(bool on);
bool photo_frame_fill(void);               /* true = fill the screen (crop), false = fit */
void photo_frame_set_fill(bool on);

/* For other apps. True if the name ends in .jpg/.jpeg/.png/.gif/.bmp. */
bool photo_is_picture(const char *name);
/* Decodes one picture file into buf (w x h RGB565, w * h <= 800 * 480),
 * scaled to fit (or fill), rotated per EXIF. Slow: never in the LVGL task.
 * On failure err (64 bytes) says why. */
bool photo_decode_file(const char *path, uint16_t *buf, int w, int h, bool fill, char *err);
/* Same for a picture in memory (type found from its first bytes), always
 * fitted. The picture's size inside buf (centred) goes to out_w / out_h. */
bool photo_decode_mem(const void *data, size_t len, uint16_t *buf, int w, int h,
                      char *err, int *out_w, int *out_h);
