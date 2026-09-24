/* Physical memory manager - bitmap over the UEFI memory map. See
 * include/kernel/pmm.h for the interface and why a bitmap. */

#include <kernel/pmm.h>
#include <kernel/serial.h>

#include <stddef.h>

/* One entry of the UEFI memory map, declared here because the kernel never
 * includes uefi.h (see bootinfo.h). This is EFI_MEMORY_DESCRIPTOR, field for
 * field, from the UEFI 2.10 spec section 7.2.
 *
 * Not packed, on purpose: the spec layout has 4 bytes of padding after type,
 * which natural alignment reproduces. The asserts pin that down.
 *
 * Never index an array of these. The firmware's descriptors are desc_size
 * apart (48 on OVMF), and desc_size is allowed to be larger than this struct -
 * see the walk in for_each_desc(). */
typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t phys_start;
    uint64_t virt_start;
    uint64_t num_pages;
    uint64_t attribute;
} efi_mem_desc_t;

_Static_assert(offsetof(efi_mem_desc_t, phys_start) == 8, "PhysicalStart at 8");
_Static_assert(offsetof(efi_mem_desc_t, num_pages) == 24, "NumberOfPages at 24");
_Static_assert(sizeof(efi_mem_desc_t) == 40, "EFI_MEMORY_DESCRIPTOR is 40 bytes");

/* The EFI_MEMORY_TYPE values this file cares about. The full list is in
 * uefi.h; the numbers are fixed by the spec. */
#define EFI_LOADER_CODE         1
#define EFI_BOOT_SERVICES_CODE  3
#define EFI_BOOT_SERVICES_DATA  4
#define EFI_CONVENTIONAL_MEMORY 7

/* Everything below 1 MiB stays reserved even where the map says it is free.
 *
 * Two reasons. Page 0 must never be handed out, so that 0 can mean "out of
 * memory" and a null pointer never aliases a real allocation. And the low
 * megabyte is the only place an application processor can start: when SMP
 * arrives, its real-mode trampoline needs a page down here, and it is easier
 * to never give them away than to get one back. 256 pages is nothing. */
#define LOW_MEMORY_LIMIT 0x100000ULL

/* The kernel image, from kernel/link.ld. Declared as arrays so that their
 * address is the value - these are symbols with no storage behind them. */
extern char __kernel_start[];
extern char __kernel_end[];

/* The bitmap itself: bit n of the array is page n, 1 = used. Words rather
 * than bytes so that a fully used stretch of 64 pages is skipped with one
 * comparison in pmm_alloc_page().
 *
 * It lives in RAM that pmm_init() finds at runtime, not in .bss, because its
 * size depends on how much memory the machine has. A static array would have
 * to be sized for the largest machine ever supported, and all of it would be
 * zero-filled by the ELF loader on every boot. */
static uint64_t *bitmap;
static uint64_t  bitmap_words;
static uint64_t  page_count;   /* pages the bitmap covers, from address 0 */
static uint64_t  free_pages;

/* Where the last allocation came from. The next search starts here instead
 * of at word 0, so that a long run of used memory at the bottom is not
 * rescanned on every call - next-fit rather than first-fit. */
static uint64_t  next_word;

/* Nothing past pmm_init() can recover from a broken allocator, so each check
 * that fails ends here: say why, on the one channel that works, then stop. */
