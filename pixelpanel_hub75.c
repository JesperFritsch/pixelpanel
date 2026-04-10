
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/of.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/of_address.h>

#include "pixelpanel.h"
#include "header_to_pin.h"

#define MAX_BIT_PLANES 8

#define BCM2835_GPIO_BASE  0x20200000  /* Pi 1 */
#define BCM2836_GPIO_BASE  0x3F200000  /* Pi 2, 3 */
#define BCM2711_GPIO_BASE  0xFE200000  /* Pi 4 */
#define BCM2712_GPIO_BASE  0x1F000D0000 /* Pi 5 */

#define GPIO_SET0   0x1C    /* GPSET0 — set pins high */
#define GPIO_CLR0   0x28    /* GPCLR0 — set pins low */
#define GPIO_FSEL0  0x00    /* Function select registers */

static struct fb_info *f_info;
static int gpio_r1, gpio_g1, gpio_b1;
static int gpio_r2, gpio_g2, gpio_b2;
static int gpio_a_addr, gpio_b_addr, gpio_c_addr, gpio_d_addr, gpio_e_addr;
static int gpio_clk, gpio_lat, gpio_oe;
static u32 color_lut[64];
static u32 addr_set_masks[32];
static u32 *gpio_set_masks;
static u32 scan_rows;    /* height / 2 — number of row pairs */
static u32 screen_width;
static void __iomem *gpio_base;
static struct task_struct *refresh_thread;
static u32 addr_mask;


enum color_idx_bits {
    IDX_R1 = 0,
    IDX_G1 = 1,
    IDX_B1 = 2,
    IDX_R2 = 3,
    IDX_G2 = 4,
    IDX_B2 = 5,
};


static void assign_pins(void)
{
    gpio_r1     = header_to_gpio[23];
    gpio_g1     = header_to_gpio[13];
    gpio_b1     = header_to_gpio[26];
    gpio_r2     = header_to_gpio[24];
    gpio_g2     = header_to_gpio[21];
    gpio_b2     = header_to_gpio[19];
    gpio_a_addr = header_to_gpio[15];
    gpio_b_addr = header_to_gpio[16];
    gpio_c_addr = header_to_gpio[18];
    gpio_d_addr = header_to_gpio[22];
    gpio_e_addr = header_to_gpio[10];
    gpio_clk    = header_to_gpio[11];
    gpio_lat    = header_to_gpio[7];
    gpio_oe     = header_to_gpio[12];
    addr_mask = BIT(gpio_a_addr) | BIT(gpio_b_addr) | 
        BIT(gpio_c_addr) | BIT(gpio_d_addr) | BIT(gpio_e_addr);
}


static void build_addr_lut(void)
{
    int i;
    for (i = 0; i < 32; i++) {
        addr_set_masks[i] = 0;
        if (i & 0x01) addr_set_masks[i] |= BIT(gpio_a_addr);
        if (i & 0x02) addr_set_masks[i] |= BIT(gpio_b_addr);
        if (i & 0x04) addr_set_masks[i] |= BIT(gpio_c_addr);
        if (i & 0x08) addr_set_masks[i] |= BIT(gpio_d_addr);
        if (i & 0x10) addr_set_masks[i] |= BIT(gpio_e_addr);
    }
}


static int gpio_map_init(void)
{
    struct device_node *soc;
    struct resource res;

    soc = of_find_node_by_path("/soc/gpio");
    if (soc) {
        of_address_to_resource(soc, 0, &res);
        gpio_base = ioremap(res.start, resource_size(&res));
        of_node_put(soc);
    }
    if (!gpio_base) {
        pr_err("failed to map GPIO registers\n");
        return -ENXIO;
    }
    return 0;
}


static void gpio_map_remove(void)
{
    if (gpio_base)
        iounmap(gpio_base);
}


static inline void gpio_set_bits(u32 mask)
{
    writel(mask, gpio_base + GPIO_SET0);
}


