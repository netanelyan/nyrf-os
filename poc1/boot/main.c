/* nyrf OS bootloader - stages 0 to 5 of the boot sequence.
 *
 * Flow (section 3.3 of the research document):
 *
 *   efi_main -> disable watchdog -> init UART -> locate GOP -> read memory map
 *   -> find RSDP -> open kernel.elf on the ESP -> load PT_LOAD segments
 *   -> refresh memory map -> ExitBootServices -> jump to the kernel
 *
 * Nothing after ExitBootServices may touch a boot service, print through
 * ConOut, or allocate. Everything the kernel needs is collected before that
 * line and handed over in a single boot_info_t.
 */

#include "../include/uefi.h"
#include "../include/bootinfo.h"
#include "elf.h"
#include "serial.h"

#define KERNEL_PATH ((CHAR16 *)u"\\kernel.elf")

/* The kernel is compiled for the System V ABI, this file for the Microsoft
 * one. Saying so explicitly makes the compiler place boot_info in RDI. */
typedef void __attribute__((sysv_abi)) (*kernel_entry_t)(boot_info_t *);

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;

/* -- freestanding runtime ------------------------------------------------- */
/* GCC may lower struct assignments and array initialisation into calls to
 * these even under -ffreestanding, and there is no libc to link against. */

void *memset(void *dest, int value, size_t count)
{
    unsigned char *d = dest;
    while (count-- > 0) {
        *d++ = (unsigned char)value;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t count)
{
    unsigned char       *d = dest;
    const unsigned char *s = src;
    while (count-- > 0) {
        *d++ = *s++;
    }
    return dest;
}

/* -- helpers -------------------------------------------------------------- */

static void con_print(const char *s)
{
    /* ConOut wants UTF-16, and only exists before ExitBootServices. */
    CHAR16 buf[128];
    UINTN  i = 0;

    while (*s != '\0' && i < 126) {
        if (*s == '\n') {
            buf[i++] = '\r';
        }
        buf[i++] = (CHAR16)(unsigned char)*s++;
    }
    buf[i] = 0;

    if (ST->ConOut != NULL) {
        ST->ConOut->OutputString(ST->ConOut, buf);
    }
}

static BOOLEAN guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if (a->Data4[i] != b->Data4[i]) {
            return 0;
        }
    }
    return 1;
}

/* Stops with a readable message instead of falling off the end of the world.
 * Recovery test of the POC protocol: a corrupt kernel.elf has to land here. */
