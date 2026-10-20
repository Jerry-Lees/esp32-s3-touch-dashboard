#pragma once
/*
 * PNG and GIF readers for the photo frame (photo_frame.c does JPEG and
 * BMP). Both read the file a little at a time and shrink the picture
 * while reading (averaging blocks of 2x2, 4x4, ... pixels), so a large
 * picture never has to fit in memory at full size. Every size, length and
 * index in the file is checked, so a damaged file gives an error, not a
 * crash. Worker task only (no LVGL).
 *
 * PNG: all colour types and bit depths, transparency (shown on black).
 *      Interlaced PNGs aren't supported. Inflate is the ESP32-S3 ROM's
 *      miniz (tinfl).
 * GIF: the first frame (GIF87a and GIF89a, interlaced too); transparent
 *      pixels are black.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint16_t *px;                  /* RGB565, PSRAM; caller frees */
    int       w, h;
} pic_img_t;

/* Asked once the picture's size is known: returns s, the picture is
 * shrunk by 2^s (0 = full size). */
typedef int (*pic_scale_fn)(int w, int h, const void *arg);

/* On failure, err (64 bytes) says why. */
bool pic_load_png(FILE *f, pic_img_t *img, pic_scale_fn scale, const void *arg, char *err);
bool pic_load_gif(FILE *f, pic_img_t *img, pic_scale_fn scale, const void *arg, char *err);