static void pmm_fatal(const char *why, uint64_t value)
{
    serial_printf("[pmm]   FATAL: %s (%x)\n", why, value);
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/* -- bit operations -------------------------------------------------------
 *
 * The only places that touch bitmap bits, and the only places free_pages
 * changes. Each checks the bit's old value so the count cannot drift: marking
 * an already-used page used is not a second allocation. */

static inline int page_is_used(uint64_t page)
{
    return (bitmap[page / 64] >> (page % 64)) & 1;
}

static void mark_used(uint64_t page)
{
    if (!page_is_used(page)) {
        bitmap[page / 64] |= 1ULL << (page % 64);
        free_pages--;
    }
}

static void mark_free(uint64_t page)
{
    if (page_is_used(page)) {
        bitmap[page / 64] &= ~(1ULL << (page % 64));
        free_pages++;
    }
}

/* Marks every page overlapping [base, base + size) - rounding outwards, so a
 * byte range that half-covers a page reserves all of it. Clamped to the
 * bitmap, because the things reserved here (the kernel, the low megabyte) are
 * not guaranteed to lie inside it on every machine. */
static void reserve_range(uint64_t base, uint64_t size)
{
    uint64_t first = base >> PMM_PAGE_SHIFT;
    uint64_t last  = (base + size + PMM_PAGE_SIZE - 1) >> PMM_PAGE_SHIFT;

    if (last > page_count) {
        last = page_count;
    }
    for (uint64_t p = first; p < last; p++) {
        mark_used(p);
    }
}

/* -- walking the map ------------------------------------------------------ */

static const efi_mem_desc_t *desc_at(const boot_info_t *bi, uint64_t i)
{
    /* Byte arithmetic in desc_size steps, never sizeof - see bootinfo.h. */
    return (const efi_mem_desc_t *)(uintptr_t)(bi->mmap_ptr + i * bi->desc_size);
}

/* Types whose pages the bitmap must cover, because they are free now or can
 * be reclaimed later.
 *
 * Only conventional memory is actually marked free today. The other three
 * become free in principle once ExitBootServices has run, but not yet in
 * practice: boot services data holds the page tables the CPU is still
 * walking, and loader code is the bootloader that is still, technically, our
 * caller's caller. They are reclaimed after the kernel has its own page tables
 * (roadmap step 4). Covering them now means that step needs no bitmap resize.
 *
 * Everything else - ACPI tables, runtime services, MMIO - is never general
 * RAM. Including MMIO would be actively harmful: on this machine the map runs
 * to 13 GiB because of PCI windows, while the RAM is 512 MiB. */
static int may_become_free(uint32_t type)
{
    return type == EFI_CONVENTIONAL_MEMORY || type == EFI_BOOT_SERVICES_CODE ||
           type == EFI_BOOT_SERVICES_DATA  || type == EFI_LOADER_CODE;
}

void pmm_init(const boot_info_t *bi)
{
    const uint64_t entries = bi->mmap_size / bi->desc_size;

    /* Pass 1: how far up does memory worth tracking go? The bitmap starts at
     * address 0 and stops here, so a page number is just an address shifted
     * right - no base to subtract, and no per-region lookup. */
    uint64_t top = 0;
    for (uint64_t i = 0; i < entries; i++) {
        const efi_mem_desc_t *d = desc_at(bi, i);
        if (may_become_free(d->type)) {
            uint64_t end = d->phys_start + d->num_pages * PMM_PAGE_SIZE;
            if (end > top) {
                top = end;
            }
        }
    }

    page_count   = top >> PMM_PAGE_SHIFT;
    bitmap_words = (page_count + 63) / 64;

    const uint64_t bitmap_bytes = bitmap_words * sizeof(uint64_t);

    /* Pass 2: somewhere to keep the bitmap. The first conventional region
     * above the low megabyte that is big enough; the bitmap then reserves its
     * own pages below, like any other allocation.
     *
     * Writing to it is safe only because the firmware's identity map is still
     * in force - this is a physical address used as a pointer. */
    bitmap = NULL;
    for (uint64_t i = 0; i < entries && bitmap == NULL; i++) {
        const efi_mem_desc_t *d = desc_at(bi, i);
        if (d->type == EFI_CONVENTIONAL_MEMORY &&
            d->phys_start >= LOW_MEMORY_LIMIT &&
            d->num_pages * PMM_PAGE_SIZE >= bitmap_bytes) {
            bitmap = (uint64_t *)(uintptr_t)d->phys_start;
        }
    }
    if (bitmap == NULL) {
        pmm_fatal("no conventional region can hold the bitmap", bitmap_bytes);
    }

    /* Start from "everything used" and carve out what is free, not the other
     * way round. A page the map does not mention - a hole, or one of the bits
     * past page_count in the last word - then defaults to never being handed
     * out, which is the only safe default for memory we know nothing about.
     *
     * free_pages starts at 0 to match, and each mark_free below counts up. */
    for (uint64_t w = 0; w < bitmap_words; w++) {
        bitmap[w] = ~0ULL;
    }
    free_pages = 0;

    /* Pass 3: conventional memory is free. The spec guarantees descriptors do
     * not overlap, so no page is counted twice. */
    uint64_t regions = 0;
    for (uint64_t i = 0; i < entries; i++) {
        const efi_mem_desc_t *d = desc_at(bi, i);
        if (d->type == EFI_CONVENTIONAL_MEMORY) {
            uint64_t first = d->phys_start >> PMM_PAGE_SHIFT;
            for (uint64_t p = first; p < first + d->num_pages; p++) {
                mark_free(p);
            }
            regions++;
        }
    }

    /* And the exceptions carved back out of it. The kernel image is
     * EfiLoaderData, so the map already excludes it - reserving it anyway
     * costs nothing and survives a bootloader change that forgets to. The
     * bitmap is not optional: it sits in conventional memory by construction. */
    reserve_range(0, LOW_MEMORY_LIMIT);
    reserve_range((uint64_t)(uintptr_t)__kernel_start,
                  (uint64_t)(uintptr_t)(__kernel_end - __kernel_start));
    reserve_range((uint64_t)(uintptr_t)bitmap, bitmap_bytes);

    next_word = 0;

    serial_printf("[pmm]   bitmap at %x, %u bytes, tracks %u pages (%u MiB)\n",
                  (uint64_t)(uintptr_t)bitmap, bitmap_bytes, page_count,
                  (page_count * PMM_PAGE_SIZE) >> 20);
    serial_printf("[pmm]   %u free regions, %u pages free (%u MiB)\n",
                  regions, free_pages, (free_pages * PMM_PAGE_SIZE) >> 20);
}

/* Next-fit over whole words: a word equal to ~0 is 64 used pages and is
 * skipped in one comparison, and inside a word with a hole, ctz finds the
 * lowest clear bit in one instruction (bsf/tzcnt).
 *
 * The cost is still a scan, worst case the whole bitmap - 2048 words at
 * 512 MiB. For a kernel whose requirement is timing consistency that is the
 * wrong shape long-term: the time an allocation takes depends on the state of
 * memory. It is acceptable here because nothing on a timing-critical path
 * allocates yet. When something does, the answer is a free list or per-size
 * pools refilled outside the critical path, with this bitmap underneath as
 * the source of truth. */
uint64_t pmm_alloc_page(void)
{
    for (uint64_t n = 0; n < bitmap_words; n++) {
        uint64_t w = next_word + n;
        if (w >= bitmap_words) {
            w -= bitmap_words;
        }

        if (bitmap[w] != ~0ULL) {
            uint64_t page = w * 64 + (uint64_t)__builtin_ctzll(~bitmap[w]);

            /* Cannot exceed page_count: the tail bits of the last word were
             * set in pmm_init() and nothing ever clears them. */
            mark_used(page);
            next_word = w;
            return page << PMM_PAGE_SHIFT;
        }
    }
    return 0;
}

void pmm_free_page(uint64_t phys)
{
    const uint64_t page = phys >> PMM_PAGE_SHIFT;

    if (phys & (PMM_PAGE_SIZE - 1)) {
        pmm_fatal("free of an unaligned address", phys);
    }
    if (page >= page_count) {
        pmm_fatal("free of an address outside tracked memory", phys);
    }
    if (!page_is_used(page)) {
        pmm_fatal("double free", phys);
    }

    /* A free page will not be reused before the search wraps around to it
     * otherwise, which is fine for correctness but lets the bitmap fragment.
     * Pulling the cursor back keeps allocations packed low. */
    mark_free(page);
    if (page / 64 < next_word) {
        next_word = page / 64;
    }
}

uint64_t pmm_free_count(void)
{
    return free_pages;
}

uint64_t pmm_tracked_count(void)
{
    return page_count;
}

/* Three pages rather than one, so the test also proves allocations are
 * distinct - a broken mark_used would return the same page three times and
 * pass a single-page test. Writing a pattern to each and reading it back
 * proves the address is real, writable RAM, not merely a plausible number. */
#define SELF_TEST_PAGES 3

void pmm_self_test(void)
{
    const uint64_t before = free_pages;
    uint64_t       pages[SELF_TEST_PAGES];

    for (int i = 0; i < SELF_TEST_PAGES; i++) {
        pages[i] = pmm_alloc_page();

        if (pages[i] == 0) {
            pmm_fatal("self-test: out of memory", (uint64_t)i);
        }
        if (pages[i] & (PMM_PAGE_SIZE - 1)) {
            pmm_fatal("self-test: unaligned page", pages[i]);
        }
        if (pages[i] < LOW_MEMORY_LIMIT) {
            pmm_fatal("self-test: page from the reserved low megabyte", pages[i]);
        }
        for (int j = 0; j < i; j++) {
            if (pages[j] == pages[i]) {
                pmm_fatal("self-test: same page handed out twice", pages[i]);
            }
        }

        /* First and last word of the page, tagged with the address, so a
         * page that aliases another would read back the wrong tag. */
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)pages[i];
        p[0]   = pages[i] ^ 0xA5A5A5A5A5A5A5A5ULL;
        p[511] = pages[i];
    }

    for (int i = 0; i < SELF_TEST_PAGES; i++) {
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)pages[i];
        if (p[0] != (pages[i] ^ 0xA5A5A5A5A5A5A5A5ULL) || p[511] != pages[i]) {
            pmm_fatal("self-test: page did not hold its pattern", pages[i]);
        }
    }

    serial_printf("[pmm]   self-test: allocated %x %x %x\n",
                  pages[0], pages[1], pages[2]);

    for (int i = 0; i < SELF_TEST_PAGES; i++) {
        pmm_free_page(pages[i]);
    }
    if (free_pages != before) {
        pmm_fatal("self-test: free count did not return", free_pages);
    }

    serial_puts("[pmm]   self-test passed\n");
}
