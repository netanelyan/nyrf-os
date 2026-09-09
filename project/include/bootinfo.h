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
 */
#ifndef NYRF_BOOTINFO_H
#define NYRF_BOOTINFO_H

#include <stdint.h>
#include <stddef.h>

/* "NYRFOS01" read as little-endian ASCII. The kernel refuses to draw if this
 * does not match, which turns a wrong pointer into a clean stop instead of a
 * random pattern on screen. */
#define NYRF_BOOTINFO_MAGIC 0x3130534F4652594EULL

/* Normalised pixel layout, so the kernel never has to include uefi.h. */
#define NYRF_PIXEL_RGBX 0 /* byte 0 = red,  byte 1 = green, byte 2 = blue */
#define NYRF_PIXEL_BGRX 1 /* byte 0 = blue, byte 1 = green, byte 2 = red  */

typedef struct {
    uint64_t magic;

    /* Framebuffer, from the Graphics Output Protocol. */
    uint64_t fb_base;      /* GOP->Mode->FrameBufferBase                    */
    uint64_t fb_size;      /* GOP->Mode->FrameBufferSize, in bytes          */
    uint32_t width;        /* HorizontalResolution: visible pixels per row  */
    uint32_t height;       /* VerticalResolution: number of rows            */
    uint32_t stride;       /* PixelsPerScanLine, padding included           */
    uint32_t pixel_format; /* one of NYRF_PIXEL_*                           */

    /* Memory map, kept for the future physical memory manager. */
    uint64_t mmap_ptr;     /* array of EFI_MEMORY_DESCRIPTOR                */
    uint64_t mmap_size;    /* size of that array in bytes                   */
    uint64_t desc_size;    /* size of one descriptor; never use sizeof here */

    /* ACPI root pointer, kept for the future APIC timer. */
    uint64_t rsdp;
} __attribute__((packed)) boot_info_t;

/* The two halves are built by two compiler invocations with two different
 * ABIs. These assertions fire in both of them, so any future edit that moves a
 * field breaks the build instead of producing a garbled screen. */
_Static_assert(sizeof(boot_info_t) == 72, "boot_info_t layout changed");
_Static_assert(offsetof(boot_info_t, fb_base) == 8, "fb_base moved");
_Static_assert(offsetof(boot_info_t, stride) == 32, "stride moved");
_Static_assert(offsetof(boot_info_t, mmap_ptr) == 40, "mmap_ptr moved");
_Static_assert(offsetof(boot_info_t, rsdp) == 64, "rsdp moved");

#endif /* NYRF_BOOTINFO_H */
