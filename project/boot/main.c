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
 *
 * This file is one long straight line on purpose. There is no error recovery
 * beyond fatal(): if any stage fails, no later stage can be meaningful, and a
 * bootloader that limps on is worse than one that stops with a message. The
 * only loop that retries anything is around ExitBootServices, and that is
 * because the firmware's own interface requires it.
 *
 * Read it bottom-up: efi_main at the end is the narrative, and everything
 * above it is a helper for one stage of that narrative.
 */

#include <uefi.h>
#include <bootinfo.h>
#include "elf.h"
#include <kernel/serial.h>

/* The u"" prefix gives a UTF-16 literal, which is what UEFI paths are; the
 * doubled backslash is C escaping, so the actual path is "\kernel.elf" - the
 * root of the volume we were loaded from. The cast is needed because the
 * firmware's Open takes a non-const CHAR16 *. */
#define KERNEL_PATH ((CHAR16 *)u"\\kernel.elf")

/* The kernel is compiled for the System V ABI, this file for the Microsoft
 * one. Saying so explicitly makes the compiler place boot_info in RDI.
 *
 * Without this attribute the call below would pass the pointer in RCX, where
 * the kernel's _start would never look for it - and the failure would be a
 * kernel drawing from a garbage framebuffer pointer, not a crash at the call.
 * This one word is the entire ABI transition between the two halves. */
typedef void __attribute__((sysv_abi)) (*kernel_entry_t)(boot_info_t *);

/* File-scope so that every helper can reach the firmware without threading it
 * through each signature. Deliberately nulled after ExitBootServices, so a
 * stray later call faults instead of working by accident. */
static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;

/* -- freestanding runtime ------------------------------------------------- */
/* GCC may lower struct assignments and array initialisation into calls to
 * these even under -ffreestanding, and there is no libc to link against.
 *
 * In other words these are not called by any code in this project; they exist
 * because the *compiler* may emit references to them by name. The naive byte
 * loops are fine precisely because nothing hot ever reaches them. */

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

/* Mirrors a line of the serial log onto the firmware console, so that someone
 * watching the screen rather than the log sees the boot happen at all.
 *
 * Widens ASCII to UTF-16 by assignment - correct only because every string
 * this is called with is plain ASCII - and expands '\n' to CR LF, which the
 * firmware console requires.
 *
 * The 126 bound leaves room for both the CR LF pair a single '\n' can write
 * and the terminator: worst case is i at 125, two bytes written, terminator at
 * 127, exactly filling buf[128]. Long strings are truncated rather than
 * overflowing, which is the right trade for a debug mirror. */
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

    /* ConOut may legitimately be absent on a headless machine, and that is not
     * a reason to fail the boot - the serial log is the real channel. */
    if (ST->ConOut != NULL) {
        ST->ConOut->OutputString(ST->ConOut, buf);
    }
}

/* Compares two 128-bit protocol identifiers.
 *
 * Field by field rather than memcmp because EFI_GUID is a mixed-endian layout
 * of three integers plus a byte array (see uefi.h), and because memcmp would
 * be a call into the freestanding runtime above for no gain. */
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
 * Recovery test of the POC protocol: a corrupt kernel.elf has to land here.
 *
 * Never returns, and reports on both channels because the two fail
 * independently: the screen may be in a graphics mode with no console, and the
 * serial log may be the only thing a headless run produces.
 *
 * hlt rather than a spin: it parks the core until an interrupt, which with
 * interrupts as they are here means indefinitely, at no power cost. The loop
 * is around it because hlt returns if any interrupt ever does arrive. */
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

/* Asks the firmware for any graphics device at all.
 *
 * LocateProtocol rather than enumerating handles: the POC wants a framebuffer,
 * not a particular GPU, and takes whatever video mode the firmware has already
 * set. The Mode and Mode->Info checks matter as much as the status - a
 * conforming implementation always fills them, but a null there would turn
 * into a null-deref two lines later in the caller. */
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

/* Collapses the firmware's several ways of describing a pixel into the two the
 * kernel knows about, so that kernel/main.c never includes uefi.h and the
 * byte-order decision is made once, here, rather than in the drawing loop. */
