
/* pixelpanel_hub75.c */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/of.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/of_address.h>
#include <linux/sched/isolation.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>

#include "pixelpanel.h"
#include "header_to_pin.h"
#include "pixelpanel_hub75.h"

#define PP_REFRESH_CPU 3
#define MAX_BIT_PLANES 8

#define GPIO_SET0   0x1C    /* GPSET0 — set pins high */
#define GPIO_CLR0   0x28    /* GPCLR0 — set pins low */
#define GPIO_FSEL0  0x00    /* Function select registers */

#define PWM_CTL   0x00
#define PWM_STA   0x04
#define PWM_RNG1  0x10

/* Clock manager PWM registers — offset from clock manager base */
#define CM_PWMCTL 0xA0
#define CM_PWMDIV 0xA4
#define CM_PASSWORD 0x5A000000

#define PWM_FIFO  0x18

#define PWM_CTL_PWEN1  (1 << 0)
#define PWM_CTL_POLA1  (1 << 4)
#define PWM_CTL_USEF1  (1 << 5)
#define PWM_CTL_CLRF1  (1 << 6)
#define PWM_STA_EMPT1  (1 << 1)

/* GPIO ALT function codes */
#define GPIO_FSEL_ALT5  2

#define PWM_CLK_DIVIDER 2
#define PLLD_FREQ_MHZ   500
#define NS_PER_TICK      (1000 / (PLLD_FREQ_MHZ / PWM_CLK_DIVIDER))  /* = 4 */

static struct fb_info *f_info;
static int gpio_r1, gpio_g1, gpio_b1;
static int gpio_r2, gpio_g2, gpio_b2;
static int gpio_a_addr, gpio_b_addr, gpio_c_addr, gpio_d_addr, gpio_e_addr;
static int gpio_clk, gpio_lat, gpio_oe;
static volatile u32 *gpio_set_reg;
static volatile u32 *gpio_clr_reg;
static volatile u32 *pwm_ctl_reg;
static volatile u32 *pwm_sta_reg;
static volatile u32 *pwm_rng1_reg;
static volatile u32 *pwm_fifo_reg;


static u32 color_lut[64];
static u32 addr_set_masks[32];
static u32 scan_rows;    /* height / 2 — number of row pairs */
static u32 addr_mask;

static void __iomem *gpio_base;
static void __iomem *pwm_base;
static void __iomem *clk_base;

static u32 *front_masks_buf;
static u32 *back_masks_buf;
static struct task_struct *refresh_thread;
static struct task_struct *compute_thread;

static uint gamma_preset = 2;  /* default: 2.2 */
static uint brightness = 50;
static uint refresh_rate = 120;
static uint base_ticks = 0;

static int param_set_brightness(const char *val, const struct kernel_param *kp)
{
    uint tmp;
    int ret = kstrtouint(val, 0, &tmp);
    if (ret)
        return ret;
    if (tmp > 100)
        return -EINVAL;
    brightness = tmp;
    return 0;
}

static int param_set_refresh_rate(const char *val, const struct kernel_param *kp)
{
    uint tmp;
    int ret = kstrtouint(val, 0, &tmp);
    if (ret)
        return ret;
    if (tmp < 1)
        return -EINVAL;
    refresh_rate = tmp;
    return 0;
}

static const struct kernel_param_ops brightness_ops = {
    .set = param_set_brightness,
    .get = param_get_uint,
};

static const struct kernel_param_ops refresh_rate_ops = {
    .set = param_set_refresh_rate,
    .get = param_get_uint,
};

module_param_cb(brightness, &brightness_ops, &brightness, 0644);
MODULE_PARM_DESC(brightness, "Brightness 0-100 (default 50)");

module_param_cb(refresh_rate, &refresh_rate_ops, &refresh_rate, 0644);
MODULE_PARM_DESC(refresh_rate, "Target refresh rate in Hz (default 120)");

module_param(gamma_preset, uint, 0644);
MODULE_PARM_DESC(gamma_preset, "Gamma preset: 0=off 1=1.8 2=2.2 3=2.5 4=2.8");