static inline void gpio_clr_bits(u32 mask)
{
    writel(mask, gpio_base + GPIO_CLR0);
}


static void gpio_set_output(int pin)
{
    int reg = pin / 10;
    int shift = (pin % 10) * 3;
    u32 val = readl(gpio_base + GPIO_FSEL0 + reg * 4);
    val &= ~(0x7 << shift);   /* clear the 3 bits */
    val |= (0x1 << shift);    /* 001 = output */
    writel(val, gpio_base + GPIO_FSEL0 + reg * 4);
}


static inline void set_address(int row)
{
    gpio_clr_bits(addr_mask);
    gpio_set_bits(addr_set_masks[row]);
}


static inline void latch_pulse(void)
{
    gpio_set_bits(BIT(gpio_lat));
    gpio_clr_bits(BIT(gpio_lat));
}


static inline void clock_pulse(void)
{
    gpio_set_bits(BIT(gpio_clk));
    gpio_clr_bits(BIT(gpio_clk));
}


static void configure_gpio_outputs(void)
{
    gpio_set_output(gpio_r1);
    gpio_set_output(gpio_g1);
    gpio_set_output(gpio_b1);
    gpio_set_output(gpio_r2);
    gpio_set_output(gpio_g2);
    gpio_set_output(gpio_b2);
    gpio_set_output(gpio_a_addr);
    gpio_set_output(gpio_b_addr);
    gpio_set_output(gpio_c_addr);
    gpio_set_output(gpio_d_addr);
    gpio_set_output(gpio_e_addr);
    gpio_set_output(gpio_clk);
    gpio_set_output(gpio_lat);
    gpio_set_output(gpio_oe);

    /* Start with display off */
    gpio_set_bits(BIT(gpio_oe));
}


/*
    Builds a lookup table to use when setting the GPIO register.
    the table contains all combinations of R1, G1, B1, R2, G2, B2 as 32 bit masks.
*/
static void build_color_set_mask_lut(void)
{
    int i;
    for (i = 0; i < 64; i++) {
        color_lut[i] = 0;
        if (i & BIT(IDX_R1)) color_lut[i] |= BIT(gpio_r1);
        if (i & BIT(IDX_G1)) color_lut[i] |= BIT(gpio_g1);
        if (i & BIT(IDX_B1)) color_lut[i] |= BIT(gpio_b1);
        if (i & BIT(IDX_R2)) color_lut[i] |= BIT(gpio_r2);
        if (i & BIT(IDX_G2)) color_lut[i] |= BIT(gpio_g2);
        if (i & BIT(IDX_B2)) color_lut[i] |= BIT(gpio_b2);
    }
}

/*
    Calculates the index in the look up table for both pixels in the scan
    @top_pxl is the color values for the pixel on the top scan
    @bot_pxl is the color values for the pixel on the bottom scan
    @bit a mask with only the current bit-planes bit set.
    @r_offset is the offset of the red byte in each u32 pixel color
    @g_offset is the offset of the green byte in each u32 pixel color
    @b_offset is the offset of the blue byte in each u32 pixel color
*/
static inline u8 lut_index(u32 top_pxl, u32 bot_pxl, 
    u8 bit, u32 r_offset, u32 g_offset, u32 b_offset)
{
    u8 idx = 0;

    idx |= (((top_pxl >> r_offset) & bit) ? 1 : 0) << IDX_R1;
    idx |= (((top_pxl >> g_offset) & bit) ? 1 : 0) << IDX_G1;
    idx |= (((top_pxl >> b_offset) & bit) ? 1 : 0) << IDX_B1;
    idx |= (((bot_pxl >> r_offset) & bit) ? 1 : 0) << IDX_R2;
    idx |= (((bot_pxl >> g_offset) & bit) ? 1 : 0) << IDX_G2;
    idx |= (((bot_pxl >> b_offset) & bit) ? 1 : 0) << IDX_B2;
    return idx;
}

