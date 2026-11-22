/* Material Design Icons by name, from the blob tools/make_icons.py builds (main/mdi_icons.bin) */
#pragma once
#include <stdint.h>

#define ICON_SIZE 24   /* pixels; must match tools/make_icons.py */

/* Maps the icon blob from its flash partition; call once at boot */
void icons_init(void);

/* No icons from now on (the OTA update is about to rewrite the partition) */
void icons_disable(void);

/* The 4 bpp alpha bitmap (ICON_SIZE x ICON_SIZE, LV_IMG_CF_ALPHA_4BIT) of "mdi:name" or
 * "name"; NULL when unknown */
const uint8_t *icon_bitmap(const char *name);

/* An icon resampled to n x n (n even) into dst (n*n/2 bytes), same 4 bpp alpha format */
void icon_scale(const uint8_t *src, uint8_t *dst, int n);
