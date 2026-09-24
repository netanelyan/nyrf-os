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
 *
 * What is worth noticing is how little this file is allowed to assume. There
 * is no firmware to call, no memory it did not receive an address for, no libc
 * and no allocator. Every number it works from arrived in the struct.
 */

#include <bootinfo.h>
#include <kernel/serial.h>

#include <stddef.h>
#include <kernel/gdt.h>     /* next to the serial.h include */
#include <kernel/idt.h>
#include <kernel/pmm.h>

/* Deliberate fault, to prove the exception path actually runs.
 *
 *   0   off - normal boot, draws the test pattern and halts
 *   1   divide by zero              -> #DE, vector 0,  no error code
 *   2   write to an unmapped address -> #PF, vector 14, error code and CR2
 *
 * Not a vector number, because 0 is a real vector and would collide with
 * "off". The two cases are chosen to exercise both stub shapes - one pushes a
 * dummy error code, the other does not - which is the part most likely to be
 * wrong and the hardest to spot by reading.
 *
 * Case 2 is not the obvious null-pointer write, because that would probably
 * not fault: UEFI leaves paging on with low memory identity-mapped, page zero
 * included, so *(uint32_t *)0 = x quietly succeeds on most firmware and would
 * look like a handler that never ran. An address far above any real mapping is
 * unambiguous.
 *
 * Overridable from the build, so proving it does not mean editing the file:
 *   make run-fat KERNEL_CFLAGS_EXTRA=-DNYRF_TEST_FAULT=2
 */
#ifndef NYRF_TEST_FAULT
#define NYRF_TEST_FAULT 0
#endif

/* Packs one pixel for whatever byte order the firmware reported.
 *
 * The framebuffer is written 32 bits at a time, so the question is which byte
 * of that word each channel lands in. Both layouts put green in the middle and
 * leave the top byte unused; they differ only in whether red or blue occupies
 * the low byte.
 *
 * Reading the RGBX branch: the name is the *byte* order in memory, and x86 is
 * little-endian, so byte 0 is the low 8 bits of the word. Red therefore goes
 * unshifted and blue at bit 16 - which looks backwards against the name until
 * you remember the endianness. BGRX is the same statement with r and b
 * exchanged.
 *
 * Branchless and inline because it runs once per pixel: at 1024x768 that is
 * three quarters of a million calls, and this is the innermost thing in the
 * only loop the POC has. */
