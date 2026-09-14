/* Memory layout of the nyrf OS kernel.
 *
 * The kernel is linked to a fixed physical address and loaded there by our own
 * bootloader, which honours p_paddr. 1 MiB is the classic choice: everything
 * below it is legacy or firmware territory.
 *
 * .bss stays inside the same PT_LOAD segment with memsz > filesz, which is
 * exactly the case boot/elf.c zero-fills.
 *
 * A linker script is needed at all because the default one targets a hosted
 * program: it would link against a C runtime, choose a load address to suit
 * an OS loader, and emit a dynamic segment. None of that applies to something
 * the firmware places in physical memory and jumps to.
 */

/* What kind of file to emit, and for which architecture. Explicit because the
 * EFI half of this build produces PE32+ from the same toolchain, and on
 * Windows the default output format is not ELF at all. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

/* Sets e_entry in the ELF header - the address boot/elf.c reads back out and
 * calls. Without this the linker picks the start of .text and warns. */
ENTRY(_start)

KERNEL_PHYS_BASE = 0x100000;

SECTIONS
{
    /* '.' is the location counter: the address the next output section will be
     * placed at. Assigning to it here is what moves the whole image up to
     * 1 MiB instead of the default base. */
    . = KERNEL_PHYS_BASE;

    /* Each ALIGN(4K) rounds the location counter up to a page boundary, so
     * that no two sections share a page. Nothing enforces permissions yet -
     * there is no MMU configuration of our own - but it keeps the segments
     * separable for when there is, at a cost of a few KiB.
     *
     * Inside each block, *(...) means "input sections with these names, from
     * every input file". The .text.* wildcard catches the per-function
     * sections a compiler emits with -ffunction-sections. */
    .text ALIGN(4K) : {
        /* First, and therefore at exactly KERNEL_PHYS_BASE - see entry.asm. */
        *(.text.entry)
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : {
        *(.data .data.*)
    }

    /* The symbols around .bss are defined for code to reference: '.' evaluates
     * to the current address, so __bss_start and __bss_end bracket the region.
     * Nothing uses them yet - boot/elf.c zeroes this memory from the ELF
     * headers instead - but a kernel that ever loads itself, or wants to
     * re-zero after a warm restart, needs exactly these two.
     *
     * COMMON is where the linker puts tentative definitions. Listing it here
     * keeps stray globals inside .bss rather than trailing off the end. */
    .bss ALIGN(4K) : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    /* The first free address above the kernel. The physical memory manager on
     * the roadmap will want this: it marks where the kernel image stops and
     * allocatable memory can begin. */
    __kernel_end = .;

    /* Nothing here is needed at runtime, and dropping it keeps the image and
     * the PT_LOAD list small enough to read at a glance in the log.
     *
     * .eh_frame is the unwind data exceptions and backtraces use, and there is
     * neither here. .note and .comment are toolchain metadata. Discarding is
     * safe precisely because nothing in a freestanding kernel reads them - but
     * note that dropping .eh_frame is what makes a debugger unable to unwind
     * past a fault, which is a trade worth remembering if GDB ever stops
     * producing useful backtraces. */
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