module_param(base_ticks, uint, 0644);
MODULE_PARM_DESC(base_ticks, "PWM base duration for LSB bit plane in clock ticks, higher = brighter (default 0=auto)");


static const struct of_device_id gpio_of_match[] = {
    { .compatible = "brcm,bcm2835-gpio" },   /* Pi 1, 2, 3, Zero */
    { .compatible = "brcm,bcm2711-gpio" },   /* Pi 4 */
    {},
};

static const struct of_device_id pwm_of_match[] = {
    { .compatible = "brcm,bcm2835-pwm" },
    {},
};

static const struct of_device_id clk_of_match[] = {
    { .compatible = "brcm,bcm2835-cprman" },   /* Pi 1, 2, 3, Zero */
    { .compatible = "brcm,bcm2711-cprman" },   /* Pi 4 */
    {},
};

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


static int map_peripherals(void)
{
    struct device_node *node;
    struct resource res;

    /* GPIO */
    node = of_find_matching_node(NULL, gpio_of_match);
    if (!node) {
        pr_err("GPIO node not found in device tree\n");
        return -ENXIO;
    }
    if (of_address_to_resource(node, 0, &res)) {
        of_node_put(node);
        return -ENXIO;
    }
    gpio_base = ioremap(res.start, resource_size(&res));
    of_node_put(node);
    if (!gpio_base)
        return -ENXIO;
    pr_info("GPIO mapped: %pR\n", &res);

    /* PWM */
    node = of_find_matching_node(NULL, pwm_of_match);
    if (!node) {
        pr_err("PWM node not found in device tree\n");
        goto err_gpio;
    }
    if (of_address_to_resource(node, 0, &res)) {
        of_node_put(node);
        goto err_gpio;
    }
    pwm_base = ioremap(res.start, resource_size(&res));
    of_node_put(node);
    if (!pwm_base)
        goto err_gpio;
    pr_info("PWM mapped: %pR\n", &res);

    /* Clock manager */
    node = of_find_matching_node(NULL, clk_of_match);
    if (!node) {
        pr_err("Clock manager node not found in device tree\n");
        goto err_pwm;
    }
    if (of_address_to_resource(node, 0, &res)) {
        of_node_put(node);
        goto err_pwm;
    }
    clk_base = ioremap(res.start, resource_size(&res));
    of_node_put(node);
    if (!clk_base)
        goto err_pwm;
    pr_info("Clock manager mapped: %pR\n", &res);

    gpio_set_reg = (volatile u32 *)(gpio_base + GPIO_SET0);
    gpio_clr_reg = (volatile u32 *)(gpio_base + GPIO_CLR0);
    pwm_ctl_reg = (volatile u32 *)(pwm_base + PWM_CTL);
    pwm_sta_reg = (volatile u32 *)(pwm_base + PWM_STA);
    pwm_rng1_reg = (volatile u32 *)(pwm_base + PWM_RNG1);
    pwm_fifo_reg = (volatile u32 *)(pwm_base + PWM_FIFO);

    return 0;

err_pwm:
    iounmap(pwm_base);
    pwm_base = NULL;
err_gpio:
    iounmap(gpio_base);
    gpio_base = NULL;
    return -ENXIO;
}


static void unmap_peripherals(void)
{
    if (clk_base)
        iounmap(clk_base);
    if (pwm_base)
        iounmap(pwm_base);
    if (gpio_base)
        iounmap(gpio_base);
}


static inline void gpio_set_bits(u32 mask)
{
    *gpio_set_reg = mask;
}


static inline void gpio_clr_bits(u32 mask)
{
    *gpio_clr_reg = mask;
}


static inline void gpio_write_masked_bits(u32 value, u32 mask)
{
    *gpio_clr_reg = ~value & mask;
    *gpio_set_reg = value & mask;
}


static void gpio_set_alt(int pin, int alt)
{
    int reg = pin / 10;
    int shift = (pin % 10) * 3;
    u32 val = readl(gpio_base + GPIO_FSEL0 + reg * 4);
    val &= ~(0x7 << shift);
    val |= (alt << shift);
    writel(val, gpio_base + GPIO_FSEL0 + reg * 4);
}


