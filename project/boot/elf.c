/* ELF64 loading: read kernel.elf off the ESP and place it in memory.
 *
 * "Loading" an executable is less than it sounds. An ELF file is not an image
 * of memory - it is a description of one. The program header table says "put
 * these bytes at this address, and make the region this big". This file walks
 * that table, allocates each region, copies the bytes in and zeroes the rest.
 * There is no relocation and no dynamic linking, because the kernel is linked
 * ET_EXEC to a fixed address (see kernel/link.ld); the addresses in the file
 * are final.
 *
 * The structures below are transcribed from the ELF64 specification rather
 * than taken from a toolchain's elf.h, for the same reason uefi.h is vendored:
 * no dependency, and the freestanding EFI build has no system headers to
 * include anyway.
 */

#include "elf.h"
#include <kernel/serial.h>

#define EI_NIDENT 16

/* The four magic bytes every ELF file starts with: 0x7F 'E' 'L' 'F'. */
#define ELF_MAG0 0x7F
#define ELF_MAG1 'E'
#define ELF_MAG2 'L'
#define ELF_MAG3 'F'

#define ELFCLASS64  2    /* e_ident[4]: 64-bit                            */
#define ELFDATA2LSB 1    /* e_ident[5]: little-endian                     */
#define ET_EXEC     2    /* e_type: fixed-address executable              */
#define EM_X86_64   0x3E /* e_machine                                     */
#define PT_LOAD     1    /* p_type: a segment that belongs in memory      */

/* The ELF header: the first 64 bytes of the file, describing what kind of
 * object this is and where to find its tables.
 *
 * Only four fields actually matter to us - e_ident and e_type/e_machine to
 * decide whether to trust the file at all, e_entry as the address to jump to,
 * and e_phoff/e_phnum/e_phentsize to locate the program header table. The
 * section headers (e_shoff and friends) are for linkers and debuggers; a
 * loader never reads them.
 *
 * packed because the layout is a file format, not a C struct. Without it the
 * compiler would be free to insert padding and every offset past e_type would
 * be wrong. */