/*
    compute the set masks from the screen_buffer
*/
static void compute_set_masks(void)
{
    u32 width = f_info->var.xres;
    u32 stride = f_info->fix.line_length / 4;  /* pixels per row */
    u32 r_off = f_info->var.red.offset;
    u32 g_off = f_info->var.green.offset;
    u32 b_off = f_info->var.blue.offset;
    u32 yoff = f_info->var.yoffset;
    u32 xoff = f_info->var.xoffset;
    u32 *pixels = (u32 *)f_info->screen_buffer;
    int plane, row, col;

    for (plane = 0; plane < MAX_BIT_PLANES; plane++) {
        u8 bit = 1 << plane;

        for (row = 0; row < scan_rows; row++) {
            u32 *row_top = &pixels[(yoff + row) * stride + xoff];
            u32 *row_bot = &pixels[(yoff + row + scan_rows) * stride + xoff];

            for (col = 0; col < width; col++) {
                u8 idx = lut_index(row_top[col], row_bot[col],
                                   bit, r_off, g_off, b_off);

                gpio_set_masks[(plane * (scan_rows * width)) + (row * width) + col] = color_lut[idx];
            }
        }
    }
}


static int refresh_fn(void *data)
{
    u32 width = f_info->var.xres;
    u32 data_mask = BIT(gpio_r1) | BIT(gpio_g1) | BIT(gpio_b1) |
                    BIT(gpio_r2) | BIT(gpio_g2) | BIT(gpio_b2);
    int plane, row, col;

    while (!kthread_should_stop()) {

        compute_set_masks();

        for (plane = 0; plane < MAX_BIT_PLANES; plane++) {

            for (row = 0; row < scan_rows; row++) {
                u32 *row_data = &gpio_set_masks[plane * scan_rows * width + row * width];

                /* Clock in data for this row pair while previous row is displayed */
                for (col = 0; col < width; col++) {
                    gpio_clr_bits(data_mask);
                    gpio_set_bits(row_data[col]);
                    clock_pulse();
                }

                /* Disable output while switching rows */
                gpio_set_bits(BIT(gpio_oe));    /* OE high = off */

                /* Latch the clocked-in data */
                latch_pulse();

                /* Set the new row address */
                set_address(row);

                /* Enable output — hold for weighted duration */
                gpio_clr_bits(BIT(gpio_oe));    /* OE low = on */
                ndelay(200 << plane);  /* T, 2T, 4T, 8T... 128T */

            }
        }
    }

    return 0;
}


int pp_renderer_init(struct fb_info *info)
{
    int ret;

    f_info = info;
    assign_pins();

    ret = gpio_map_init();
    if (ret)
        return ret;

    configure_gpio_outputs();
    build_color_set_mask_lut();
    build_addr_lut();

    scan_rows = f_info->var.yres / 2;
    screen_width = f_info->var.xres;
    gpio_set_masks = vmalloc(MAX_BIT_PLANES * scan_rows * screen_width * sizeof(u32));
    if (!gpio_set_masks) {
        gpio_map_remove();
        return -ENOMEM;
    }

    return 0;
}


void pp_renderer_start(void)
{
    refresh_thread = kthread_run(refresh_fn, NULL, "pp_refresh");
    if (IS_ERR(refresh_thread)) {
        pr_err("failed to create refresh thread\n");
        refresh_thread = NULL;
    }
}


void pp_renderer_stop(void)
{
    if (refresh_thread) {
        kthread_stop(refresh_thread);
        refresh_thread = NULL;
    }
    gpio_set_bits(BIT(gpio_oe));  /* turn off display */
}


void pp_renderer_remove(void)
{
    if (gpio_base)
        gpio_set_bits(BIT(gpio_oe));
    gpio_map_remove();
    if (gpio_set_masks)
        vfree(gpio_set_masks);

}
