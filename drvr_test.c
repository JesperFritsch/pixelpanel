/* fbtest.c — framebuffer ghosting test tool */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <time.h>

static uint32_t *fb;
static int width, height;

static void clear(void)
{
    memset(fb, 0, width * height * 4);
}

static void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= 0 && x < width && y >= 0 && y < height)
        fb[y * width + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
}

static void fill_color(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t c = (0xFF << 24) | (r << 16) | (g << 8) | b;
    int i;
    for (i = 0; i < width * height; i++)
        fb[i] = c;
}

static void random_pixels(int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int x = rand() % width;
        int y = rand() % height;
        uint8_t r = rand() % 256;
        uint8_t g = rand() % 256;
        uint8_t b = rand() % 256;
        set_pixel(x, y, r, g, b);
    }
}

static void single_row(int y, uint8_t r, uint8_t g, uint8_t b)
{
    int x;
    clear();
    for (x = 0; x < width; x++)
        set_pixel(x, y, r, g, b);
}

static void checkerboard(int tile, uint8_t r, uint8_t g, uint8_t b)
{
    int x, y;
    clear();
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            if (((x / tile) + (y / tile)) % 2 == 0)
                set_pixel(x, y, r, g, b);
}

static void usage(const char *prog)
{
    printf("Usage: %s [command]\n\n", prog);
    printf("Commands:\n");
    printf("  clear                     — black screen\n");
    printf("  random N                  — N random colored pixels on black\n");
    printf("  fill R G B                — fill with color\n");
    printf("  row Y R G B               — single bright row\n");
    printf("  checker TILE R G B        — checkerboard pattern\n");
    printf("  blink ROW R G B DELAY_MS  — blink a single row on/off\n");
    printf("  gradient                  — horizontal white gradient\n");
    printf("  interactive               — loop: type commands, 'q' to quit\n");
}

int main(int argc, char *argv[])
{
    int fd;
    struct fb_var_screeninfo vinfo;
    size_t size;

    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("ioctl FBIOGET_VSCREENINFO");
        close(fd);
        return 1;
    }

    width = vinfo.xres;
    height = vinfo.yres;
    size = width * height * 4;

    printf("Framebuffer: %dx%d @ %dbpp\n", width, height, vinfo.bits_per_pixel);

    fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    srand(time(NULL));

    if (argc < 2) {
        usage(argv[0]);
        goto done;
    }

    if (strcmp(argv[1], "clear") == 0) {
        clear();

    } else if (strcmp(argv[1], "random") == 0) {
        int n = argc > 2 ? atoi(argv[2]) : 100;
        clear();
        random_pixels(n);

    } else if (strcmp(argv[1], "fill") == 0) {
        uint8_t r = argc > 2 ? atoi(argv[2]) : 255;
        uint8_t g = argc > 3 ? atoi(argv[3]) : 255;
        uint8_t b = argc > 4 ? atoi(argv[4]) : 255;
        fill_color(r, g, b);

    } else if (strcmp(argv[1], "row") == 0) {
        int y = argc > 2 ? atoi(argv[2]) : 0;
        uint8_t r = argc > 3 ? atoi(argv[3]) : 255;
        uint8_t g = argc > 4 ? atoi(argv[4]) : 255;
        uint8_t b = argc > 5 ? atoi(argv[5]) : 255;
        single_row(y, r, g, b);

    } else if (strcmp(argv[1], "checker") == 0) {
        int tile = argc > 2 ? atoi(argv[2]) : 4;
        uint8_t r = argc > 3 ? atoi(argv[3]) : 255;
        uint8_t g = argc > 4 ? atoi(argv[4]) : 255;
        uint8_t b = argc > 5 ? atoi(argv[5]) : 255;
        checkerboard(tile, r, g, b);

    } else if (strcmp(argv[1], "blink") == 0) {
        int y = argc > 2 ? atoi(argv[2]) : 0;
        uint8_t r = argc > 3 ? atoi(argv[3]) : 255;
        uint8_t g = argc > 4 ? atoi(argv[4]) : 255;
        uint8_t b = argc > 5 ? atoi(argv[5]) : 255;
        int delay_ms = argc > 6 ? atoi(argv[6]) : 500;
        printf("Blinking row %d, Ctrl+C to stop\n", y);
        while (1) {
            single_row(y, r, g, b);
            usleep(delay_ms * 1000);
            clear();
            usleep(delay_ms * 1000);
        }

    } else if (strcmp(argv[1], "gradient") == 0) {
        int x, y;
        clear();
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++) {
                uint8_t v = (x * 255) / (width - 1);
                set_pixel(x, y, v, v, v);
            }

    } else if (strcmp(argv[1], "interactive") == 0) {
        char line[256];
        printf("Interactive mode. Commands: clear, random N, fill R G B, row Y R G B, gradient, q\n");
        while (1) {
            printf("> ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin))
                break;
            if (line[0] == 'q')
                break;

            int a1, a2, a3, a4;
            if (strncmp(line, "clear", 5) == 0) {
                clear();
            } else if (sscanf(line, "random %d", &a1) == 1) {
                clear();
                random_pixels(a1);
            } else if (sscanf(line, "fill %d %d %d", &a1, &a2, &a3) == 3) {
                fill_color(a1, a2, a3);
            } else if (sscanf(line, "row %d %d %d %d", &a1, &a2, &a3, &a4) == 4) {
                single_row(a1, a2, a3, a4);
            } else if (strncmp(line, "gradient", 8) == 0) {
                int x2, y2;
                clear();
                for (y2 = 0; y2 < height; y2++)
                    for (x2 = 0; x2 < width; x2++) {
                        uint8_t v = (x2 * 255) / (width - 1);
                        set_pixel(x2, y2, v, v, v);
                    }
            } else {
                printf("Unknown command\n");
            }
        }
    } else {
        usage(argv[0]);
    }

done:
    munmap(fb, size);
    close(fd);
    return 0;
}