typedef struct {
    UINT8  e_ident[EI_NIDENT]; /* magic, class, endianness, ABI            */
    UINT16 e_type;             /* ET_EXEC, ET_DYN, ...                     */
    UINT16 e_machine;          /* target architecture                      */
    UINT32 e_version;
    UINT64 e_entry;            /* virtual address of the entry point       */
    UINT64 e_phoff;            /* file offset of the program header table  */
    UINT64 e_shoff;            /* file offset of the section header table  */
    UINT32 e_flags;
    UINT16 e_ehsize;           /* size of this header                      */
    UINT16 e_phentsize;        /* size of one program header               */
    UINT16 e_phnum;            /* number of program headers                */
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

/* One program header: a single instruction to the loader.
 *
 * The three fields that do the work are p_offset (where the bytes are in the
 * file), p_paddr (where they go in memory) and the p_filesz/p_memsz pair.
 *
 * filesz and memsz differ whenever a segment ends in .bss: the zeros are not
 * stored in the file, so memsz > filesz and the loader is responsible for
 * zeroing the difference. Getting that wrong gives a kernel whose globals
 * start out as whatever the firmware last left in that memory - a bug that
 * reproduces differently on every machine.
 *
 * p_vaddr and p_paddr are the virtual and physical destinations. They are
 * equal in this kernel, since nothing has set up paging yet. */
typedef struct {
    UINT32 p_type;   /* PT_LOAD, PT_DYNAMIC, PT_NOTE, ...                 */
    UINT32 p_flags;  /* read/write/execute permissions - no MMU yet, so
                      * nothing here can be enforced; recorded for later   */
    UINT64 p_offset; /* where the bytes start in the file                  */
    UINT64 p_vaddr;  /* virtual address they belong at                     */
    UINT64 p_paddr;  /* physical address they belong at                    */
    UINT64 p_filesz; /* how many bytes are in the file                     */
    UINT64 p_memsz;  /* how many bytes the segment occupies in memory      */
    UINT64 p_align;
} __attribute__((packed)) elf64_phdr_t;

/* Reads exactly `size` bytes starting at `offset`. The firmware is allowed to
 * return a short read, so the loop is not optional.
 *
 * EFI_FILE_PROTOCOL has no pread: position is state on the handle, so this
 * seeks and then reads. Read takes its size parameter as in/out - `chunk` goes
 * in as "how much I want" and comes back as "how much you got" - which is why
 * it has to be reassigned on every iteration rather than set once. */
static EFI_STATUS read_at(EFI_FILE_PROTOCOL *file, UINT64 offset, VOID *buffer, UINTN size)
{
    EFI_STATUS status = file->SetPosition(file, offset);
    if (EFI_ERROR(status)) {
        return status;
    }

    UINT8 *out = (UINT8 *)buffer;
    UINTN  done = 0;

    while (done < size) {
        UINTN chunk = size - done;
        status = file->Read(file, &chunk, out + done);
        if (EFI_ERROR(status)) {
            return status;
        }
        /* A zero-length read with no error means end of file. Treated as a
         * failure here because every caller asked for a specific count and a
         * truncated ELF is not something to continue from. */
        if (chunk == 0) {
            return EFI_LOAD_ERROR; /* end of file before we had everything */
        }
        done += chunk;
    }

    return EFI_SUCCESS;
}

/* Rejects anything we cannot load, with one log line per reason.
 *
 * This is the POC's recovery test in practice: feeding the bootloader a
 * corrupt kernel.elf has to produce a named failure here rather than a jump to
 * a garbage address. Each check therefore prints *why* before returning - a
 * single "bad kernel" message would leave nothing to diagnose. */
static BOOLEAN header_is_valid(const elf64_ehdr_t *eh)
{
    if (eh->e_ident[0] != ELF_MAG0 || eh->e_ident[1] != ELF_MAG1 ||
        eh->e_ident[2] != ELF_MAG2 || eh->e_ident[3] != ELF_MAG3) {
        serial_puts("[elf]   bad magic, not an ELF file\n");
        return 0;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) {
        serial_puts("[elf]   not 64-bit little-endian\n");
        return 0;
    }
    /* ET_EXEC specifically, not ET_DYN: a position-independent executable
     * would need its relocations applied, and this loader deliberately does
     * none. The -no-pie in the Makefile is the other half of this check. */
    if (eh->e_type != ET_EXEC) {
        serial_puts("[elf]   not ET_EXEC; the kernel must be linked fixed-address\n");
        return 0;
    }
    if (eh->e_machine != EM_X86_64) {
        serial_puts("[elf]   wrong machine, expected x86_64\n");
        return 0;
    }
    /* e_phentsize is checked against our own struct because the loop below
     * indexes the table as an array of elf64_phdr_t. If a file ever declared a
     * different stride, that indexing would silently walk off the entries -
     * the same desc_size trap the memory map has, caught by rejection here
     * instead of handled. */
    if (eh->e_phnum == 0 || eh->e_phentsize != sizeof(elf64_phdr_t)) {
        serial_puts("[elf]   missing or malformed program header table\n");
        return 0;
    }
    return 1;
}

/* Header, then program header table, then one pass over the PT_LOAD segments.
 * On success *entry holds the address to jump to. */
EFI_STATUS elf_load_kernel(EFI_BOOT_SERVICES *bs, EFI_FILE_PROTOCOL *file, UINT64 *entry)
{
    elf64_ehdr_t eh;

    EFI_STATUS status = read_at(file, 0, &eh, sizeof(eh));
    if (EFI_ERROR(status)) {
        serial_printf("[elf]   cannot read ELF header: %x\n", (UINT64)status);
        return status;
    }
    if (!header_is_valid(&eh)) {
        return EFI_LOAD_ERROR;
    }

    serial_printf("[elf]   ELF64 x86_64 executable, entry=%x, %u program headers\n",
                  eh.e_entry, (UINT64)eh.e_phnum);

    /* The table is read into a pool allocation rather than a local, because
     * e_phnum is the file's choice and a variable-length array on a firmware
     * stack of unknown size is not worth the risk. */
    elf64_phdr_t *phdrs = NULL;
    UINTN         phsize = (UINTN)eh.e_phnum * sizeof(elf64_phdr_t);

    status = bs->AllocatePool(EfiLoaderData, phsize, (VOID **)&phdrs);
    if (EFI_ERROR(status)) {
        serial_printf("[elf]   AllocatePool failed: %x\n", (UINT64)status);
        return status;
    }

    status = read_at(file, eh.e_phoff, phdrs, phsize);
    if (EFI_ERROR(status)) {
        serial_printf("[elf]   cannot read program headers: %x\n", (UINT64)status);
        bs->FreePool(phdrs);
        return status;
    }

    /* Counted so that an ELF with a valid header but nothing to load is
     * rejected below rather than "succeeding" into an empty jump. */
    UINTN loaded = 0;

    for (UINT16 i = 0; i < eh.e_phnum; i++) {
        elf64_phdr_t *ph = &phdrs[i];
        /* Everything that is not PT_LOAD describes the file, not the memory
         * image: notes, the dynamic section, the program header table itself.
         * A loader ignores all of it. */
        if (ph->p_type != PT_LOAD) {
            continue;
        }

        /* p_paddr is what we honour: the kernel is linked to a fixed physical
         * address and the firmware still has everything identity-mapped. */
        EFI_PHYSICAL_ADDRESS dest  = ph->p_paddr;
        /* Round up: AllocatePages works in whole 4 KiB pages, and a segment of
         * one byte still needs one. The +SIZE-1 before the divide is the
         * standard integer ceiling. */
        UINTN                pages = (UINTN)((ph->p_memsz + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE);

        serial_printf("[elf]   PT_LOAD %u -> %x  filesz=%x memsz=%x (%u pages)\n",
                      (UINT64)i, dest, ph->p_filesz, ph->p_memsz, (UINT64)pages);

        /* AllocateAddress means "give me exactly this address or fail" - the
         * demand that makes fixed-address loading work. It fails if the
         * firmware has already put something there, which is the real reason
         * link.ld chooses 1 MiB: low memory is where firmware structures live.
         *
         * dest is in/out for this call, hence the address-of; with
         * AllocateAddress the firmware returns it unchanged. */
        status = bs->AllocatePages(AllocateAddress, EfiLoaderData, pages, &dest);
        if (EFI_ERROR(status)) {
            serial_printf("[elf]   AllocatePages at %x failed: %x\n", ph->p_paddr, (UINT64)status);
            bs->FreePool(phdrs);
            return status;
        }

        /* Guarded because a pure .bss segment has nothing in the file to copy,
         * and asking read_at for zero bytes would still seek. */
        if (ph->p_filesz > 0) {
            status = read_at(file, ph->p_offset, (VOID *)(UINTN)dest, (UINTN)ph->p_filesz);
            if (EFI_ERROR(status)) {
                serial_printf("[elf]   segment read failed: %x\n", (UINT64)status);
                bs->FreePool(phdrs);
                return status;
            }
        }

        /* Everything past p_filesz is .bss and must be zeroed by the loader.
         *
         * A byte loop rather than memset: it is a handful of KiB once, so the
         * speed is irrelevant, and it keeps this file free of any dependency
         * on the freestanding runtime defined over in main.c. */
        UINT8 *bss = (UINT8 *)(UINTN)dest + ph->p_filesz;
        for (UINT64 b = ph->p_filesz; b < ph->p_memsz; b++) {
            *bss++ = 0;
        }

        loaded++;
    }

    /* The table has served its purpose; the segments are in their final
     * places and do not point back into it. */
    bs->FreePool(phdrs);

    if (loaded == 0) {
        serial_puts("[elf]   no PT_LOAD segments found\n");
        return EFI_LOAD_ERROR;
    }

    serial_printf("[elf]   %u segments loaded\n", (UINT64)loaded);
    /* Reported rather than returned as the function's value, because
     * EFI_STATUS already owns that slot. */
    *entry = eh.e_entry;
    return EFI_SUCCESS;
}
