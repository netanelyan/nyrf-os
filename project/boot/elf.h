/* ELF64 parsing and PT_LOAD placement - milestone M3 of the POC.
 *
 * The entire interface is one function, because loading the kernel is one
 * transaction: either every segment is in place and there is an address to
 * jump to, or the boot is over. There is nothing useful to expose in between,
 * so the ELF structures themselves stay private to elf.c.
 *
 * boot-side only. The kernel never parses ELF - it *is* the ELF.
 */
#ifndef NYRF_ELF_H
#define NYRF_ELF_H

#include <uefi.h>

/* Reads the kernel from an already-open EFI_FILE_PROTOCOL handle, validates
 * the ELF64 header, allocates every PT_LOAD segment at its physical address
 * and copies it in. On success *entry holds e_entry.
 *
 * Every allocation goes through BootServices->AllocatePages: the POC reads the
 * memory map but deliberately does not allocate from it (section 3.2).
 *
 * bs is passed in rather than taken from a global so that this file needs
 * nothing from main.c - the dependency runs one way only.
 *
 * The caller keeps ownership of the file handle and closes it. On failure
 * *entry is untouched and any pages already allocated are left as they are:
 * the only path out of a failed load is fatal(), so there is nothing to
 * unwind for. */
EFI_STATUS elf_load_kernel(EFI_BOOT_SERVICES *bs,
                           EFI_FILE_PROTOCOL *file,
                           UINT64            *entry);

#endif /* NYRF_ELF_H */
