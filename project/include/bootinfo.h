/* The single interface between the two halves of the POC.
 *
 * Everything the firmware can tell us has to be collected before
 * ExitBootServices and handed over in one structure - afterwards there is no
 * way to ask again. See section 3.3 of the research document.
 *
 * This header is compiled twice with two different ABIs: the bootloader uses
 * the Microsoft x64 convention, the kernel uses System V. Only the layout has
 * to agree, which is why every field is a fixed-width type and the structure
 * is packed.
 *
 * That last point is the thing to understand about this file. Normally a
 * shared header is safe because both sides are built the same way. Here they
 * are not: two compiler invocations, two targets, two calling conventions, two
 * object formats. Nothing but the byte layout is common ground - so the layout
 * is nailed down explicitly and then asserted, rather than assumed.
 *
 * It is also the reason there are no pointers-to-structs and no enums here.
 * A pointer is fine (it is just a 64-bit number both sides agree on), but a
 * struct behind it would have to have its layout agreed too, and an enum's
 * width is a compiler choice. Hence mmap_ptr is a uint64_t and not an
 * EFI_MEMORY_DESCRIPTOR *: the kernel never includes uefi.h at all.
 */
#ifndef NYRF_BOOTINFO_H
#define NYRF_BOOTINFO_H

#include <stdint.h>
#include <stddef.h>

/* "NYRFOS01" read as little-endian ASCII. The kernel refuses to draw if this
 * does not match, which turns a wrong pointer into a clean stop instead of a
 * random pattern on screen.
 *
 * Reading it back: the bytes are 4E 59 52 46 4F 53 30 31 in memory, and
 * little-endian means the first byte is the least significant - so the
 * constant is written in the reverse order, 0x3130534F4652594E. It shows up
 * the right way round in a hex dump, which is where you would be looking. */
#define NYRF_BOOTINFO_MAGIC 0x3130534F4652594EULL

/* Normalised pixel layout, so the kernel never has to include uefi.h.
 * The bootloader collapses the firmware's several PixelFormat values into
 * exactly these two; kernel/main.c's pack() knows no others. */
#define NYRF_PIXEL_RGBX 0 /* byte 0 = red,  byte 1 = green, byte 2 = blue */
#define NYRF_PIXEL_BGRX 1 /* byte 0 = blue, byte 1 = green, byte 2 = red  */

typedef struct {
    /* Checked first by the kernel; see NYRF_BOOTINFO_MAGIC above. Kept at
     * offset 0 so that a wrong pointer is detected by reading the very first
     * bytes it points at. */
    uint64_t magic;

    /* Framebuffer, from the Graphics Output Protocol.
     *
     * fb_base is a physical address of video memory, which is why the kernel
     * can still draw with the firmware gone: writing there needs no call to
     * anyone. width/height are what is visible, stride is what a row actually
     * costs - see the warning on stride below. */
    uint64_t fb_base;      /* GOP->Mode->FrameBufferBase                    */
    uint64_t fb_size;      /* GOP->Mode->FrameBufferSize, in bytes          */
    uint32_t width;        /* HorizontalResolution: visible pixels per row  */
    uint32_t height;       /* VerticalResolution: number of rows            */
    /* Not the same as width, and the difference is invisible on most
     * hardware - which is exactly what makes it dangerous. Row n starts at
     * n * stride pixels, never n * width. */
    uint32_t stride;       /* PixelsPerScanLine, padding included           */
    uint32_t pixel_format; /* one of NYRF_PIXEL_*                           */

    /* Memory map, kept for the future physical memory manager.
     *
     * Collected but unused: nothing allocates in POC 1. It is here because
     * this is the last moment it can be obtained at all, and the memory
     * manager is the next roadmap item after descriptor tables.
     *
     * The buffer these point at is EfiLoaderData, so it survives the firmware
     * being torn down. */
    uint64_t mmap_ptr;     /* array of EFI_MEMORY_DESCRIPTOR                */
    uint64_t mmap_size;    /* size of that array in bytes                   */
    /* Travels with the map because the firmware may use a descriptor larger
     * than the published struct. Entry count is mmap_size / desc_size, and
     * walking is in desc_size steps - sizeof would be silently wrong. */
    uint64_t desc_size;    /* size of one descriptor; never use sizeof here */

    /* ACPI root pointer, kept for the future APIC timer. Zero if the firmware
     * published neither ACPI table GUID. Same reasoning as the memory map:
     * unused today, unobtainable later. */
    uint64_t rsdp;
} __attribute__((packed)) boot_info_t;

/* The two halves are built by two compiler invocations with two different
 * ABIs. These assertions fire in both of them, so any future edit that moves a
 * field breaks the build instead of producing a garbled screen.
 *
 * _Static_assert is compile-time, so this costs nothing at runtime and cannot
 * be skipped. The offsets are spot checks rather than an exhaustive list: one
 * at the start of each group, plus the total size, is enough to catch an
 * insertion or a type change anywhere in the struct.
 *
 * If you add a field, add it at the *end* and update sizeof deliberately -
 * that keeps every existing offset valid. A failure here is not a nuisance to
 * silence; it means the two halves have stopped agreeing on what they are
 * passing each other. */
_Static_assert(sizeof(boot_info_t) == 72, "boot_info_t layout changed");
_Static_assert(offsetof(boot_info_t, fb_base) == 8, "fb_base moved");
_Static_assert(offsetof(boot_info_t, stride) == 32, "stride moved");
_Static_assert(offsetof(boot_info_t, mmap_ptr) == 40, "mmap_ptr moved");
_Static_assert(offsetof(boot_info_t, rsdp) == 64, "rsdp moved");

#endif /* NYRF_BOOTINFO_H */
