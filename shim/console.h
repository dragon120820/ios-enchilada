#pragma once
#include <stdint.h>

#ifndef FB_BASE
#define FB_BASE		0x9d400000UL	/* OnePlus 6T and 6: cont_splash_region (36 MiB) */
#endif
#define FB_W		1080
#ifndef FB_H
#define FB_H		2340		/* 6T panel; the OnePlus 6 is 2280 (FB_H=2280 shim/build.sh) */
#endif
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
/* One line of plain text at the left margin, truncated to the panel width. */
void con_line(const char *s);
