
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "pixelpanel.h"

static struct fb_info *f_info;

int pp_renderer_init(struct fb_info *info)
{
    f_info = info;
    return 0;
}

void pp_renderer_start(void)
{

}

void pp_renderer_stop(void)
{

}

void pp_renderer_remove(void)
{

}