#include <linux/init.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fb.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/of.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jesper Fritsch");
MODULE_DESCRIPTION("Framebuffer driver mainly for LED displays (HUB75, ...)");

#define DEVICE_NAME "pixelpanel"
uint width = 0;
uint height = 0;
uint pp_renderer_type = 0;

module_param(width, uint, 0);
module_param(height, uint, 0);
MODULE_PARM_DESC(width, "Display width in pixels");
MODULE_PARM_DESC(height, "Display height in pixels");

static void *videomemory;
static u_long videomemorysize;

module_param(videomemorysize, ulong, 0);
MODULE_PARM_DESC(videomemorysize, "RAM available to frame buffer (in bytes)");


static struct fb_fix_screeninfo pp_fix_si = {
    .id = DEVICE_NAME,
    .type = FB_TYPE_PACKED_PIXELS,
    .accel = FB_ACCEL_NONE
};


static struct fb_var_screeninfo pp_var_si = {
    .bits_per_pixel     = 32,
    .red                = { .offset = 16, .length = 8 },
    .green              = { .offset = 8,  .length = 8 },
    .blue               = { .offset = 0,  .length = 8 },
    .transp             = { .offset = 24, .length = 8 }
};


static u_long get_line_length(int xres_virtual, int bpp)
{
    u_long length;
    length = xres_virtual * bpp;
    length = (length + 31) & ~31;
    length >>= 3;
    return length;
}


static int pp_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
                        u_int transp, struct fb_info *info)
{
    if (regno >= 16)
        return -EINVAL;

    if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
        u32 v;

        red   >>= 16 - info->var.red.length;
        green >>= 16 - info->var.green.length;
        blue  >>= 16 - info->var.blue.length;
        transp >>= 16 - info->var.transp.length;

        v = (red   << info->var.red.offset)   |
            (green << info->var.green.offset)  |
            (blue  << info->var.blue.offset)   |
            (transp << info->var.transp.offset);

        ((u32 *)info->pseudo_palette)[regno] = v;
    }

    return 0;
}


static int pp_check_var(struct fb_var_screeninfo *var,
			 struct fb_info *info)
{
	u_long line_length;

	/*
	 *  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
	 *  as FB_VMODE_SMOOTH_XPAN is only used internally
	 */

	if (var->vmode & FB_VMODE_CONUPDATE) {
		var->vmode |= FB_VMODE_YWRAP;
		var->xoffset = info->var.xoffset;
		var->yoffset = info->var.yoffset;
	}

	/*
	 *  Some very basic checks
	 */
	if (!var->xres)
		var->xres = 1;
	if (!var->yres)
		var->yres = 1;
	if (var->xres > var->xres_virtual)
		var->xres_virtual = var->xres;
	if (var->yres > var->yres_virtual)
		var->yres_virtual = var->yres;
	if (var->bits_per_pixel != 32)
        return -EINVAL;

    var->red    = (struct fb_bitfield){ .offset = 16, .length = 8 };
    var->green  = (struct fb_bitfield){ .offset = 8,  .length = 8 };
    var->blue   = (struct fb_bitfield){ .offset = 0,  .length = 8 };
    var->transp = (struct fb_bitfield){ .offset = 24, .length = 8 };

	if (var->xres_virtual < var->xoffset + var->xres)
		var->xres_virtual = var->xoffset + var->xres;
	if (var->yres_virtual < var->yoffset + var->yres)
		var->yres_virtual = var->yoffset + var->yres;

	/*
	 *  Memory limit
	 */
	line_length =
	    get_line_length(var->xres_virtual, var->bits_per_pixel);
	if (line_length * var->yres_virtual > videomemorysize)
		return -ENOMEM;

	
	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;
	var->transp.msb_right = 0;

	return 0;
}


static int pp_set_par(struct fb_info *info)
{
    info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.line_length = get_line_length(info->var.xres_virtual,
						info->var.bits_per_pixel);
	return 0;
}


static int pp_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
    vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);

    return remap_vmalloc_range(vma, (void *)info->fix.smem_start, vma->vm_pgoff);
}


