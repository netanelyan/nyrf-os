#include "elf.h"
#include "serial.h"

#define EI_NIDENT 16

#define ELF_MAG0 0x7F
#define ELF_MAG1 'E'
#define ELF_MAG2 'L'
#define ELF_MAG3 'F'

#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define EM_X86_64   0x3E
#define PT_LOAD     1

typedef struct {
    UINT8  e_ident[EI_NIDENT];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} __attribute__((packed)) elf64_phdr_t;

/* Reads exactly `size` bytes starting at `offset`. The firmware is allowed to
 * return a short read, so the loop is not optional. */
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
        if (chunk == 0) {
            return EFI_LOAD_ERROR; /* end of file before we had everything */
        }
        done += chunk;
    }

    return EFI_SUCCESS;
}

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
    if (eh->e_type != ET_EXEC) {
        serial_puts("[elf]   not ET_EXEC; the kernel must be linked fixed-address\n");
        return 0;
    }
    if (eh->e_machine != EM_X86_64) {
        serial_puts("[elf]   wrong machine, expected x86_64\n");
        return 0;
    }
    if (eh->e_phnum == 0 || eh->e_phentsize != sizeof(elf64_phdr_t)) {
        serial_puts("[elf]   missing or malformed program header table\n");
        return 0;
    }
    return 1;
}

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

    UINTN loaded = 0;

    for (UINT16 i = 0; i < eh.e_phnum; i++) {
        elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) {
            continue;
        }

        /* p_paddr is what we honour: the kernel is linked to a fixed physical
         * address and the firmware still has everything identity-mapped. */
        EFI_PHYSICAL_ADDRESS dest  = ph->p_paddr;
        UINTN                pages = (UINTN)((ph->p_memsz + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE);

        serial_printf("[elf]   PT_LOAD %u -> %x  filesz=%x memsz=%x (%u pages)\n",
                      (UINT64)i, dest, ph->p_filesz, ph->p_memsz, (UINT64)pages);

        status = bs->AllocatePages(AllocateAddress, EfiLoaderData, pages, &dest);
        if (EFI_ERROR(status)) {
            serial_printf("[elf]   AllocatePages at %x failed: %x\n", ph->p_paddr, (UINT64)status);
            bs->FreePool(phdrs);
            return status;
        }

        if (ph->p_filesz > 0) {
            status = read_at(file, ph->p_offset, (VOID *)(UINTN)dest, (UINTN)ph->p_filesz);
            if (EFI_ERROR(status)) {
                serial_printf("[elf]   segment read failed: %x\n", (UINT64)status);
                bs->FreePool(phdrs);
                return status;
            }
        }

        /* Everything past p_filesz is .bss and must be zeroed by the loader. */
        UINT8 *bss = (UINT8 *)(UINTN)dest + ph->p_filesz;
        for (UINT64 b = ph->p_filesz; b < ph->p_memsz; b++) {
            *bss++ = 0;
        }

        loaded++;
    }

    bs->FreePool(phdrs);

    if (loaded == 0) {
        serial_puts("[elf]   no PT_LOAD segments found\n");
        return EFI_LOAD_ERROR;
    }

    serial_printf("[elf]   %u segments loaded\n", (UINT64)loaded);
    *entry = eh.e_entry;
    return EFI_SUCCESS;
}
