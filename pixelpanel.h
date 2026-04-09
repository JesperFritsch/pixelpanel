#ifndef PIXELPANEL_H
#define PIXELPANEL_H

#include <linux/types.h>

struct pp_renderer_ops {
    int (*init)(struct pp_renderer_ops *ops);
    void (*remove)(struct pp_renderer_ops *ops);
    void (*render_frame)(struct pp_renderer_ops *ops,
                         const void *buffer,
                         unsigned int width,
                         unsigned int height,
                         unsigned int stride);
};

#endif