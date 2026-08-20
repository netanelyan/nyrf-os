/* nyrf OS kernel - the whole POC kernel.
 *
 * It receives boot_info_t, draws one static test pattern straight into the
 * framebuffer and halts. No descriptor tables, no paging, no allocator, no
 * scheduler: those all live above a kernel that has already booted, which is
 * precisely what this POC is trying to prove is possible.
 *
 * The pattern is graded on purpose (section 3.4):
 *   - three vertical bars, one per colour channel, each a dark-to-bright ramp.
 *     A swapped RGB/BGR order shows up immediately as red and blue trading
 *     places.
 *   - a one pixel white frame around the screen. If stride were confused with
 *     width, the frame would run diagonally instead of square.
 * A single filled rectangle would have caught neither mistake.
 *
 * Serial logging is linked in as well. After ExitBootServices it is the only
 * output channel left, and without it a black screen cannot be told apart from
 * a kernel that was never reached.
 */

#include "../include/bootinfo.h"
#include "../boot/serial.h"

#include <stddef.h>

/* Packs one pixel for whatever byte order the firmware reported. */
static inline uint32_t pack(uint32_t format, uint8_t r, uint8_t g, uint8_t b)
{
    return (format == NYRF_PIXEL_RGBX)
               ? ((uint32_t)b << 16) | ((uint32_t)g << 8) | r
               : ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void kernel_main(boot_info_t *bi)
{
    serial_init();
    serial_puts("\n[kern]  nyrf OS kernel reached\n");

    if (bi == NULL || bi->magic != NYRF_BOOTINFO_MAGIC) {
        serial_puts("[kern]  bad BootInfo magic, refusing to draw\n");
        for (;;) {
            __asm__ __volatile__("hlt");
        }
    }

    serial_printf("[kern]  fb=%x %ux%u stride=%u\n",
                  bi->fb_base, (uint64_t)bi->width,
                  (uint64_t)bi->height, (uint64_t)bi->stride);

    volatile uint32_t *fb     = (volatile uint32_t *)(uintptr_t)bi->fb_base;
    const uint32_t     fmt    = bi->pixel_format;
    const uint32_t     width  = bi->width;
    const uint32_t     height = bi->height;
    const uint32_t     stride = bi->stride;
    const uint32_t     bar    = width / 3;
    const uint32_t     white  = pack(fmt, 255, 255, 255);

    for (uint32_t y = 0; y < height; y++) {
        /* Row start uses stride, never width - that is the trap the frame
         * around the screen is there to expose. */
        volatile uint32_t *row = fb + (uint64_t)y * stride;

        for (uint32_t x = 0; x < width; x++) {
            uint32_t channel = (bar != 0) ? (x / bar) : 0;
            uint32_t offset  = (bar != 0) ? (x % bar) : x;

            /* Dark to bright inside each bar, so banding is visible too. */
            uint8_t level = (uint8_t)(32 + (offset * 223) / (bar != 0 ? bar : width));

            switch (channel) {
            case 0:  row[x] = pack(fmt, level, 0, 0); break;
            case 1:  row[x] = pack(fmt, 0, level, 0); break;
            default: row[x] = pack(fmt, 0, 0, level); break;
            }
        }

        if (y == 0 || y == height - 1) {
            for (uint32_t x = 0; x < width; x++) {
                row[x] = white;
            }
        } else {
            row[0]         = white;
            row[width - 1] = white;
        }
    }

    serial_puts("[kern]  test pattern drawn, halting\n");

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
