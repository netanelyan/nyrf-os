/* Global Descriptor Table.
 *
 * In long mode segmentation is nearly gone: for code and data the base is
 * forced to 0 and the limit is ignored, and paging does the job segmentation
 * used to do. Three things survived, and they are the reason this file exists:
 *
 *   1. CS must point at a valid 64-bit code descriptor. The CPU will not
 *      execute without one.
 *   2. The descriptor carries the privilege level, which is how ring 0 and
 *      ring 3 are expressed in hardware.
 *   3. The TSS lives in the GDT, and the TSS holds the stack pointers the CPU
 *      switches to on an interrupt. That is why the GDT comes before the IDT.
 *
 * Until gdt_init() runs, the kernel is executing on the GDT the UEFI firmware
 * built: memory we intend to reclaim, described by a table we did not write.
 */
#ifndef NYRF_GDT_H
#define NYRF_GDT_H

#include <stdint.h>

/* A segment selector is (index << 3) | TI | RPL.
 *
 *   index  which entry in the table
 *   TI     0 = this is the GDT, 1 = the LDT. We never use an LDT.
 *   RPL    requested privilege level, 0 for kernel.
 *
 * With TI and RPL both 0, the selector is simply the byte offset of the entry,
 * which is why entry 1 is 0x08 and entry 2 is 0x10. */
#define GDT_SEL_NULL        0x00
#define GDT_SEL_KERNEL_CODE 0x08
#define GDT_SEL_KERNEL_DATA 0x10

/* The TSS descriptor is a system descriptor, and those are 16 bytes rather
 * than 8 - so it occupies two consecutive rows, indices 3 and 4. Only the
 * first is ever named by a selector; index 4 exists because the descriptor
 * spills into it, and nothing may be placed there. */
#define GDT_INDEX_TSS       3
#define GDT_SEL_TSS         0x18

/* Builds our table, loads it with lgdt, and reloads every segment register. */
void gdt_init(void);

/* Writes the TSS descriptor into indices 3 and 4.
 *
 * Exposed rather than done inside gdt_init() because the TSS itself belongs to
 * tss.c: this file owns the table, that one owns what the descriptor points
 * at. Safe to call after gdt_init() has already run lgdt - the CPU re-reads
 * the table from memory on each selector load, and the limit set by gdt_init()
 * already covers both rows.
 *
 * limit is the size of the TSS minus one, in bytes. */
void gdt_set_tss(uint64_t base, uint32_t limit);

#endif /* NYRF_GDT_H */