static uint32_t normalise_pixel_format(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info)
{
    switch (info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        return NYRF_PIXEL_RGBX;
    case PixelBlueGreenRedReserved8BitPerColor:
        return NYRF_PIXEL_BGRX;
    case PixelBitMask:
        /* Accept the two masks that happen to match our two layouts.
         * Red in the lowest byte is RGBX; red in the third is BGRX. Any other
         * mask is a layout we have no packing code for, so the guess below is
         * logged loudly rather than made silently - BGRX because it is what
         * essentially all PC firmware reports. */
        if (info->PixelInformation.RedMask == 0x000000FF) {
            return NYRF_PIXEL_RGBX;
        }
        if (info->PixelInformation.RedMask == 0x00FF0000) {
            return NYRF_PIXEL_BGRX;
        }
        serial_puts("[boot]  unsupported PixelBitMask, assuming BGRX\n");
        return NYRF_PIXEL_BGRX;
    default:
        /* PixelBltOnly means there is no linear framebuffer at all - drawing
         * would require GOP->Blt, a boot service, which is gone by the time
         * the kernel runs. Unrecoverable by construction, so stop here. */
        fatal("framebuffer is Blt-only, cannot draw directly", EFI_UNSUPPORTED);
        return NYRF_PIXEL_BGRX; /* unreachable; fatal() never returns */
    }
}

/* -- stage 3: memory map --------------------------------------------------- */

/* Everything GetMemoryMap hands back, kept together because ExitBootServices
 * needs the key and any walk of the map needs desc_size.
 *
 * capacity is tracked separately from size because size is overwritten by
 * every call with the number of bytes actually used - so without capacity
 * there would be no record of how big the buffer really is. */
typedef struct {
    EFI_MEMORY_DESCRIPTOR *map;
    UINTN                  size;
    UINTN                  key;
    UINTN                  desc_size;
    UINT32                 desc_version;
    UINTN                  capacity;
} memory_map_t;

/* Fills mm->map, allocating on the first call. AllocatePool itself changes the
 * map, so the buffer is deliberately oversized and reused on later calls.
 *
 * The chicken-and-egg problem this solves: you must allocate a buffer to hold
 * the map, but allocating changes the map, which may make it need a bigger
 * buffer. The standard answer is a probe call with a null buffer - which is
 * *expected* to fail with EFI_BUFFER_TOO_SMALL, the one place in this file
 * where an error return is the success path - then allocate with slack.
 *
 * Called repeatedly from the ExitBootServices loop, which is the reason for
 * reusing the buffer: allocating a fresh one each time would invalidate the
 * very key the retry is trying to obtain. */
static EFI_STATUS get_memory_map(memory_map_t *mm)
{
    if (mm->map == NULL) {
        UINTN      probe = 0;
        EFI_STATUS status = BS->GetMemoryMap(&probe, NULL, &mm->key,
                                             &mm->desc_size, &mm->desc_version);
        if (status != EFI_BUFFER_TOO_SMALL) {
            return status;
        }

        /* Room for eight more descriptors than the firmware asked for.
         * Eight because our own AllocatePool can split at most a couple of
         * regions in practice; the margin costs half a kilobyte and removes a
         * whole class of retry. */
        mm->capacity = probe + 8 * mm->desc_size;
        status = BS->AllocatePool(EfiLoaderData, mm->capacity, (VOID **)&mm->map);
        if (EFI_ERROR(status)) {
            return status;
        }
    }

    /* Reset to the full buffer size on every call: GetMemoryMap reads this as
     * "how much room you have" and writes back "how much I used", so leaving
     * the previous result in place would shrink the buffer each time. */
    mm->size = mm->capacity;
    return BS->GetMemoryMap(&mm->size, mm->map, &mm->key,
                            &mm->desc_size, &mm->desc_version);
}

/* Logs a one-line summary of what the firmware says about memory.
 *
 * Purely diagnostic for the POC - nothing allocates from this map yet - but it
 * is the first proof that the map is being walked correctly. A wrong stride
 * shows up immediately as an absurd total. */
static void report_memory_map(const memory_map_t *mm)
{
    UINTN  entries = mm->size / mm->desc_size;
    UINT64 free_pages = 0;
    UINT64 total_pages = 0;

    for (UINTN i = 0; i < entries; i++) {
        /* Descriptors are desc_size apart, which is not sizeof the struct.
         * Hence the byte-pointer arithmetic instead of mm->map[i] - the
         * firmware is allowed to use a larger descriptor than we declare. */
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)((const UINT8 *)mm->map + i * mm->desc_size);

        total_pages += d->NumberOfPages;
        /* The four types that become available once the firmware is gone:
         * conventional memory is free already, and boot services code/data
         * plus loader code are reclaimable after ExitBootServices. Notably
         * absent is EfiLoaderData - that is what we allocated the kernel and
         * the BootInfo page as, precisely so it is not in this set. */
        if (d->Type == EfiConventionalMemory || d->Type == EfiBootServicesCode ||
            d->Type == EfiBootServicesData || d->Type == EfiLoaderCode) {
            free_pages += d->NumberOfPages;
        }
    }

    serial_printf("[boot]  memory map: %u descriptors, desc_size=%u\n",
                  (UINT64)entries, (UINT64)mm->desc_size);
    /* >> 20 converts bytes to MiB. */
    serial_printf("[boot]  mapped %u MiB, usable after boot services %u MiB\n",
                  (total_pages * EFI_PAGE_SIZE) >> 20,
                  (free_pages * EFI_PAGE_SIZE) >> 20);
}

