/* memset and memcpy for the kernel.
 *
 * Nothing in the kernel calls these by name. They exist because the compiler
 * may: GCC documents that even with -ffreestanding it can turn a fill or copy
 * loop into a call to memset or memcpy, and the kernel has no libc to supply
 * them. kernel/mm/pmm.c's bitmap fill is exactly such a loop. Without these,
 * the GCC toolchain would fail to link - or, worse, link against nothing and
 * fault.
 *
 * The bootloader has its own copies in boot/main.c; the two halves are built
 * for different ABIs and cannot share an object.
 *
 * Byte loops, deliberately simple. The volatile-free pointers mean the
 * compiler could, in principle, recognise these very loops as memset and
 * memcpy and emit a call to themselves; clang and GCC both suppress that
 * inside a function with the same name, which is what makes this safe. */

#include <stddef.h>

void *memset(void *dest, int value, size_t count)
{
    unsigned char *d = dest;

    while (count--) {
        *d++ = (unsigned char)value;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t count)
{
    unsigned char       *d = dest;
    const unsigned char *s = src;

    while (count--) {
        *d++ = *s++;
    }
    return dest;
}