static inline uint32_t pack(uint32_t format, uint8_t r, uint8_t g, uint8_t b)
{
    return (format == NYRF_PIXEL_RGBX)
               ? ((uint32_t)b << 16) | ((uint32_t)g << 8) | r
               : ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Called from _start with bi already in RDI. Never returns. */
void kernel_main(boot_info_t *bi)
{
    /* Re-initialised rather than inherited: the kernel cannot assume the
     * bootloader left the UART in any particular state, and serial_init is
     * cheap and idempotent. */
    serial_init();
    serial_puts("\n[kern]  nyrf OS kernel reached\n");

    /* Roadmap step 1: replace the firmware's GDT with our own, so that the
     * segment registers point at descriptors this kernel owns rather than at
     * ones sitting in memory the kernel is free to reclaim. Precondition for
     * an IDT, and through that for every interrupt.
     *
     * Placed before the BootInfo check on purpose: it depends on nothing in
     * bi, and it is worth doing even on the path where the kernel is about to
     * refuse to draw - a fault in that state should land somewhere defined. */
    gdt_init();             /* first thing after the kernel announces itself */

    /* Roadmap step 2: our own interrupt table, so that a fault becomes a
     * register dump on the serial line instead of a triple fault and a silent
     * reset. Immediately after the GDT because it depends on it twice - gates
     * name a code selector from it, and the TSS descriptor lives in it.
     *
     * Interrupts stay masked. This installs handlers for the 32 CPU
     * exceptions; hardware IRQs need the APIC and come later. */
    idt_init();

#if NYRF_TEST_FAULT == 1
    /* The div has to be written in asm. In C, dividing by zero is undefined
     * behaviour, so the compiler may assume the divisor is never zero - and
     * clang does: even through volatile, it turns 1 / x into a compare and a
     * cmov (1 / x is 1 for x == 1, -1 for x == -1, else 0) and emits no
     * divide instruction at all. The kernel then carries on and the handler
     * never runs. */
    serial_puts("[kern]  provoking a divide by zero\n");
    __asm__ __volatile__("xorl %%ecx, %%ecx\n\t"
                         "movl $1, %%eax\n\t"
                         "xorl %%edx, %%edx\n\t"
                         "divl %%ecx"
                         ::: "eax", "ecx", "edx");
#elif NYRF_TEST_FAULT == 2
    /* 2^46: canonical, so the CPU will try to translate it rather than
     * rejecting the address outright as a #GP, and far beyond anything the
     * firmware mapped - so the translation fails and we get the #PF we want,
     * with this address in CR2. */
    serial_puts("[kern]  provoking a write to an unmapped address\n");
    *(volatile uint32_t *)0x0000400000000000ULL = 0xDEADBEEF;
#endif

    /* The first thing worth proving about the handoff, and the reason magic
     * exists at all: that the pointer in RDI really is our struct. Everything
     * after this line treats bi as authoritative, so a wrong pointer caught
     * here is a clean stop, while one caught later is a wild write into video
     * memory - or into something that is not video memory. */
    if (bi == NULL || bi->magic != NYRF_BOOTINFO_MAGIC) {
        serial_puts("[kern]  bad BootInfo magic, refusing to draw\n");
        for (;;) {
            __asm__ __volatile__("hlt");
        }
    }

    /* Roadmap step 3: take ownership of RAM. After the magic check rather
     * than with the descriptor tables above, because unlike them it reads bi
     * - and it trusts mmap_ptr enough to write a bitmap wherever the map says
     * there is free memory.
     *
     * The self-test runs on every boot: three allocations and three frees,
     * well under a millisecond, and make check then covers the allocator for
     * free. */
    pmm_init(bi);
    pmm_self_test();

    serial_printf("[kern]  fb=%x %ux%u stride=%u\n",
                  bi->fb_base, (uint64_t)bi->width,
                  (uint64_t)bi->height, (uint64_t)bi->stride);

    /* volatile because these writes have no reader the compiler can see: it
     * would be entitled to notice that nothing ever loads from fb and delete
     * the entire loop. volatile says the writes are the point.
     *
     * The rest are copied into locals out of the packed struct. Reading a
     * packed member is not free - the compiler cannot assume alignment - and
     * these are read in the innermost loop. */
    volatile uint32_t *fb     = (volatile uint32_t *)(uintptr_t)bi->fb_base;
    const uint32_t     fmt    = bi->pixel_format;
    const uint32_t     width  = bi->width;
    const uint32_t     height = bi->height;
    const uint32_t     stride = bi->stride;
    const uint32_t     bar    = width / 3;
    const uint32_t     white  = pack(fmt, 255, 255, 255);

    for (uint32_t y = 0; y < height; y++) {
        /* Row start uses stride, never width - that is the trap the frame
         * around the screen is there to expose.
         *
         * The cast to uint64_t before multiplying matters: at large
         * resolutions y * stride overflows 32 bits well before the bottom of
         * the screen, and the result would wrap to a row near the top. */
        volatile uint32_t *row = fb + (uint64_t)y * stride;

        for (uint32_t x = 0; x < width; x++) {
            /* Which third of the screen this pixel is in, and how far into
             * that third. Integer division, so the last bar absorbs the
             * remainder when width is not a multiple of three - channel can
             * come out as 3 for the final pixels, which the default branch
             * below folds into blue.
             *
             * The bar != 0 guards are for a hypothetical screen under three
             * pixels wide. Not a real machine, but the division would be by
             * zero and this is the only place in the kernel that could
             * fault. */
            uint32_t channel = (bar != 0) ? (x / bar) : 0;
            uint32_t offset  = (bar != 0) ? (x % bar) : x;

            /* Dark to bright inside each bar, so banding is visible too.
             *
             * 32 to 255 across the width of a bar: offset/bar is the fraction
             * of the way across, times 223 is the range, plus 32 is the floor.
             * The floor is deliberate - starting at 0 would make the left edge
             * of each bar indistinguishable from a black screen, and a black
             * screen is exactly the failure this pattern has to be
             * distinguishable from. */
            uint8_t level = (uint8_t)(32 + (offset * 223) / (bar != 0 ? bar : width));

            switch (channel) {
            case 0:  row[x] = pack(fmt, level, 0, 0); break;
            case 1:  row[x] = pack(fmt, 0, level, 0); break;
            default: row[x] = pack(fmt, 0, 0, level); break;
            }
        }

        /* The frame, drawn over the bars rather than around them. Top and
         * bottom rows are solid white; every other row gets just its two end
         * pixels.
         *
         * Because each row is drawn immediately after being filled, this needs
         * no second pass over the screen. */
        if (y == 0 || y == height - 1) {
            for (uint32_t x = 0; x < width; x++) {
                row[x] = white;
            }
        } else {
            row[0]         = white;
            row[width - 1] = white;
        }
    }

    /* The string make check greps for. Ten runs have to reach this line. */
    serial_puts("[kern]  test pattern drawn, halting\n");

    /* Halt forever. Interrupts are still masked from _start and there is no
     * IDT, so there is nothing to wake up for - and nothing this kernel could
     * usefully do next. The loop is around hlt because an NMI would resume
     * execution past it. */
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