static const struct fb_ops pp_ops = {
	.owner		= THIS_MODULE,
	__FB_DEFAULT_SYSMEM_OPS_RDWR,
	.fb_check_var	= pp_check_var,
	.fb_set_par	= pp_set_par,
	.fb_setcolreg	= pp_setcolreg,
	__FB_DEFAULT_SYSMEM_OPS_DRAW,
	.fb_mmap	= pp_mmap,
};



static int pp_probe(struct platform_device *dev)
{
	struct fb_info *info;
    unsigned int dt_width = 0, dt_height = 0;
	unsigned int size;
	int retval = -ENOMEM;


    /* Read Device Tree values */
    if (dev->dev.of_node) {
        of_property_read_u32(dev->dev.of_node, "width", &dt_width);
        of_property_read_u32(dev->dev.of_node, "height", &dt_height);
    }

    /* Only use DT values if module params are still at defaults */
    if (width == 0 && dt_width)
        width = dt_width;
    if (height == 0 && dt_height)
        height = dt_height;

    if (!(width && height))
    {
        pr_err("%s: width and height must be set\n", DEVICE_NAME);
        return -EINVAL;
    }

    pr_info("pixelpanel: probe, %ux%u @ %dbpp\n", width, height, 32);

    videomemorysize = width * height * 4;
    size = PAGE_ALIGN(videomemorysize);

	/*
	 * For real video cards we use ioremap.
	 */
	if (!(videomemory = vmalloc_32_user(size)))
		return retval;

    pr_info("pixelpanel: buffer at %p, %lu bytes\n", videomemory, videomemorysize);

	info = framebuffer_alloc(sizeof(u32) * 16, &dev->dev);
	if (!info)
		goto err;



    pp_fix_si.smem_start = (unsigned long) videomemory;
    pp_fix_si.smem_len = videomemorysize;

    pp_var_si.xres = width;
    pp_var_si.yres = height;
    pp_var_si.xres_virtual = width;
    pp_var_si.yres_virtual = height;

	info->flags = FBINFO_VIRTFB;
	info->screen_buffer = videomemory;
	info->fbops = &pp_ops;
    info->var = pp_var_si;
	info->fix = pp_fix_si;
	info->pseudo_palette = info->par;
	info->par = NULL;

	retval = register_framebuffer(info);
	if (retval < 0)
		goto err1;

    pr_info("pixelpanel: registered /dev/fb%d\n", info->node);

	platform_set_drvdata(dev, info);

	pp_set_par(info);

	fb_info(info, "pixelpanel device, using %ldK of video memory\n",
		videomemorysize >> 10);
	return 0;
err1:
	framebuffer_release(info);
err:
	vfree(videomemory);
	return retval;
}


#if LINUX_VERSION_CODE > KERNEL_VERSION(6, 9, 0)
static void pp_remove(struct platform_device *dev)
#else
static int pp_remove(struct platform_device *dev)
#endif
{
    struct fb_info *info = platform_get_drvdata(dev);

    if (info) {
        unregister_framebuffer(info);
        vfree(videomemory);
        framebuffer_release(info);
    }
#if LINUX_VERSION_CODE <= KERNEL_VERSION(6, 9, 0)
    return 0;
#endif
}


static struct platform_driver pp_driver = {
	.probe	= pp_probe,
	.remove = pp_remove,
	.driver = {
		.name	= DEVICE_NAME,
	},
};

static struct platform_device *pp_device;


static int __init pp_init(void)
{
	int ret = 0;

	ret = platform_driver_register(&pp_driver);

	if (!ret) {
		pp_device = platform_device_alloc(DEVICE_NAME, 0);

		if (pp_device)
			ret = platform_device_add(pp_device);
		else
			ret = -ENOMEM;

		if (ret) {
			platform_device_put(pp_device);
			platform_driver_unregister(&pp_driver);
		}
	}

	return ret;
}


static void __exit pp_exit(void)
{
    platform_device_unregister(pp_device);
    platform_driver_unregister(&pp_driver);
}

module_init(pp_init);
module_exit(pp_exit);