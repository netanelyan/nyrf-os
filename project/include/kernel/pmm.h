/* Physical memory manager - which 4 KiB frames of RAM are free.
 *
 * Roadmap step 3. Everything above it - page tables, a heap, stacks for
 * tasks - needs somewhere to put its memory, and this is the only layer that
 * knows where the RAM actually is. It learns that from one source: the UEFI
 * memory map the bootloader collected before ExitBootServices and passed in
 * boot_info_t.
 *
 * The design is a bitmap, one bit per 4 KiB page, 1 = in use. That is the
 * simplest structure that can answer "is this page free?" in constant time,
 * and at 512 MiB it costs 16 KiB. The price is that allocation is a scan; see
 * pmm_alloc_page() for why that is acceptable for now and what replaces it.
 *
 * This hands out *physical* addresses. They are only usable as pointers
 * because the firmware left paging on with RAM identity-mapped, and the
 * kernel has not replaced those page tables yet. Once it does (roadmap
 * step 4), a physical address and a pointer stop being the same thing.
 */
#ifndef NYRF_PMM_H
#define NYRF_PMM_H

#include <bootinfo.h>

#include <stdint.h>

/* The granularity of everything here, and not a choice: it is the smallest
 * page x86_64 paging can map, and the unit the UEFI map counts in. */
#define PMM_PAGE_SIZE  4096ULL
#define PMM_PAGE_SHIFT 12

/* Builds the bitmap from bi's memory map. Must run once, before any other
 * pmm_ call, and after the BootInfo magic has been checked - it trusts
 * mmap_ptr completely. Halts with a message if there is nowhere to put the
 * bitmap. */
void pmm_init(const boot_info_t *bi);

/* One page, 4 KiB aligned, contents undefined. Returns its physical address,
 * or 0 when memory is exhausted.
 *
 * 0 can double as the failure value because page 0 is never handed out: the
 * whole first MiB is reserved in pmm_init(). */
uint64_t pmm_alloc_page(void);

/* Returns a page from pmm_alloc_page(). Halts on an unaligned address, one
 * outside tracked memory, or a page that is already free - a double free is
 * a bug that corrupts memory silently later, so it is stopped where it
 * happens. */
void pmm_free_page(uint64_t phys);

/* Counts in pages, for logging and for the self-test. */
uint64_t pmm_free_count(void);
uint64_t pmm_tracked_count(void);

/* Allocates, writes, frees and checks the free count comes back, logging
 * each step. Halts on any mismatch. Cheap enough to run on every boot. */
void pmm_self_test(void);

#endif /* NYRF_PMM_H */