static void fatal(const char *what, EFI_STATUS status)
{
    serial_printf("[boot]  FATAL: %s (status=%x)\n", what, (UINT64)status);
    con_print("nyrf: FATAL: ");
    con_print(what);
    con_print("\n");
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

/* -- stage 2: graphics output --------------------------------------------- */

static EFI_GRAPHICS_OUTPUT_PROTOCOL *locate_gop(void)
{
    EFI_GUID                      gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;

    EFI_STATUS status = BS->LocateProtocol(&gop_guid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
        fatal("no Graphics Output Protocol", status);
    }
    return gop;
}

static uint32_t normalise_pixel_format(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info)
{
    switch (info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        return NYRF_PIXEL_RGBX;
    case PixelBlueGreenRedReserved8BitPerColor:
        return NYRF_PIXEL_BGRX;
    case PixelBitMask:
        /* Accept the two masks that happen to match our two layouts. */
        if (info->PixelInformation.RedMask == 0x000000FF) {
            return NYRF_PIXEL_RGBX;
        }
        if (info->PixelInformation.RedMask == 0x00FF0000) {
            return NYRF_PIXEL_BGRX;
        }
        serial_puts("[boot]  unsupported PixelBitMask, assuming BGRX\n");
        return NYRF_PIXEL_BGRX;
    default:
        /* PixelBltOnly means there is no linear framebuffer at all. */
        fatal("framebuffer is Blt-only, cannot draw directly", EFI_UNSUPPORTED);
        return NYRF_PIXEL_BGRX;
    }
}

/* -- stage 3: memory map --------------------------------------------------- */

typedef struct {
    EFI_MEMORY_DESCRIPTOR *map;
    UINTN                  size;
    UINTN                  key;
    UINTN                  desc_size;
    UINT32                 desc_version;
    UINTN                  capacity;
} memory_map_t;

/* Fills mm->map, allocating on the first call. AllocatePool itself changes the
 * map, so the buffer is deliberately oversized and reused on later calls. */
static EFI_STATUS get_memory_map(memory_map_t *mm)
{
    if (mm->map == NULL) {
        UINTN      probe = 0;
        EFI_STATUS status = BS->GetMemoryMap(&probe, NULL, &mm->key,
                                             &mm->desc_size, &mm->desc_version);
        if (status != EFI_BUFFER_TOO_SMALL) {
            return status;
        }

        /* Room for eight more descriptors than the firmware asked for. */
        mm->capacity = probe + 8 * mm->desc_size;
        status = BS->AllocatePool(EfiLoaderData, mm->capacity, (VOID **)&mm->map);
        if (EFI_ERROR(status)) {
            return status;
        }
    }

    mm->size = mm->capacity;
    return BS->GetMemoryMap(&mm->size, mm->map, &mm->key,
                            &mm->desc_size, &mm->desc_version);
}

static void report_memory_map(const memory_map_t *mm)
{
    UINTN  entries = mm->size / mm->desc_size;
    UINT64 free_pages = 0;
    UINT64 total_pages = 0;

    for (UINTN i = 0; i < entries; i++) {
        /* Descriptors are desc_size apart, which is not sizeof the struct. */
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)((const UINT8 *)mm->map + i * mm->desc_size);

        total_pages += d->NumberOfPages;
        if (d->Type == EfiConventionalMemory || d->Type == EfiBootServicesCode ||
            d->Type == EfiBootServicesData || d->Type == EfiLoaderCode) {
            free_pages += d->NumberOfPages;
        }
    }

    serial_printf("[boot]  memory map: %u descriptors, desc_size=%u\n",
                  (UINT64)entries, (UINT64)mm->desc_size);
    serial_printf("[boot]  mapped %u MiB, usable after boot services %u MiB\n",
                  (total_pages * EFI_PAGE_SIZE) >> 20,
                  (free_pages * EFI_PAGE_SIZE) >> 20);
}

/* -- stage 4: ACPI root pointer ------------------------------------------- */

static UINT64 find_rsdp(void)
{
    EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10 = EFI_ACPI_10_TABLE_GUID;
    UINT64   fallback = 0;

    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (guid_equal(&t->VendorGuid, &acpi20)) {
            return (UINT64)(UINTN)t->VendorTable; /* prefer ACPI 2.0+ */
        }
        if (guid_equal(&t->VendorGuid, &acpi10)) {
            fallback = (UINT64)(UINTN)t->VendorTable;
        }
    }
    return fallback;
}

/* -- stage 5: reading the kernel off the ESP ------------------------------ */

static EFI_FILE_PROTOCOL *open_kernel(EFI_HANDLE image_handle)
{
    EFI_GUID   li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID   fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_STATUS status;

    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    status = BS->HandleProtocol(image_handle, &li_guid, (VOID **)&li);
    if (EFI_ERROR(status)) {
        fatal("HandleProtocol(LoadedImage)", status);
    }

    /* DeviceHandle is the volume we were loaded from - the ESP. */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    status = BS->HandleProtocol(li->DeviceHandle, &fs_guid, (VOID **)&fs);
    if (EFI_ERROR(status)) {
        fatal("HandleProtocol(SimpleFileSystem)", status);
    }

    EFI_FILE_PROTOCOL *root = NULL;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) {
        fatal("OpenVolume", status);
    }

    EFI_FILE_PROTOCOL *kernel = NULL;
    status = root->Open(root, &kernel, KERNEL_PATH, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        fatal("kernel.elf not found on the ESP", status);
    }

    root->Close(root);
    return kernel;
}

