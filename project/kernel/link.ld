/* Memory layout of the nyrf OS kernel.
 *
 * The kernel is linked to a fixed physical address and loaded there by our own
 * bootloader, which honours p_paddr. 1 MiB is the classic choice: everything
 * below it is legacy or firmware territory.
 *
 * .bss stays inside the same PT_LOAD segment with memsz > filesz, which is
 * exactly the case boot/elf.c zero-fills.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_PHYS_BASE = 0x100000;

SECTIONS
{
    . = KERNEL_PHYS_BASE;

    .text ALIGN(4K) : {
        *(.text.entry)
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : {
        *(.data .data.*)
    }

    .bss ALIGN(4K) : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    __kernel_end = .;

    /* Nothing here is needed at runtime, and dropping it keeps the image and
     * the PT_LOAD list small enough to read at a glance in the log. */
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