/* -- stage 4: ACPI root pointer ------------------------------------------- */

/* Finds the ACPI root pointer by scanning the system table's configuration
 * array for the ACPI GUID.
 *
 * This is the UEFI way and it replaces the legacy technique of searching low
 * memory for the "RSD PTR " signature - on a UEFI machine that search is not
 * guaranteed to find anything.
 *
 * Both GUIDs are accepted, 2.0+ preferred, because 2.0 provides an XSDT with
 * 64-bit table addresses while 1.0 only has a 32-bit RSDT. The loop cannot
 * return early on a 1.0 match, since a 2.0 entry may still appear later in the
 * array - hence the fallback variable rather than an immediate return.
 *
 * Returns 0 if neither is present. Nothing uses the value yet; it is collected
 * because the APIC timer on the roadmap will need it and this is the last
 * moment it can be obtained. */
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

/* Opens \kernel.elf on the volume this executable itself came from.
 *
 * The chain is the interesting part, and it is three questions in a row:
 * "what do you know about me?" (LoadedImage on our own handle), "can the
 * volume I came from do filesystems?" (SimpleFileSystem on its DeviceHandle),
 * and then open a path on it. Following the chain rather than enumerating
 * disks is what lets the image boot from a USB stick, a virtual disk or
 * QEMU's vvfat with no change and no hardcoded device. */
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

    /* The root directory handle is not needed once the file is open; the file
     * handle does not depend on it staying open. */
    root->Close(root);
    return kernel;
}

/* -- entry point ---------------------------------------------------------- */

