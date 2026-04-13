/* pixelpanel.h */
#ifndef PIXELPANEL_H
#define PIXELPANEL_H

#include <linux/types.h>
#include <linux/fb.h>

int pp_renderer_init(struct fb_info *info);
void pp_renderer_start(void);
void pp_renderer_stop(void);
void pp_renderer_remove(void);
/* Vsync support */
void pp_signal_vsync(void);
int pp_wait_vsync(void);

#endif