static void pwm_init_hw(void)
{
    /* Stop PWM */
    writel(PWM_CTL_CLRF1, pwm_base + PWM_CTL);
    udelay(10);

    /* Kill the clock */
    writel(CM_PASSWORD | (1 << 5), clk_base + CM_PWMCTL);  /* KILL bit */
    udelay(10);
    while (readl(clk_base + CM_PWMCTL) & 0x80)  /* wait for BUSY to clear */
        udelay(1);

    /*
     * Source = PLLD (500 MHz on all Pi models).
     * CLK_SRC_PLLD = 6.
     * Divider: pick something that gives useful resolution.
     * hzeller computes this from the base nanosecond timing,
     * but a divider of 2 gives 250 MHz = 4 ns per tick,
     * which is plenty fine for OE pulses in the microsecond range.
     */
    writel(CM_PASSWORD | (PWM_CLK_DIVIDER << 12), clk_base + CM_PWMDIV);
    writel(CM_PASSWORD | (1 << 4) | 6, clk_base + CM_PWMCTL);  /* ENAB | SRC=PLLD */
    udelay(10);
    while (!(readl(clk_base + CM_PWMCTL) & 0x80))  /* wait for BUSY */
        udelay(1);

    /* Set GPIO 18 to ALT5 (PWM0) */
    gpio_set_alt(gpio_oe, GPIO_FSEL_ALT5);

    /*
     * Configure PWM:
     * - USEF1: use FIFO instead of DAT register
     * - POLA1: invert polarity (so silence = OE high = display off)
     * - CLRF1: clear the FIFO
     * SBIT1 stays 0, so when FIFO is empty the output is low,
     * but POLA1 inverts it to high → OE inactive.
     */
    writel(PWM_CTL_USEF1 | PWM_CTL_POLA1 | PWM_CTL_CLRF1,
           pwm_base + PWM_CTL);
}


/*
 * Fire a one-shot OE pulse.
 *
 * We write words into the FIFO — each word is consumed over one PWM period
 * (RNG1 ticks). A non-zero word means OE active for that many ticks within
 * the period. After the real data, two zero "sentinel" words ensure the
 * FIFO drains cleanly and OE returns to inactive.
 *
 * For short pulses (low bit planes), one FIFO word is enough.
 * For longer pulses, we split across multiple FIFO words to keep
 * RNG1 small — this matters because after the pulse, the PWM must
 * run through one full "zero" period (RNG1 ticks of silence),
 * and we don't want that dead time to be huge.
 */
static void pwm_send_pulse(int plane)
{
    u32 range = (base_ticks * brightness) / 100;
    u32 total;

    if (range < 1)
        range = 1;

    total = range << plane;

    if (total <= 16) {
        *pwm_rng1_reg = total; 
        *pwm_fifo_reg = total;
    } else {
        u32 chunk = total / 8;
        int i;

        if (chunk < 1)
            chunk = 1;

        *pwm_rng1_reg = chunk;
        for (i = 0; i < 8; i++)
            *pwm_fifo_reg = chunk;
    }

    *pwm_fifo_reg = 0;
    *pwm_fifo_reg = 0;

    *pwm_ctl_reg = PWM_CTL_USEF1 | PWM_CTL_PWEN1 | PWM_CTL_POLA1;
}


static int pwm_wait_pulse_done(void)
{
    u32 max_ticks = (base_ticks << MAX_BIT_PLANES) * 10;
    ktime_t deadline = ktime_add_ns(ktime_get(), (u64)max_ticks * NS_PER_TICK);

    while (!(*pwm_sta_reg & PWM_STA_EMPT1)) {
        if (ktime_after(ktime_get(), deadline)) {
            pr_warn("PWM pulse timeout\n");
            break;
        }
    }

    *pwm_ctl_reg = PWM_CTL_USEF1 | PWM_CTL_POLA1 | PWM_CTL_CLRF1;

    if (ktime_after(ktime_get(), deadline))
        return -ETIMEDOUT;
    return 0;
}