/* Where the firmware starts us. The name is arbitrary as far as UEFI is
 * concerned - it is the linker's -entry:efi_main that makes it the entry
 * point - but EFIAPI is not: the firmware calls with the Microsoft x64
 * convention regardless of what this file is compiled as.
 *
 * image_handle identifies this executable to the firmware, and is needed twice
 * later: to find the volume we came from, and to surrender ourselves at
 * ExitBootServices. */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    ST = system_table;
    BS = system_table->BootServices;

    /* The firmware resets the machine after five minutes unless the watchdog
     * is disarmed. Stability test of the POC depends on this line.
     *
     * First thing in the function because the kernel halts forever on purpose,
     * and a reboot five minutes into a demo would look like a crash. */
    BS->SetWatchdogTimer(0, 0, 0, NULL);

    /* Serial before anything that can fail, so that a failure has somewhere to
     * report itself. Until this line a fault is completely silent. */
    serial_init();
    serial_puts("\n[boot]  nyrf OS bootloader, POC 1\n");
    con_print("nyrf OS bootloader, POC 1\n");

    /* Purely informational: where the firmware chose to relocate us. Worth a
     * line because it is otherwise invisible, and it is the number to compare
     * against when a stray pointer turns out to land inside our own image.
     * Failure is ignored - this is a log line, not a dependency. */
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

    /* Sanity check from goal 2: the buffer has to hold stride * height pixels.
     * A warning rather than fatal because the kernel's drawing loop is bounded
     * by width and height anyway, and a firmware that under-reports the size
     * is more likely to be lying about the size than about the geometry. */
    UINT64 expected = (UINT64)info->PixelsPerScanLine * info->VerticalResolution * 4;
    if (gop->Mode->FrameBufferSize < expected) {
        serial_printf("[boot]  WARNING: framebuffer smaller than stride*height*4 (%x)\n", expected);
    }

    /* Stage 3: memory map. The zero initialiser is what tells get_memory_map
     * this is the first call and the buffer still has to be allocated. */
    memory_map_t mm = {0};
    EFI_STATUS   status = get_memory_map(&mm);
    if (EFI_ERROR(status)) {
        fatal("GetMemoryMap", status);
    }
    report_memory_map(&mm);

    UINT64 rsdp = find_rsdp();
    serial_printf("[boot]  RSDP=%x\n", rsdp);

    /* Stage 4: load the kernel. The file handle is closed as soon as the
     * segments are in memory - nothing later reads from it. */
    EFI_FILE_PROTOCOL *kernel_file = open_kernel(image_handle);
    UINT64             kernel_entry = 0;

    status = elf_load_kernel(BS, kernel_file, &kernel_entry);
    kernel_file->Close(kernel_file);
    if (EFI_ERROR(status)) {
        fatal("could not load kernel.elf", status);
    }
    serial_printf("[boot]  kernel entry=%x\n", kernel_entry);

    /* BootInfo lives in its own EfiLoaderData page so that it stays valid once
     * boot services memory is reclaimed.
     *
     * A whole page for 72 bytes, because a page is the allocation unit that
     * carries a memory type, and the type is the entire point: anything in
     * boot services memory becomes free the instant the kernel starts, and a
     * local or a pool allocation would be exactly that. The kernel would read
     * a struct it is simultaneously allowed to overwrite. */
    EFI_PHYSICAL_ADDRESS bi_page = 0;
    status = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &bi_page);
    if (EFI_ERROR(status)) {
        fatal("AllocatePages(BootInfo)", status);
    }

    /* Double cast: EFI_PHYSICAL_ADDRESS is a fixed 64-bit integer, UINTN is
     * pointer-sized. Going through UINTN is the conversion the compiler will
     * accept without a warning about integer-to-pointer width. */
    boot_info_t *bi = (boot_info_t *)(UINTN)bi_page;
    bi->magic        = NYRF_BOOTINFO_MAGIC;
    bi->fb_base      = (uint64_t)gop->Mode->FrameBufferBase;
    bi->fb_size      = (uint64_t)gop->Mode->FrameBufferSize;
    bi->width        = info->HorizontalResolution;
    bi->height       = info->VerticalResolution;
    bi->stride       = info->PixelsPerScanLine;
    bi->pixel_format = fmt;
    bi->rsdp         = rsdp;
    /* The mmap_* fields are deliberately left for the loop below: the map is
     * about to be re-read, and filling them now would record a buffer state
     * that ExitBootServices is going to invalidate. */

    /* Stage 5: leave boot services. The map key goes stale whenever anything
     * allocates, so a refresh-and-retry loop is required, not defensive.
     *
     * The sequence has to be exactly this, every time round: re-read the map,
     * publish the fresh pointers into BootInfo, then exit with the key that
     * read returned. Anything between the read and the exit that allocates -
     * including a serial_printf, if it ever grew a buffer - would invalidate
     * the key again.
     *
     * Three attempts rather than an unbounded loop: a firmware that cannot
     * settle in three is not racing us, it is broken, and an infinite retry
     * would hang with no message instead of failing with one. */
    serial_puts("[boot]  calling ExitBootServices\n");

    for (int attempt = 1; attempt <= 3; attempt++) {
        status = get_memory_map(&mm);
        if (EFI_ERROR(status)) {
            fatal("GetMemoryMap before exit", status);
        }

        /* The map the kernel inherits: a raw pointer, its length, and the
         * stride to walk it with. desc_size travels along because sizeof on
         * the kernel's side would be just as wrong as it is here. */
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

    /* status still holds the last attempt's result, so this catches the case
     * where the loop ran out rather than broke out. */
    if (EFI_ERROR(status)) {
        /* Boot services are still alive here, so fatal() may still print.
         * That is only true on this path - a failed exit leaves the firmware
         * intact. After a successful exit, con_print would be a jump into
         * memory the firmware no longer owns. */
        fatal("ExitBootServices failed three times", status);
    }

    /* No firmware from this point on: no ConOut, no allocation, no return.
     * Nulling the globals makes that structural rather than a convention -
     * any helper that forgets and calls through BS now faults immediately,
     * at the offending line, instead of corrupting something quietly. */
    ST = NULL;
    BS = NULL;

    /* Serial still works: it is our driver talking to hardware directly, and
     * owes nothing to the firmware. This is the whole reason kernel/dev/
     * serial.c is compiled into this half too. */
    serial_puts("[boot]  jumping to kernel\n");

    /* The handoff. Cast the entry address to the sysv_abi function pointer
     * declared at the top, and call it - which puts bi in RDI and lands on
     * _start in kernel/arch/x86_64/entry.asm.
     *
     * A call rather than a jump, so there is a return address on the stack
     * that nothing will ever use. The stack is the firmware's, which is why
     * _start's first real act is to switch to its own. */
    ((kernel_entry_t)(UINTN)kernel_entry)(bi);

    /* The kernel never returns. If it does, stop quietly - there is no
     * firmware left to report to and nowhere to return to. */
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
