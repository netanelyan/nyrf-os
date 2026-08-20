/* ELF64 parsing and PT_LOAD placement - milestone M3 of the POC. */
#ifndef NYRF_ELF_H
#define NYRF_ELF_H

#include "../include/uefi.h"

/* Reads the kernel from an already-open EFI_FILE_PROTOCOL handle, validates
 * the ELF64 header, allocates every PT_LOAD segment at its physical address
 * and copies it in. On success *entry holds e_entry.
 *
 * Every allocation goes through BootServices->AllocatePages: the POC reads the
 * memory map but deliberately does not allocate from it (section 3.2). */
EFI_STATUS elf_load_kernel(EFI_BOOT_SERVICES *bs,
                           EFI_FILE_PROTOCOL *file,
                           UINT64            *entry);

#endif /* NYRF_ELF_H */