static void pwm_cleanup(void)
{
    writel(0, pwm_base + PWM_CTL);
    gpio_set_alt(gpio_oe, 0x01);
    gpio_set_bits(BIT(gpio_oe));
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
    gpio_set_alt(gpio_r1, 0x01);
    gpio_set_alt(gpio_g1, 0x01);
    gpio_set_alt(gpio_b1, 0x01);
    gpio_set_alt(gpio_r2, 0x01);
    gpio_set_alt(gpio_g2, 0x01);
    gpio_set_alt(gpio_b2, 0x01);
    gpio_set_alt(gpio_a_addr, 0x01);
    gpio_set_alt(gpio_b_addr, 0x01);
    gpio_set_alt(gpio_c_addr, 0x01);
    gpio_set_alt(gpio_d_addr, 0x01);
    gpio_set_alt(gpio_e_addr, 0x01);
    gpio_set_alt(gpio_clk, 0x01);
    gpio_set_alt(gpio_lat, 0x01);
    gpio_set_alt(gpio_oe, 0x01);

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
static void compute_set_masks(u32 *masks_buffer)
{
    u32 width = f_info->var.xres;
    u32 stride = f_info->fix.line_length / 4;
    u32 r_off = f_info->var.red.offset;
    u32 g_off = f_info->var.green.offset;
    u32 b_off = f_info->var.blue.offset;
    u32 yoff = f_info->var.yoffset;
    u32 xoff = f_info->var.xoffset;
    u32 *pixels = (u32 *)f_info->screen_buffer;
    const u8 *gamma = gamma_tables[gamma_preset < NUM_GAMMA_PRESETS ? gamma_preset : 0];
    int plane, row, col;

    for (plane = 0; plane < MAX_BIT_PLANES; plane++) {
        u8 bit = 1 << plane;

        for (row = 0; row < scan_rows; row++) {
            u32 *row_top = &pixels[(yoff + row) * stride + xoff];
            u32 *row_bot = &pixels[(yoff + row + scan_rows) * stride + xoff];

            for (col = 0; col < width; col++) {
                u32 pxl_top = row_top[col];
                u32 pxl_bot = row_bot[col];

                u32 gt = (gamma[(pxl_top >> r_off) & 0xFF] << r_off) |
                         (gamma[(pxl_top >> g_off) & 0xFF] << g_off) |
                         (gamma[(pxl_top >> b_off) & 0xFF] << b_off);

                u32 gb = (gamma[(pxl_bot >> r_off) & 0xFF] << r_off) |
                         (gamma[(pxl_bot >> g_off) & 0xFF] << g_off) |
                         (gamma[(pxl_bot >> b_off) & 0xFF] << b_off);

                u8 idx = lut_index(gt, gb, bit, r_off, g_off, b_off);

                masks_buffer[(plane * (scan_rows * width)) + (row * width) + col] = color_lut[idx];
            }
        }
    }
}


static int scan_fn(void *data)
{
    u32 width = f_info->var.xres;
    u32 data_mask = BIT(gpio_r1) | BIT(gpio_g1) | BIT(gpio_b1) |
                    BIT(gpio_r2) | BIT(gpio_g2) | BIT(gpio_b2);
    u32 color_clk_mask = data_mask | BIT(gpio_clk);
    int plane, row, col;
    u32 start_time_us, elapsed_us, target_frame_us;
    int first_iter;
    u32 *scan_buf;

    while (!kthread_should_stop()) {

        target_frame_us = 1000000 / refresh_rate;
        start_time_us = ktime_to_us(ktime_get());

        scan_buf = front_masks_buf;
        first_iter = 1;

        for (plane = 0; plane < MAX_BIT_PLANES; plane++) {
            for (row = 0; row < scan_rows; row++) {
                u32 *row_data = &scan_buf[
                    plane * scan_rows * width + row * width];

                for (col = 0; col < width; col++) {
                    gpio_write_masked_bits(row_data[col], color_clk_mask);
                    gpio_set_bits(BIT(gpio_clk));
                }

                if (!first_iter) {
                    if (pwm_wait_pulse_done())
                        goto frame_done;
                }
                first_iter = 0;

                latch_pulse();
                set_address(row);
                pwm_send_pulse(plane);
            }
        }

        pwm_wait_pulse_done();

frame_done:
        /* Sleep most of remainder, busy-wait the tail for precision */
        elapsed_us = ktime_to_us(ktime_get()) - start_time_us;
        if (elapsed_us < target_frame_us) {
            u32 remaining_us = target_frame_us - elapsed_us;
            if (remaining_us > 200)
                usleep_range(remaining_us - 200, remaining_us - 100);

            do {
                elapsed_us = ktime_to_us(ktime_get()) - start_time_us;
            } while (elapsed_us < target_frame_us);
        }
    }
    return 0;
}


static int compute_fn(void *data)
{
    while (!kthread_should_stop()) {
        u32 sleep_us = 1000000 / refresh_rate;
        compute_set_masks(back_masks_buf);
        swap(front_masks_buf, back_masks_buf);
        usleep_range(sleep_us, sleep_us + 100);
    }
    return 0;
}


int pp_renderer_init(struct fb_info *info)
{
    int ret;
    size_t masks_size;

    f_info = info;
    assign_pins();

    ret = map_peripherals();
    if (ret)
        return ret;

    configure_gpio_outputs();
    pwm_init_hw();
    build_color_set_mask_lut();
    build_addr_lut();

    scan_rows = f_info->var.yres / 2;

    if (base_ticks == 0) {
        u64 frame_ns = 1000000000ULL / refresh_rate;
        u32 width = f_info->var.xres;
        u32 plane_sum = (1 << MAX_BIT_PLANES) - 1;

        u64 per_row_budget = frame_ns / ((u64)scan_rows * NS_PER_TICK);
        u64 clocking_overhead = (u64)width * 4 * MAX_BIT_PLANES;

        if (per_row_budget > clocking_overhead)
            base_ticks = (u32)((per_row_budget - clocking_overhead) / plane_sum);
        else
            base_ticks = 1;

        base_ticks = (base_ticks * 70) / 100;
        if (base_ticks < 1)
            base_ticks = 1;
    }

    pr_info("base_ticks=%u, refresh_rate=%u\n", base_ticks, refresh_rate);

    masks_size = MAX_BIT_PLANES * scan_rows * f_info->var.xres * sizeof(u32);
    front_masks_buf = vmalloc(masks_size);
    back_masks_buf = vmalloc(masks_size);
    if (!front_masks_buf || !back_masks_buf) {
        vfree(front_masks_buf);
        vfree(back_masks_buf);
        pwm_cleanup();
        unmap_peripherals();
        return -ENOMEM;
    }

    /* Compute initial frame so display has data immediately */
    compute_set_masks(front_masks_buf);
    memcpy(back_masks_buf, front_masks_buf, masks_size);

    return 0;
}


void pp_renderer_start(void)
{
    refresh_thread = kthread_create(scan_fn, NULL, "pp_scan");
    if (IS_ERR(refresh_thread)) {
        pr_err("failed to create scan thread\n");
        refresh_thread = NULL;
        return;
    }

    if (num_online_cpus() > PP_REFRESH_CPU &&
        !housekeeping_test_cpu(PP_REFRESH_CPU, HK_TYPE_DOMAIN)) {
        kthread_bind(refresh_thread, PP_REFRESH_CPU);
        sched_set_fifo(refresh_thread);
        pr_info("scan thread pinned to isolated core %d\n", PP_REFRESH_CPU);
    }

    wake_up_process(refresh_thread);

    compute_thread = kthread_run(compute_fn, NULL, "pp_compute");
    if (IS_ERR(compute_thread)) {
        pr_err("failed to create compute thread\n");
        compute_thread = NULL;
    }
}


void pp_renderer_stop(void)
{
    if (compute_thread) {
        kthread_stop(compute_thread);
        compute_thread = NULL;
    }
    if (refresh_thread) {
        kthread_stop(refresh_thread);
        refresh_thread = NULL;
    }
    if (pwm_base)
        pwm_cleanup();
    if (gpio_base)
        gpio_set_bits(BIT(gpio_oe));
}


void pp_renderer_remove(void)
{
    unmap_peripherals();
    vfree(front_masks_buf);
    vfree(back_masks_buf);
}