/* -- entry point ---------------------------------------------------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    ST = system_table;
    BS = system_table->BootServices;

    /* The firmware resets the machine after five minutes unless the watchdog
     * is disarmed. Stability test of the POC depends on this line. */
    BS->SetWatchdogTimer(0, 0, 0, NULL);

    serial_init();
    serial_puts("\n[boot]  nyrf OS bootloader, POC 1\n");
    con_print("nyrf OS bootloader, POC 1\n");

    EFI_GUID                   li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *self = NULL;
    if (!EFI_ERROR(BS->HandleProtocol(image_handle, &li_guid, (VOID **)&self))) {
        serial_printf("[boot]  ImageBase=%x size=%x\n",
                      (UINT64)(UINTN)self->ImageBase, self->ImageSize);
    }

    /* Stage 2: framebuffer. */
    EFI_GRAPHICS_OUTPUT_PROTOCOL         *gop  = locate_gop();
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    uint32_t                              fmt  = normalise_pixel_format(info);

    serial_printf("[boot]  GOP base=%x size=%x\n",
                  (UINT64)gop->Mode->FrameBufferBase, (UINT64)gop->Mode->FrameBufferSize);
    serial_printf("[boot]  %ux%u stride=%u format=%s\n",
                  (UINT64)info->HorizontalResolution, (UINT64)info->VerticalResolution,
                  (UINT64)info->PixelsPerScanLine, fmt == NYRF_PIXEL_RGBX ? "RGBX" : "BGRX");

    /* Sanity check from goal 2: the buffer has to hold stride * height pixels. */
    UINT64 expected = (UINT64)info->PixelsPerScanLine * info->VerticalResolution * 4;
    if (gop->Mode->FrameBufferSize < expected) {
        serial_printf("[boot]  WARNING: framebuffer smaller than stride*height*4 (%x)\n", expected);
    }

    /* Stage 3: memory map. */
    memory_map_t mm = {0};
    EFI_STATUS   status = get_memory_map(&mm);
    if (EFI_ERROR(status)) {
        fatal("GetMemoryMap", status);
    }
    report_memory_map(&mm);

    UINT64 rsdp = find_rsdp();
    serial_printf("[boot]  RSDP=%x\n", rsdp);

    /* Stage 4: load the kernel. */
    EFI_FILE_PROTOCOL *kernel_file = open_kernel(image_handle);
    UINT64             kernel_entry = 0;

    status = elf_load_kernel(BS, kernel_file, &kernel_entry);
    kernel_file->Close(kernel_file);
    if (EFI_ERROR(status)) {
        fatal("could not load kernel.elf", status);
    }
    serial_printf("[boot]  kernel entry=%x\n", kernel_entry);

    /* BootInfo lives in its own EfiLoaderData page so that it stays valid once
     * boot services memory is reclaimed. */
    EFI_PHYSICAL_ADDRESS bi_page = 0;
    status = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &bi_page);
    if (EFI_ERROR(status)) {
        fatal("AllocatePages(BootInfo)", status);
    }

    boot_info_t *bi = (boot_info_t *)(UINTN)bi_page;
    bi->magic        = NYRF_BOOTINFO_MAGIC;
    bi->fb_base      = (uint64_t)gop->Mode->FrameBufferBase;
    bi->fb_size      = (uint64_t)gop->Mode->FrameBufferSize;
    bi->width        = info->HorizontalResolution;
    bi->height       = info->VerticalResolution;
    bi->stride       = info->PixelsPerScanLine;
    bi->pixel_format = fmt;
    bi->rsdp         = rsdp;

    /* Stage 5: leave boot services. The map key goes stale whenever anything
     * allocates, so a refresh-and-retry loop is required, not defensive. */
    serial_puts("[boot]  calling ExitBootServices\n");

    for (int attempt = 1; attempt <= 3; attempt++) {
        status = get_memory_map(&mm);
        if (EFI_ERROR(status)) {
            fatal("GetMemoryMap before exit", status);
        }

        bi->mmap_ptr  = (uint64_t)(UINTN)mm.map;
        bi->mmap_size = (uint64_t)mm.size;
        bi->desc_size = (uint64_t)mm.desc_size;

        status = BS->ExitBootServices(image_handle, mm.key);
        if (!EFI_ERROR(status)) {
            serial_printf("[boot]  ExitBootServices OK on attempt %u\n", (UINT64)attempt);
            break;
        }
        serial_printf("[boot]  ExitBootServices attempt %u failed (%x), refreshing map\n",
                      (UINT64)attempt, (UINT64)status);
    }

    if (EFI_ERROR(status)) {
        /* Boot services are still alive here, so fatal() may still print. */
        fatal("ExitBootServices failed three times", status);
    }

    /* No firmware from this point on: no ConOut, no allocation, no return. */
    ST = NULL;
    BS = NULL;

    serial_puts("[boot]  jumping to kernel\n");
    ((kernel_entry_t)(UINTN)kernel_entry)(bi);

    /* The kernel never returns. If it does, stop quietly. */
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
